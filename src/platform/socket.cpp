// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/socket.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "platform/descriptors.hpp"
#include "platform/paths.hpp"

#if defined(__APPLE__)
#include <sys/ucred.h>
#endif

namespace ckm::platform {
namespace {

// A socket that will never raise SIGPIPE on this process (macOS's half of the
// guarantee flush() states; on Linux MSG_NOSIGNAL there does it per write).
void suppress_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

// A stream socket that is close-on-exec from the moment it exists where the
// system offers that, and one call later where it does not. macOS has no
// SOCK_CLOEXEC, so both halves of this are live code.
int make_stream_socket() {
#if defined(SOCK_CLOEXEC)
    return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (set_close_on_exec(fd)) return fd;
    const int failure = errno;
    (void)::close(fd);
    errno = failure;
    return -1;
#endif
}

// Finishes a connect a signal interrupted, and returns the errno it ended
// with — 0 when it succeeded.
//
// POSIX: an interrupted connect is NOT aborted; the connection continues to be
// established asynchronously. Calling `connect` again would be answered with
// EALREADY (or EISCONN once it has completed), so the way to learn how it went
// is to wait for the descriptor to become writable and ask it. Waiting
// indefinitely is what the interrupted call was already doing.
int finish_interrupted_connect(int fd) {
    for (;;) {
        pollfd waiting{};
        waiting.fd = fd;
        waiting.events = POLLOUT;
        if (::poll(&waiting, 1, -1) < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        int failure = 0;
        socklen_t length = sizeof(failure);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &length) != 0) return errno;
        return failure;
    }
}

// Fills a `sockaddr_un`, or returns false when the path cannot be one.
bool address_for(const std::filesystem::path& path, sockaddr_un& address) {
    const std::string text = path.string();
    if (text.empty() || text.size() + 1 > sizeof(address.sun_path)) return false;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
    return true;
}

// Whether the peer on this connection is the user running this process.
//
// Belt and braces: the 0700 directory already stops anybody else from reaching
// the socket. It is worth having both because the directory's mode is a fact
// about the file system at one moment, and a check on the connection is a fact
// about the connection.
bool peer_is_this_user(int fd, std::string& refusal) {
#if defined(__APPLE__)
    xucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &credentials, &length) != 0) {
        refusal = "cannot read the peer's credentials: " + std::string(std::strerror(errno));
        return false;
    }
    if (credentials.cr_version != XUCRED_VERSION) {
        refusal = "the peer's credentials are in a format this build does not know";
        return false;
    }
    const uid_t peer = credentials.cr_uid;
#else
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        refusal = "cannot read the peer's credentials: " + std::string(std::strerror(errno));
        return false;
    }
    const uid_t peer = credentials.uid;
#endif
    if (peer == ::geteuid()) return true;
    refusal = "refused a connection from uid " + std::to_string(static_cast<unsigned long>(peer)) +
              "; this server belongs to uid " +
              std::to_string(static_cast<unsigned long>(::geteuid()));
    return false;
}

}  // namespace

std::filesystem::path socket_path() {
    if (const char* const explicit_path = environment_value("CKMUX_SOCKET"))
        return std::filesystem::path(explicit_path);
    // Both of these name a directory to build a path under, so both have to be
    // absolute: a socket under a relative runtime directory is a different
    // socket for every directory ckmux is started from, which reads to a
    // reader as "my sessions are gone".
    const char* runtime = environment_directory("XDG_RUNTIME_DIR");
    if (runtime == nullptr) runtime = environment_directory("TMPDIR");
    const std::filesystem::path base = runtime != nullptr ? std::filesystem::path(runtime)
                                                          : std::filesystem::path("/tmp");
    const std::string directory = "ckmux-" + std::to_string(static_cast<unsigned long>(::geteuid()));
    return base / directory / "default.sock";
}

bool socket_path_fits(const std::filesystem::path& path) {
    sockaddr_un probe{};
    return address_for(path, probe);
}

