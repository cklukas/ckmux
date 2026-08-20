// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/clipboard.hpp"

#if !defined(_WIN32)

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/descriptors.hpp"
#include "platform/process.hpp"

namespace ckm::platform {
namespace {

using clock_type = std::chrono::steady_clock;

// How long a pass around the loop may sleep. There is no descriptor to wait on
// for "the child has exited", so the loop asks with WNOHANG and comes back;
// this is what that costs a copy that has already been written — a fiftieth of
// a second, once.
constexpr int kExitPollSliceMs = 20;

// Enough of the helper's output to name what went wrong, and no more: the
// point of capturing it is a message a reader can act on, not a transcript.
// Everything past this is still read — a helper blocked on a full pipe is a
// helper that never exits — and dropped.
constexpr std::size_t kDiagnosticsCap = 4096;

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
    bool open() const noexcept { return fd_ >= 0; }
    void close_now() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_;
};

// SIGPIPE ignored for as long as this object lives, and back to whatever it
// was afterwards. A helper that exits without reading — a misspelled command,
// a clipboard daemon that has gone — must not take ckmux down with it: the
// write simply fails, and the failure is reported like any other.
class SigpipeIgnored {
public:
    SigpipeIgnored() {
        struct sigaction ignore = {};
        ignore.sa_handler = SIG_IGN;
        // Unqualified on purpose: sigemptyset is a macro on some platforms, and
        // a macro has no namespace to qualify it with.
        sigemptyset(&ignore.sa_mask);
        swapped_ = sigaction(SIGPIPE, &ignore, &previous_) == 0;
    }
    ~SigpipeIgnored() {
        if (swapped_) (void)sigaction(SIGPIPE, &previous_, nullptr);
    }
    SigpipeIgnored(const SigpipeIgnored&) = delete;
    SigpipeIgnored& operator=(const SigpipeIgnored&) = delete;

private:
    struct sigaction previous_ = {};
    bool swapped_ = false;
};

int milliseconds_until(clock_type::time_point deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now()).count();
    if (remaining <= 0) return 0;
    return remaining > kExitPollSliceMs ? kExitPollSliceMs : static_cast<int>(remaining);
}

}  // namespace

