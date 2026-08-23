// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// ckspy proving itself (tools/ckspy.c).
//
// This suite exists because of one rule, learned the hard way and now written
// into the engineering standard: **a zero from an instrument you have not seen fire is
// worthless.** Every way `DYLD_INSERT_LIBRARIES` can fail — a hardened or
// SIP-protected target, a dylib that did not build, an environment variable
// that did not survive an exec — produces an EMPTY LOG, and an empty log reads
// exactly like a clean run. A test asserting "ckmux opened no printer device"
// against a silently disabled interposer passes forever while measuring
// nothing.
//
// So the assertions here are almost all POSITIVE: they show the instrument
// catching things it must catch, against a real ckmux server that really does
// fork a real child. Only once that is established does the one negative in
// this file — no printer device — mean anything at all.
//
// It is also where the technique's limits are recorded as executable facts
// rather than as comments somebody may not read: SIP-protected binaries are
// demonstrated to be immune, so that a future reader who points ckspy at
// `/bin/sh` and gets nothing knows immediately whether they have found a clean
// run or a stripped environment.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "reader_harness.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

using ckmtest::binary_path;
using ckmtest::end_process;
using ckmtest::forget;
using ckmtest::private_socket;
using ckmtest::start_server;
using ckmtest::wait_for_socket;

std::filesystem::path spy_dylib() {
#if defined(CKSPY_DYLIB_PATH)
    return std::filesystem::path(CKSPY_DYLIB_PATH);
#else
    return {};
#endif
}

std::string read_log(const std::filesystem::path& path) {
    std::string text;
    if (std::FILE* const file = std::fopen(path.string().c_str(), "rb")) {
        char buffer[8192];
        std::size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, got);
        (void)std::fclose(file);
    }
    return text;
}

std::size_t count_of(const std::string& log, const std::string& kind) {
    std::size_t count = 0;
    std::size_t at = 0;
    while ((at = log.find(kind, at)) != std::string::npos) {
        ++count;
        at += kind.size();
    }
    return count;
}

// Runs `program` under the interposer and returns what it recorded. The
// environment is set in THIS process before the fork, because that is how the
// child inherits it — and inheritance across exec is the property the whole
// instrument depends on.
std::string spy_on(const std::vector<std::string>& command, const std::filesystem::path& log) {
    std::error_code ignored;
    std::filesystem::remove(log, ignored);
    ::setenv("CKSPY_LOG", log.string().c_str(), 1);
    ::setenv("DYLD_INSERT_LIBRARIES", spy_dylib().string().c_str(), 1);
    const ::pid_t child = ::fork();
    if (child == 0) {
        std::vector<char*> argv;
        for (const std::string& word : command) argv.push_back(const_cast<char*>(word.c_str()));
        argv.push_back(nullptr);
        const int null_fd = ::open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            (void)::dup2(null_fd, STDOUT_FILENO);
            (void)::dup2(null_fd, STDERR_FILENO);
            (void)::close(null_fd);
        }
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    int status = 0;
    (void)::waitpid(child, &status, 0);
    ::unsetenv("DYLD_INSERT_LIBRARIES");
    ::unsetenv("CKSPY_LOG");
    return read_log(log);
}

}  // namespace