bool prepare_socket_directory(const std::filesystem::path& socket, std::string& problem) {
    const std::filesystem::path directory = socket.parent_path();
    if (directory.empty()) {
        problem = "the socket path has no directory: " + socket.string();
        return false;
    }
    // The ancestors are ordinary directories — $TMPDIR, $XDG_RUNTIME_DIR and
    // their parents exist already in every case but a hand-written
    // $CKMUX_SOCKET — and they are made under the process umask like anything
    // else. The LAST component is the one that carries the 0700, so it is
    // created WITH that mode: a mkdir under a permissive umask followed by a
    // chmod leaves a window, however short, in which the directory the socket
    // is about to appear in is one anybody may write to.
    if (directory.has_parent_path()) {
        std::error_code ignored;
        std::filesystem::create_directories(directory.parent_path(), ignored);
    }
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        problem = "cannot create " + directory.string() + ": " + std::string(std::strerror(errno));
        return false;
    }
    // An existing directory is not an error; anything else about it might be —
    // and what it is has to be asked of the thing that will be used, not of a
    // name that may mean something else by the time the socket is bound.
    // O_NOFOLLOW refuses a symbolic link at first, because following one
    // blindly would check the ownership of a target somebody else chose. But a
    // link is only somebody else's choice when somebody else made it: macOS's
    // /tmp is root's link to /private/tmp, and a reader may point their own
    // links wherever they like. So a link owned by root or by this user is
    // followed — the target still has to pass every check below, through the
    // descriptor that will be used — and only a link planted by another user
    // stays refused, which is the one case the follow would launder.
    int handle = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int failure = handle < 0 ? errno : 0;
    if (handle < 0 && (failure == ELOOP || failure == EMLINK || failure == ENOTDIR)) {
        // Which of a link and a plain file the refusal was about, the errno
        // cannot say alone — Darwin answers ENOTDIR for a link met with
        // O_DIRECTORY | O_NOFOLLOW, Linux ELOOP — so lstat is asked.
        struct ::stat link{};
        if (::lstat(directory.c_str(), &link) == 0 && S_ISLNK(link.st_mode)) {
            if (link.st_uid != 0 && link.st_uid != ::geteuid()) {
                problem = directory.string() + " is a symbolic link planted by another user";
                return false;
            }
            handle = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            failure = handle < 0 ? errno : 0;
        }
    }
    if (handle < 0) {
        if (failure == ELOOP || failure == EMLINK)
            problem = directory.string() + " is a loop of symbolic links, not a directory";
        else if (failure == ENOTDIR)
            problem = directory.string() + " exists and is not a directory";
        else
            problem = "cannot open " + directory.string() + ": " +
                      std::string(std::strerror(failure));
        return false;
    }
    struct ::stat info{};
    if (::fstat(handle, &info) != 0) {
        problem = "cannot examine " + directory.string() + ": " +
                  std::string(std::strerror(errno));
        (void)::close(handle);
        return false;
    }
    if (info.st_uid != ::geteuid()) {
        problem = directory.string() + " belongs to another user";
        (void)::close(handle);
        return false;
    }
    // Owner-only, always, and reset rather than merely checked: a directory
    // anybody else can write to is a socket anybody else can replace, and the
    // reader who chmod'd it once by accident should not have to find that out
    // from a security advisory.
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0 && ::fchmod(handle, 0700) != 0) {
        problem = "cannot make " + directory.string() + " owner-only: " +
                  std::string(std::strerror(errno));
        (void)::close(handle);
        return false;
    }
    (void)::close(handle);
    return true;
}

ConnectResult connect_to_server(const std::filesystem::path& path) {
    ConnectResult result;
    sockaddr_un address{};
    if (!address_for(path, address)) {
        result.status = ConnectStatus::Unusable;
        result.problem = "the socket path is too long to be a Unix socket address: " + path.string();
        return result;
    }
    const int fd = make_stream_socket();
    if (fd < 0) {
        result.status = ConnectStatus::Unusable;
        result.problem = "cannot make a socket: " + std::string(std::strerror(errno));
        return result;
    }
    int outcome = 0;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        outcome = errno;
        // A signal is not a failed connect. The request is still in flight, so
        // the answer comes from the descriptor rather than from a second call.
        if (outcome == EINTR) outcome = finish_interrupted_connect(fd);
    }
    if (outcome != 0) {
        const int failure = outcome;
        (void)::close(fd);
        // ENOENT is "no server has ever run here"; ECONNREFUSED is "a socket
        // file is here and nothing is listening", which is what a server that
        // died leaves behind. To a client both mean start one.
        if (failure == ENOENT || failure == ECONNREFUSED) {
            result.status = ConnectStatus::NoServer;
            return result;
        }
        if (failure == EACCES || failure == EPERM) {
            result.status = ConnectStatus::Denied;
            result.problem = "not allowed to use " + path.string();
            return result;
        }
        result.status = ConnectStatus::Unusable;
        result.problem = "cannot connect to " + path.string() + ": " +
                         std::string(std::strerror(failure));
        return result;
    }
    if (!set_non_blocking(fd)) {
        (void)::close(fd);
        result.status = ConnectStatus::Unusable;
        result.problem = "cannot make the connection non-blocking";
        return result;
    }
    result.status = ConnectStatus::Connected;
    suppress_sigpipe(fd);
    result.fd = fd;
    return result;
}

Listener::~Listener() { close(); }

