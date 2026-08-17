// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "platform/clipboard.hpp"

#if !defined(_WIN32)

#include <csignal>
#include <cerrno>
#include <unistd.h>
#include <sys/wait.h>

namespace ckm::platform {
namespace {

// Closes a descriptor once, and not twice — the write path closes early to
// tell the helper its input has ended, and the guard must not close whatever
// descriptor number was handed out next.
class Descriptor {
public:
    explicit Descriptor(int fd) noexcept : fd_(fd) {}
    ~Descriptor() { close_now(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    int get() const noexcept { return fd_; }
    void close_now() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_;
};

}  // namespace

bool write_to_command(const std::string& command, std::string_view text) {
    if (command.empty()) return false;
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) return false;
    Descriptor read_end(pipe_fds[0]);
    Descriptor write_end(pipe_fds[1]);

    const ::pid_t child = ::fork();
    if (child < 0) return false;
    if (child == 0) {
        // The child: stdin comes from the pipe, everything else is left as it
        // was. Nothing here may allocate or run arbitrary code — between fork
        // and exec only async-signal-safe calls are defined.
        if (::dup2(pipe_fds[0], STDIN_FILENO) < 0) ::_exit(127);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    read_end.close_now();
    // A helper that exits without reading — a misspelled command, a full
    // clipboard daemon — must not take ckmux down with SIGPIPE. The write
    // simply fails, and the failure is reported like any other.
    struct sigaction ignore = {};
    struct sigaction previous = {};
    ignore.sa_handler = SIG_IGN;
    // Unqualified on purpose: sigemptyset is a macro on some platforms, and
    // a macro has no namespace to qualify it with.
    sigemptyset(&ignore.sa_mask);
    const bool swapped = sigaction(SIGPIPE, &ignore, &previous) == 0;

    bool written = true;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const ::ssize_t count = ::write(write_end.get(), text.data() + offset, text.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        written = false;
        break;
    }
    // Closed before the wait, or the helper sits reading a pipe that will
    // never end and both processes wait for each other.
    write_end.close_now();
    if (swapped) sigaction(SIGPIPE, &previous, nullptr);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return false;
    return written && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace ckm::platform

#endif  // !_WIN32
