// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The CLI's front door, against the real binary. `--help` and `--version` are
// what a reader types before trusting a program with their terminals, and the
// refusal for everything else is what keeps a typo from silently opening a
// client. All of it is dispatch in main(), so nothing short of running the
// actual entry point tests it — the same reasoning that has the lifecycle
// suite start a real server.
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "common/proto.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

std::filesystem::path binary_path() {
#if defined(CKMUX_BINARY_PATH)
    return std::filesystem::path(CKMUX_BINARY_PATH);
#else
    return {};
#endif
}

struct RunResult {
    int exit_code = -1;
    std::string out;
    std::string err;
};

std::string read_all(int fd) {
    std::string text;
    char buffer[4096];
    for (;;) {
        const ::ssize_t got = ::read(fd, buffer, sizeof buffer);
        if (got <= 0) break;
        text.append(buffer, static_cast<std::size_t>(got));
    }
    return text;
}

// Runs the real ckmux with one argument vector and brings back everything an
// observer at a shell would have seen: both streams, and the exit status.
// Only argument vectors that answer and exit belong here — a bare `ckmux`
// would attach a client to the test's own terminal and wait forever.
RunResult run_ckmux(const std::vector<std::string>& arguments) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(out_pipe) != 0) return {};
    if (::pipe(err_pipe) != 0) {
        (void)::close(out_pipe[0]);
        (void)::close(out_pipe[1]);
        return {};
    }
    const ::pid_t child = ::fork();
    if (child == 0) {
        (void)::dup2(out_pipe[1], STDOUT_FILENO);
        (void)::dup2(err_pipe[1], STDERR_FILENO);
        (void)::close(out_pipe[0]);
        (void)::close(out_pipe[1]);
        (void)::close(err_pipe[0]);
        (void)::close(err_pipe[1]);
        const std::string program = binary_path().string();
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const std::string& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        ::execv(program.c_str(), argv.data());
        ::_exit(127);
    }
    (void)::close(out_pipe[1]);
    (void)::close(err_pipe[1]);
    RunResult result;
    // Both streams are read to end-of-file before the wait: these pages are
    // far smaller than a pipe's buffer, so neither stream can block the
    // child, and end-of-file on both is the child being gone.
    result.out = read_all(out_pipe[0]);
    result.err = read_all(err_pipe[0]);
    (void)::close(out_pipe[0]);
    (void)::close(err_pipe[0]);
    int status = 0;
    (void)::waitpid(child, &status, 0);
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    return result;
}

}  // namespace

CK_TEST(help_is_a_page_on_stdout_and_an_exit_of_zero) {
    const RunResult result = run_ckmux({"--help"});
    CK_CHECK(result.exit_code == 0);
    CK_CHECK(result.err.empty());
    // The page opens with the same identity `--version` prints and the
    // handshake sends — one number, everywhere.
    CK_CHECK(result.out.find(std::string(ckm::proto::kBuildIdentity)) == 0);
    // And it covers the whole surface: every way in, and both environment
    // variables a reader can set. A subcommand missing from this list is a
    // subcommand the refusal below turns away, so the two must agree.
    CK_CHECK(result.out.find("Usage:") != std::string::npos);
    CK_CHECK(result.out.find("kill-server") != std::string::npos);
    CK_CHECK(result.out.find("--server") != std::string::npos);
    CK_CHECK(result.out.find("--version") != std::string::npos);
    CK_CHECK(result.out.find("CKMUX_SOCKET") != std::string::npos);
    CK_CHECK(result.out.find("CKMUX_CONFIG") != std::string::npos);
}

CK_TEST(every_spelling_of_help_prints_the_same_page) {
    // `-h`, `--help` and `help` are all answered — the reader typing any of
    // them is asking, not scripting — and answered identically, so nobody
    // has to learn which spelling is the real one.
    const RunResult reference = run_ckmux({"--help"});
    for (const char* spelling : {"-h", "help"}) {
        const RunResult again = run_ckmux({spelling});
        CK_CHECK(again.exit_code == 0);
        CK_CHECK(again.out == reference.out);
    }
}

CK_TEST(the_help_page_fits_an_eighty_column_terminal) {
    // The page is typeset by hand for the narrowest terminal worth taking
    // seriously. Measured in bytes, which is stricter than columns: the one
    // multi-byte character (the em dash) still leaves its line inside 80.
    const RunResult result = run_ckmux({"--help"});
    CK_CHECK(!result.out.empty());
    CK_CHECK(result.out.back() == '\n');
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = result.out.find('\n', start);
        if (end == std::string::npos) break;
        CK_CHECK(end - start <= 80);
        start = end + 1;
    }
}

CK_TEST(version_prints_exactly_the_identity_the_handshake_compares) {
    // Pinned to equality, not containment: the string a reader quotes in a
    // bug report and the string a server refuses a mismatched client with
    // must be the same string.
    const RunResult result = run_ckmux({"--version"});
    CK_CHECK(result.exit_code == 0);
    CK_CHECK(result.err.empty());
    CK_CHECK(result.out == std::string(ckm::proto::kBuildIdentity) + "\n");
    const RunResult shorthand = run_ckmux({"-V"});
    CK_CHECK(shorthand.exit_code == 0);
    CK_CHECK(shorthand.out == result.out);
}

CK_TEST(an_unknown_argument_is_refused_toward_help) {
    const RunResult result = run_ckmux({"frobnicate"});
    CK_CHECK(result.exit_code == 2);
    // The refusal names what was typed, says where the answers are, and
    // stays off stdout — a script that piped this gets nothing, not a page.
    CK_CHECK(result.out.empty());
    CK_CHECK(result.err.find("frobnicate") != std::string::npos);
    CK_CHECK(result.err.find("--help") != std::string::npos);
}