Listener::Status Listener::listen(const std::filesystem::path& path) {
    path_ = path;
    sockaddr_un address{};
    if (!address_for(path, address)) {
        problem_ = "the socket path is too long to be a Unix socket address: " + path.string();
        return Status::Failed;
    }
    if (!prepare_socket_directory(path, problem_)) return Status::Failed;

    // The start lock. Held for the server's whole life, so that a starter which
    // arrives while this one is running always loses it and goes back to
    // connecting instead of unlinking a live server's socket.
    const std::filesystem::path lock_path = std::filesystem::path(path.string() + ".lock");
    lock_fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd_ < 0) {
        problem_ = "cannot open " + lock_path.string() + ": " + std::string(std::strerror(errno));
        return Status::Failed;
    }
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        const int failure = errno;
        (void)::close(lock_fd_);
        lock_fd_ = -1;
        if (failure == EWOULDBLOCK) return Status::Racing;
        problem_ = "cannot lock " + lock_path.string() + ": " + std::string(std::strerror(failure));
        return Status::Failed;
    }

    // Under the lock: is somebody already listening? If so this starter has
    // simply lost, and saying so is what keeps it from unlinking a socket other
    // clients are using.
    const ConnectResult probe = connect_to_server(path);
    if (probe.status == ConnectStatus::Connected) {
        (void)::close(probe.fd);
        close();
        return Status::AlreadyRunning;
    }

    // Whatever is at the path now is not a server. Unlinking it is the
    // stale-socket recovery of the architecture spec.
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    fd_ = make_stream_socket();
    if (fd_ < 0) {
        problem_ = "cannot make a socket: " + std::string(std::strerror(errno));
        close();
        return Status::Failed;
    }
    // The socket is created with the process umask applied, so the mode is set
    // afterwards rather than hoped for. Safe to do by name here and nowhere
    // else: the directory it sits in was just established as owner-only and
    // owned by this user, so there is nobody who could put something else at
    // this path between the bind and the chmod.
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        problem_ = "cannot bind " + path.string() + ": " + std::string(std::strerror(errno));
        close();
        return Status::Failed;
    }
    if (::chmod(path.c_str(), 0600) != 0) {
        problem_ = "cannot set the mode of " + path.string() + ": " +
                   std::string(std::strerror(errno));
        close();
        return Status::Failed;
    }
    if (::listen(fd_, 16) != 0) {
        problem_ = "cannot listen on " + path.string() + ": " + std::string(std::strerror(errno));
        close();
        return Status::Failed;
    }
    if (!set_non_blocking(fd_)) {
        problem_ = "cannot make the listener non-blocking";
        close();
        return Status::Failed;
    }
    return Status::Listening;
}

Listener::AcceptResult Listener::accept_one() {
    AcceptResult result;
    if (fd_ < 0) {
        result.status = AcceptStatus::Failed;
        result.problem = "there is nothing listening to accept a connection on";
        return result;
    }
#if defined(SOCK_CLOEXEC)
    // Linux and the BSDs make the accepted descriptor with both flags in the
    // one call, which is the only form with no window at all: a fork between an
    // accept and an fcntl hands the new control connection to whatever is being
    // started. macOS has neither accept4 nor SOCK_CLOEXEC, and takes the branch
    // below.
    const int client = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    const int failure = client < 0 ? errno : 0;
#else
    const int client = ::accept(fd_, nullptr, nullptr);
    const int failure = client < 0 ? errno : 0;
    if (client >= 0 && (!set_close_on_exec(client) || !set_non_blocking(client))) {
        (void)::close(client);
        result.status = AcceptStatus::Failed;
        result.problem = "cannot make the connection non-blocking and close-on-exec";
        return result;
    }
#endif
    if (client < 0) {
        result.error = failure;
        switch (failure) {
            case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
                result.status = AcceptStatus::Idle;
                return result;
            case EINTR:
            case ECONNABORTED:
#if defined(EPROTO)
            // accept(2) says to treat a protocol error on a pending connection
            // as a connection that went away, not as a listener that is broken.
            case EPROTO:
#endif
                result.status = AcceptStatus::Retry;
                return result;
            case EMFILE:
            case ENFILE:
            case ENOMEM:
            case ENOBUFS:
                // The listener stays readable for as long as the connection is
                // pending, so a caller that comes straight back gets here again
                // with nothing changed. What has to change is elsewhere: fewer
                // descriptors held, or a pause before asking again.
                result.status = AcceptStatus::Exhausted;
                result.problem = "cannot accept a connection: " +
                                 std::string(std::strerror(failure));
                return result;
            default:
                result.status = AcceptStatus::Failed;
                result.problem = "cannot accept a connection: " +
                                 std::string(std::strerror(failure));
                return result;
        }
    }
    std::string refusal;
    if (!peer_is_this_user(client, refusal)) {
        (void)::close(client);
        result.status = AcceptStatus::Refused;
        result.problem = std::move(refusal);
        return result;
    }
    suppress_sigpipe(client);
    result.status = AcceptStatus::Accepted;
    result.fd = client;
    return result;
}

