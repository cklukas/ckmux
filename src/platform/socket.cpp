// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "platform/socket.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/ucred.h>
#endif

namespace ckm::platform {
namespace {

// An environment variable that is set but empty is not a value. Treating one as
// a value is how a program comes to listen on "/ckmux-501/default.sock".
const char* value_of(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

bool set_non_blocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
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
    if (const char* const explicit_path = value_of("CKMUX_SOCKET"))
        return std::filesystem::path(explicit_path);
    const char* runtime = value_of("XDG_RUNTIME_DIR");
    if (runtime == nullptr) runtime = value_of("TMPDIR");
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
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    // An existing directory is not an error; anything else about it might be.
    struct ::stat info{};
    if (::stat(directory.c_str(), &info) != 0) {
        problem = "cannot create " + directory.string() + ": " + std::string(std::strerror(errno));
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        problem = directory.string() + " exists and is not a directory";
        return false;
    }
    if (info.st_uid != ::geteuid()) {
        problem = directory.string() + " belongs to another user";
        return false;
    }
    // Owner-only, always, and reset rather than merely checked: a directory
    // anybody else can write to is a socket anybody else can replace, and the
    // reader who chmod'd it once by accident should not have to find that out
    // from a security advisory.
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0 && ::chmod(directory.c_str(), 0700) != 0) {
        problem = "cannot make " + directory.string() + " owner-only: " +
                  std::string(std::strerror(errno));
        return false;
    }
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
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        result.status = ConnectStatus::Unusable;
        result.problem = "cannot make a socket: " + std::string(std::strerror(errno));
        return result;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int failure = errno;
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
    // stale-socket recovery of plans/02.
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        problem_ = "cannot make a socket: " + std::string(std::strerror(errno));
        close();
        return Status::Failed;
    }
    // The socket is created with the process umask applied, so the mode is set
    // afterwards rather than hoped for.
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

int Listener::accept_one(std::string& refusal) {
    if (fd_ < 0) return -1;
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client < 0) return -1;
    if (!peer_is_this_user(client, refusal)) {
        (void)::close(client);
        return -1;
    }
    if (!set_non_blocking(client)) {
        refusal = "cannot make the connection non-blocking";
        (void)::close(client);
        return -1;
    }
    return client;
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
        (void)::flock(lock_fd_, LOCK_UN);
        (void)::close(lock_fd_);
        lock_fd_ = -1;
    }
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
    (void)flush();
    return !over_high_water();
}

bool Stream::flush() {
    while (sent_ < pending_.size()) {
        const ssize_t written = ::write(fd_, pending_.data() + sent_, pending_.size() - sent_);
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
