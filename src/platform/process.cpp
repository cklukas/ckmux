// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/process.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace ckm::platform {
namespace {

// How many signal numbers there are to put back. NSIG counts them on every
// system ckmux builds on; the constant is only here so that a system which
// hides it behind a feature-test macro gets the POSIX signals plus room for
// the real-time range rather than nothing at all.
#if defined(NSIG)
constexpr int kSignalCount = NSIG;
#elif defined(_NSIG)
constexpr int kSignalCount = _NSIG;
#else
constexpr int kSignalCount = 65;
#endif

}  // namespace

void reset_signals_for_child() noexcept {
    struct sigaction restore_default = {};
    restore_default.sa_handler = SIG_DFL;
    // Unqualified on purpose: sigemptyset is a macro on some platforms, and a
    // macro has no namespace to qualify it with.
    sigemptyset(&restore_default.sa_mask);
    // Every number, rather than the ones ckmux is known to touch today: this
    // runs in a child that is about to become somebody else's program, and the
    // list of what this process ignored is not a list a fork site should have
    // to keep. SIGKILL and SIGSTOP refuse, which is the answer that costs
    // nothing — they cannot be ignored or caught either.
    for (int number = 1; number < kSignalCount; ++number)
        (void)sigaction(number, &restore_default, nullptr);
    sigset_t nothing_blocked;
    sigemptyset(&nothing_blocked);
    (void)sigprocmask(SIG_SETMASK, &nothing_blocked, nullptr);
}

bool daemonize(const std::filesystem::path& log) {
    const pid_t first = ::fork();
    if (first < 0) return false;
    if (first > 0) ::_exit(0);  // the starter's child: leaves the parent free

    if (::setsid() < 0) return false;

    // The second fork gives up session leadership, which is what makes it
    // impossible for this process to ever acquire a controlling terminal.
    const pid_t second = ::fork();
    if (second < 0) return false;
    if (second > 0) ::_exit(0);

    if (::chdir("/") != 0) return false;

    // Stdio to the log, and to /dev/null when even that cannot be opened: a
    // server whose descriptors 0, 1 and 2 are closed will hand those numbers to
    // the next thing it opens, and then a stray printf writes into a PTY.
    const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    int log_fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (log_fd < 0) log_fd = null_fd;
    if (null_fd >= 0) (void)::dup2(null_fd, STDIN_FILENO);
    if (log_fd >= 0) {
        (void)::dup2(log_fd, STDOUT_FILENO);
        (void)::dup2(log_fd, STDERR_FILENO);
    }
    // Both were opened close-on-exec so that no spare copy reaches a shell, and
    // `dup2` clears the flag on the copy it makes — except when there is no
    // copy to make. If 0, 1 or 2 was closed when this process started, the open
    // above was handed that number and `dup2(fd, fd)` is a no-op that leaves
    // the flag alone: the server's own stdout would then close itself on the
    // first exec, and every terminal it started would run with no standard
    // error at all. Cleared explicitly, so which case this is does not matter.
    for (const int standard : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        const int flags = ::fcntl(standard, F_GETFD, 0);
        if (flags >= 0) (void)::fcntl(standard, F_SETFD, flags & ~FD_CLOEXEC);
    }
    if (null_fd >= 0 && null_fd > STDERR_FILENO) (void)::close(null_fd);
    if (log_fd >= 0 && log_fd > STDERR_FILENO && log_fd != null_fd) (void)::close(log_fd);
    return true;
}

bool start_server(const std::filesystem::path& executable, const std::filesystem::path& socket,
                  std::string& problem) {
    // Everything the child needs is built before the fork. After it, only
    // async-signal-safe calls are defined — no allocation, no arbitrary code —
    // and building an argument vector out of std::string is both.
    const std::string program = executable.string();
    const std::string flag = "--server";
    const std::string path = socket.string();
    char* const argv[] = {const_cast<char*>(program.c_str()), const_cast<char*>(flag.c_str()),
                          const_cast<char*>(path.c_str()), nullptr};

    const pid_t child = ::fork();
    if (child < 0) {
        problem = "cannot fork to start a server: " + std::string(std::strerror(errno));
        return false;
    }
    if (child == 0) {
        // The server is a fresh program, not a continuation of the client: it
        // installs its own handlers, and must not start out with the client's
        // dispositions and blocked signals still in force.
        reset_signals_for_child();
        ::execv(program.c_str(), argv);
        // An exec that fails leaves a copy of the client running, which would
        // be worse than no server at all.
        ::_exit(127);
    }
    // The child is the first half of the double fork and exits at once; reaping
    // it here is what keeps a client from collecting zombies. Waiting for the
    // SERVER would be waiting for the whole session to end.
    int status = 0;
    bool reported = false;
    for (;;) {
        if (::waitpid(child, &status, 0) == child) {
            reported = true;
            break;
        }
        if (errno == EINTR) continue;
        // ECHILD: something reaped it already, or SIGCHLD is ignored and the
        // system reaped it for us. Either way there is no status to read, and
        // reading the untouched `status` would say "exited 0" about a process
        // nobody watched.
        break;
    }
    if (reported && WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        problem = "cannot run " + executable.string();
        return false;
    }
    return true;
}

std::filesystem::path executable_path(const char* argv0) {
#if defined(__APPLE__)
    std::vector<char> buffer(1024, '\0');
    std::uint32_t size = static_cast<std::uint32_t>(buffer.size());
    bool have_path = _NSGetExecutablePath(buffer.data(), &size) == 0;
    if (!have_path) {
        // The one documented failure: the buffer was too small, and `size` now
        // says how large it has to be. Retried rather than given up on, because
        // giving up falls back to argv[0] — which, for a ckmux started through
        // PATH, is "ckmux" and not a path at all, and the server it would then
        // try to start does not exist.
        buffer.assign(size + 1u, '\0');
        size = static_cast<std::uint32_t>(buffer.size());
        have_path = _NSGetExecutablePath(buffer.data(), &size) == 0;
    }
    if (have_path) {
        std::error_code error;
        const std::filesystem::path resolved =
            std::filesystem::canonical(std::filesystem::path(buffer.data()), error);
        if (!error) return resolved;
        return std::filesystem::path(buffer.data());
    }
#else
    std::error_code error;
    const std::filesystem::path resolved =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return resolved;
#endif
    return argv0 != nullptr ? std::filesystem::path(argv0) : std::filesystem::path();
}

std::filesystem::path server_log_path(const std::filesystem::path& socket) {
    std::filesystem::path log = socket;
    log += ".log";
    return log;
}

}  // namespace ckm::platform