CK_TEST(the_interposer_records_what_a_real_ckmux_server_opened_and_executed) {
    if (binary_path().empty() || spy_dylib().empty()) return;
    const std::filesystem::path log =
        std::filesystem::temp_directory_path() / "ckspy-server.log";
    const std::filesystem::path socket = private_socket("ckspy");
    forget(socket);

    std::error_code ignored;
    std::filesystem::remove(log, ignored);
    ::setenv("CKSPY_LOG", log.string().c_str(), 1);
    ::setenv("DYLD_INSERT_LIBRARIES", spy_dylib().string().c_str(), 1);
    const ::pid_t server = start_server(socket);
    const bool listening = wait_for_socket(socket);
    CK_CHECK(listening);

    // A reader, so the server actually FORKS something. A server that only
    // started and stopped opens files and forks nothing, and a test asserting
    // "records what it opened and forked" against that run would be proving
    // half its own name — which is how an instrument ends up trusted for a
    // question it was never shown answering.
    ckmtest::Reader reader;
    // The client is deliberately NOT measured: it inherits an explicit
    // environment from the harness, and what this test needs is the SERVER's
    // fork of the terminal's program, which the server does under the
    // interposer it was started with.
    const bool started = reader.start(socket, ckv::Size{80, 24});
    CK_CHECK(started);
    // "Terminal 1" and not the footer's "new term": the footer is drawn
    // whether or not a terminal exists, so waiting on it proves the client
    // started and nothing about whether the server ever launched a child.
    if (started) CK_CHECK(reader.sees("Terminal 1", 20000));
    reader.settle(1500);
    reader.quit();
    ::unsetenv("DYLD_INSERT_LIBRARIES");
    ::unsetenv("CKSPY_LOG");

    end_process(server);
    forget(socket);

    const std::string recorded = read_log(log);
    std::printf("  [ckspy] %zu OPEN, %zu FORK, %zu SPAWN, %zu EXEC from a real server\n",
                count_of(recorded, "[OPEN]"), count_of(recorded, "[FORK]"),
                count_of(recorded, "[SPAWN]"), count_of(recorded, "[EXEC]"));

    // THE PROOF OF LIFE. A server that started, listened and was asked to stop
    // cannot have opened nothing — it read a configuration, made a socket and
    // wrote a lock. If this is zero the instrument is disabled, not the server
    // idle, and every negative taken with it is void.
    CK_CHECK(!recorded.empty());
    CK_CHECK(count_of(recorded, "[OPEN]") > 0);
    // A child was really exec'd — the event kind a "ckmux ran no command"
    // claim actually rests on. Without this, a negative taken with this
    // instrument would assert the absence of something it has never been shown
    // detecting.
    //
    // Note what is NOT asserted: `[FORK]`. The server launches a terminal
    // through `forkpty`, whose fork happens inside libutil and never reaches
    // the public `fork` symbol this interposes — caveat 2 in tools/ckspy.c,
    // measured here rather than assumed. The exec that follows IS caught, and
    // that is the one that matters: "ran a command" means an exec, and a
    // spooler reached by fork+exec or by posix_spawn is visible either way.
    // A test demanding a FORK event here would fail forever for a reason that
    // has nothing to do with the instrument working.
    CK_CHECK(count_of(recorded, "[EXEC]") > 0);
    // And it saw this specific process: the log carries the pid of whoever
    // logged, which is what stops a stale log from a previous run being read as
    // this one's evidence.
    CK_CHECK(recorded.find("pid=") != std::string::npos);

    // Only now is this worth asserting. It is the shape WP-21 §5 needs, taken
    // by an instrument that cannot miss an open between two samples the way
    // sampling `lsof` can.
    CK_CHECK(recorded.find("/dev/lp") == std::string::npos);
    CK_CHECK(recorded.find("/dev/usb/lp") == std::string::npos);

    std::filesystem::remove(log, ignored);
}

CK_TEST(the_interposer_follows_a_child_across_fork_and_exec) {
    if (binary_path().empty() || spy_dylib().empty()) return;
    // `DYLD_INSERT_LIBRARIES` is inherited across exec, which is the property
    // that lets ckspy answer "did ckmux run a command" at all: a spooler
    // spawned by a child is still measured. Demonstrated rather than assumed,
    // because if it stopped being true every "nothing was spawned" result in
    // the suite would silently become unfalsifiable.
    const std::filesystem::path log = std::filesystem::temp_directory_path() / "ckspy-exec.log";
    // `ckmux ls` with no server: it connects to nothing, says so and exits —
    // the cheapest real invocation of the product there is.
    const std::filesystem::path socket = private_socket("ckspyls");
    forget(socket);
    const std::string recorded =
        spy_on({binary_path().string(), "ls", "--socket", socket.string()}, log);

    std::printf("  [ckspy] %zu OPEN, %zu EXEC from `ckmux ls`\n", count_of(recorded, "[OPEN]"),
                count_of(recorded, "[EXEC]"));
    // The exec'd program logged from inside itself, which only happens if the
    // library survived the exec into a new image.
    CK_CHECK(!recorded.empty());
    CK_CHECK(count_of(recorded, "[OPEN]") > 0);

    std::error_code ignored;
    std::filesystem::remove(log, ignored);
    forget(socket);
}

CK_TEST(a_protected_binary_is_immune_and_that_is_why_a_zero_needs_a_witness) {
    if (spy_dylib().empty()) return;
    // The limit, executable. macOS strips `DYLD_INSERT_LIBRARIES` for
    // SIP-protected binaries, so pointing ckspy at `/bin/sh` records NOTHING
    // — and that empty log is indistinguishable from a run in which nothing
    // happened.
    //
    // This is the case that makes the rule non-negotiable: every other test
    // that trusts ckspy must assert something it expects to FIND, or it is
    // asserting nothing. Recorded here so the next reader who gets an empty
    // log knows to suspect the target before suspecting the code.
    const std::filesystem::path log = std::filesystem::temp_directory_path() / "ckspy-sip.log";
    const std::string recorded = spy_on({"/bin/sh", "-c", "cat /etc/hosts >/dev/null"}, log);
    std::printf("  [ckspy] SIP-protected /bin/sh recorded %zu bytes (expected: none)\n",
                recorded.size());
    CK_CHECK(recorded.empty());
    std::error_code ignored;
    std::filesystem::remove(log, ignored);
}