bool write_to_command(const std::string& command, std::string_view text, std::string* diagnostics,
                      int idle_budget_ms) {
    if (diagnostics != nullptr) diagnostics->clear();
    if (command.empty()) return false;

    // Both pipes are close-on-exec, so the helper inherits exactly the three
    // descriptors `dup2` gives it and none of ckmux's — not the control socket
    // above all, which a `sh -c` line could otherwise talk to as if it were the
    // client itself.
    int input[2] = {-1, -1};
    if (!make_pipe(input)) return false;
    Descriptor input_read(input[0]);
    Descriptor input_write(input[1]);
    int captured[2] = {-1, -1};
    if (!make_pipe(captured)) return false;
    Descriptor captured_read(captured[0]);
    Descriptor captured_write(captured[1]);

    const ::pid_t child = ::fork();
    if (child < 0) return false;
    if (child == 0) {
        // The child. Nothing here may allocate or run arbitrary code — between
        // fork and exec only async-signal-safe calls are defined.
        //
        // stdout and stderr go to the capture pipe rather than to whatever
        // ckmux is drawing on. `dup2` clears close-on-exec on the copy it
        // makes, which is what lets both pipes be close-on-exec and the
        // helper's three descriptors still survive the exec.
        if (::dup2(input[0], STDIN_FILENO) < 0) ::_exit(127);
        if (::dup2(captured[1], STDOUT_FILENO) < 0) ::_exit(127);
        if (::dup2(captured[1], STDERR_FILENO) < 0) ::_exit(127);
        // Whatever this process ignores or blocks for its own sake — a client
        // and a server both ignore SIGPIPE so a peer that vanishes cannot kill
        // them — survives exec. A helper that is itself a pipeline
        // (`gzip | ssh host pbcopy`) would inherit that and stop behaving like
        // a program started from a shell.
        reset_signals_for_child();
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    // The ends that belong to the child, so that its stdin sees an end and its
    // output pipe sees a reader that can notice EOF.
    input_read.close_now();
    captured_write.close_now();
    const SigpipeIgnored no_sigpipe;

    // Non-blocking, both of them: a helper that reads nothing must not be able
    // to stop the loop that draws the screen, and neither must one that says
    // more than it is asked.
    const bool usable =
        set_non_blocking(input_write.get()) && set_non_blocking(captured_read.get());

    // Everything the helper says, up to the cap, and read past the cap either
    // way: a helper blocked writing into a pipe nobody empties is a helper that
    // never exits.
    const auto absorb = [diagnostics](const char* said, std::size_t length) {
        if (diagnostics == nullptr || diagnostics->size() >= kDiagnosticsCap) return;
        diagnostics->append(said, std::min(length, kDiagnosticsCap - diagnostics->size()));
    };

    std::size_t offset = 0;
    bool wrote_everything = usable && text.empty();
    bool reaped = false;
    bool status_known = false;
    int status = 0;
    const auto budget = std::chrono::milliseconds(idle_budget_ms < 0 ? 0 : idle_budget_ms);
    clock_type::time_point deadline = clock_type::now() + budget;

    if (usable && text.empty()) input_write.close_now();

    while (usable) {
        // Asked first, because a helper that has exited is finished whether or
        // not its output pipe has closed: `xclip` forks a process that holds
        // the selection — and the pipe — for as long as it owns the clipboard,
        // so waiting for EOF on the capture would mean waiting out the whole
        // budget on every single copy.
        const ::pid_t seen = ::waitpid(child, &status, WNOHANG);
        if (seen == child) {
            reaped = true;
            status_known = true;
            break;
        }
        if (seen < 0 && errno != EINTR) {
            // ECHILD: the child is gone and its status went with it — SIGCHLD
            // ignored process-wide has the system reap children itself. There
            // is nothing left to wait for and nothing to read from it.
            reaped = true;
            break;
        }
        if (clock_type::now() >= deadline) break;

        pollfd waiting[2] = {};
        nfds_t count = 0;
        const bool writing = input_write.open() && offset < text.size();
        if (writing) {
            waiting[count].fd = input_write.get();
            waiting[count].events = POLLOUT;
            ++count;
        }
        if (captured_read.open()) {
            waiting[count].fd = captured_read.get();
            waiting[count].events = POLLIN;
            ++count;
        }
        const int slice = milliseconds_until(deadline);
        const int ready = ::poll(count > 0 ? waiting : nullptr, count, slice);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        for (nfds_t index = 0; index < count; ++index) {
            if (waiting[index].revents == 0) continue;
            if (writing && index == 0) {
                const ::ssize_t written =
                    ::write(input_write.get(), text.data() + offset, text.size() - offset);
                if (written > 0) {
                    offset += static_cast<std::size_t>(written);
                    deadline = clock_type::now() + budget;
                    if (offset == text.size()) {
                        wrote_everything = true;
                        // Closed as soon as there is nothing left to say, or
                        // the helper sits reading a pipe that will never end
                        // and both processes wait for each other.
                        input_write.close_now();
                    }
                    continue;
                }
                if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                    continue;
                // EPIPE, or anything else: the helper is not taking this text.
                input_write.close_now();
                continue;
            }
            char buffer[1024];
            const ::ssize_t read_bytes = ::read(captured_read.get(), buffer, sizeof buffer);
            if (read_bytes > 0) {
                deadline = clock_type::now() + budget;
                absorb(buffer, static_cast<std::size_t>(read_bytes));
                continue;
            }
            if (read_bytes == 0) {
                captured_read.close_now();
                continue;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            captured_read.close_now();
        }
    }

    // What the helper said last. The loop stops the moment the child is reaped,
    // which can be before its final words have been read out of the pipe — and
    // those are exactly the words worth having, because a helper that failed
    // says why on its way out. Non-blocking, so a grandchild still holding the
    // write end (xclip's daemon does) cannot turn this into a wait.
    while (captured_read.open()) {
        char buffer[1024];
        const ::ssize_t read_bytes = ::read(captured_read.get(), buffer, sizeof buffer);
        if (read_bytes > 0) {
            absorb(buffer, static_cast<std::size_t>(read_bytes));
            continue;
        }
        if (read_bytes < 0 && errno == EINTR) continue;
        captured_read.close_now();
    }

    if (!reaped) {
        // Out of budget, or something failed before the loop began. The helper
        // is killed rather than left behind: an unreaped child is a zombie for
        // the life of the client, and a stuck helper holding the write end of
        // ckmux's pipes is a descriptor leak on top of it. SIGKILL because a
        // helper that has ignored a deadline has already shown what it does
        // with a polite signal.
        (void)::kill(child, SIGKILL);
        for (;;) {
            if (::waitpid(child, &status, 0) == child) {
                status_known = true;
                break;
            }
            if (errno == EINTR) continue;
            break;  // ECHILD: somebody reaped it, and there is no status to read
        }
        return false;
    }

    // A status that could not be read is not a failure on its own: the text
    // went out, and "the system reaped the helper for us" says nothing about
    // how it went. What is never success is text that did not all arrive.
    if (!wrote_everything) return false;
    return !status_known || (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

}  // namespace ckm::platform

#endif  // !_WIN32