int Listener::accept_one(std::string& refusal) {
    const AcceptResult result = accept_one();
    // Only what a reader could act on, which is what this shape has always
    // said: "none pending" arrives several times a second and a message for it
    // would bury everything else, and a descriptor table that has run out says
    // nothing here because a caller that cannot pause is a caller that would
    // print it as fast as the loop goes round. Telling those apart is what the
    // AcceptResult overload is for (M-F5, consumed by the server loop in R2).
    if (result.status == AcceptStatus::Refused || result.status == AcceptStatus::Failed)
        refusal = result.problem;
    return result.fd;
}

void Listener::close() noexcept {
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
        // Only the process that bound it removes it, and only while it still
        // holds the lock — otherwise a shutting-down server would delete the
        // socket its replacement had just created.
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    if (lock_fd_ >= 0) {
        // The lock goes; the lock FILE stays. A starter that arrived while this
        // one was shutting down has already opened that file, and unlinking it
        // would leave the two of them holding exclusive locks on two different
        // inodes — the very race the lock exists to settle. Closing the
        // descriptor releases the lock on its own; the explicit unlock is for
        // the reader.
        (void)::flock(lock_fd_, LOCK_UN);
        (void)::close(lock_fd_);
        lock_fd_ = -1;
    }
}

Stream::Stream(int fd) : fd_(fd) {
    // Whoever made this descriptor, flush() promises never to raise SIGPIPE on
    // this process — and on macOS that promise is a socket option rather than a
    // flag on each write. Set here so it holds for every Stream and not only
    // for the ones whose descriptor came from this file.
    if (fd_ >= 0) suppress_sigpipe(fd_);
}

Stream::~Stream() { close(); }

Stream::Stream(Stream&& other) noexcept
    : fd_(other.fd_), pending_(std::move(other.pending_)), sent_(other.sent_) {
    other.fd_ = -1;
    other.sent_ = 0;
}

Stream& Stream::operator=(Stream&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        pending_ = std::move(other.pending_);
        sent_ = other.sent_;
        other.fd_ = -1;
        other.sent_ = 0;
    }
    return *this;
}

bool Stream::send(std::string_view bytes) {
    pending_.append(bytes);
    // A write that failed means the peer is gone, and that is the one answer a
    // sender must not be given as "fine": a server told nothing goes on
    // queueing a session's whole output — megabyte after megabyte of it — into
    // a socket that will never take another byte.
    if (!flush()) return false;
    return !over_high_water();
}

bool Stream::flush() {
    while (sent_ < pending_.size()) {
        // Never SIGPIPE. A peer that goes away must not take this process with
        // it: `write` to a socket whose other end has closed raises SIGPIPE,
        // whose default action is death, and a client that happened to be
        // sending a request when its server exited died mid-frame with nothing
        // on the screen to say why. The server ignores the signal process-wide;
        // this is the same guarantee for everyone who holds a Stream, made at
        // the one call that can raise it.
#if defined(MSG_NOSIGNAL)
        const ssize_t written =
            ::send(fd_, pending_.data() + sent_, pending_.size() - sent_, MSG_NOSIGNAL);
#else
        // macOS has no MSG_NOSIGNAL; SO_NOSIGPIPE on the socket does the same
        // thing once, and is set where the socket is made.
        const ssize_t written = ::write(fd_, pending_.data() + sent_, pending_.size() - sent_);
#endif
        if (written > 0) {
            sent_ += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return false;  // the peer is gone
    }
    // Reclaim once the dead prefix is worth reclaiming, rather than erasing the
    // front on every write: the same reason the emulator's history keeps an
    // offset instead.
    if (sent_ == pending_.size()) {
        pending_.clear();
        sent_ = 0;
    } else if (sent_ > (1u << 16u) && sent_ * 2 > pending_.size()) {
        pending_.erase(0, sent_);
        sent_ = 0;
    }
    return true;
}

bool Stream::receive(std::string& into, std::size_t byte_budget) {
    char buffer[64 * 1024];
    while (byte_budget > 0) {
        const std::size_t want = std::min(byte_budget, sizeof buffer);
        const ssize_t count = ::read(fd_, buffer, want);
        if (count > 0) {
            into.append(buffer, static_cast<std::size_t>(count));
            byte_budget -= static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return false;  // the peer closed: a detach, from here
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
    // Out of budget with bytes still waiting. Not an error and not the end: the
    // caller comes back after doing everything else it has to do.
    return true;
}

void Stream::close() noexcept {
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
    pending_.clear();
    sent_ = 0;
}

}  // namespace ckm::platform
