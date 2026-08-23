// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
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
#include <cstdlib>
#include <system_error>
#include <string_view>
#include <filesystem>
#include <string>
#include <vector>

#include "common/config.hpp"
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
// A socket of this test's own, in a directory this user owns. The server
// refuses to bind under a directory owned by somebody else, so a bare /tmp
// path is not usable here even though the address fits.
std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-cli" + std::to_string(static_cast<unsigned long>(::getpid()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (std::string(name) + ".sock");
}

void forget(const std::filesystem::path& socket) {
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".log"), ignored);
    std::filesystem::remove(socket.parent_path(), ignored);
}

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

// The same, with the child pointed at a private server. `setenv` in this
// process is inherited across the fork, and is undone afterwards so one case
// cannot decide where the next one looks.
RunResult run_ckmux_against(const std::filesystem::path& socket,
                            const std::vector<std::string>& arguments) {
    (void)::setenv("CKMUX_SOCKET", socket.string().c_str(), 1);
    RunResult result = run_ckmux(arguments);
    (void)::unsetenv("CKMUX_SOCKET");
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
    // Including the flags a subcommand takes. `--share` and `--adopt-size` are
    // both opt-ins for things that change what OTHER readers see, so a page
    // that lists the subcommand and hides them documents the safe half of a
    // command whose interesting half is the other one.
    CK_CHECK(result.out.find("--share") != std::string::npos);
    CK_CHECK(result.out.find("--adopt-size") != std::string::npos);
}

CK_TEST(an_attach_flag_that_does_not_exist_is_refused_at_the_shell) {
    // Before a clock, a terminal or a socket — the same reasoning that has
    // `ckmux attach typo` answered here rather than behind PosixTerminal's
    // demand for a real tty. A script gets an exit code and a sentence.
    const RunResult result = run_ckmux({"attach", "--adopt-siz", "work"});
    CK_CHECK(result.exit_code == 2);
    CK_CHECK(result.out.empty());
    CK_CHECK(result.err.find("--adopt-siz") != std::string::npos);
    // And the usage line that follows names the flag they meant.
    CK_CHECK(result.err.find("--adopt-size") != std::string::npos);
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

// --- WP-11: the subcommands -------------------------------------------------

CK_TEST(the_help_page_lists_every_subcommand_the_build_answers) {
    // The refusal below turns away anything not on this page, so the page and
    // the dispatch have to agree — the same rule the `kill-server` line has
    // always been held to, now that there are five more.
    const RunResult result = run_ckmux({"--help"});
    CK_CHECK(result.exit_code == 0);
    for (const char* subcommand : {"ckmux ls", "ckmux new", "ckmux attach",
                                   "ckmux kill-session", "ckmux check-config"})
        CK_CHECK(result.out.find(subcommand) != std::string::npos);
}

CK_TEST(a_subcommand_that_takes_no_arguments_refuses_the_ones_it_was_given) {
    // Silently ignoring them is how `ckmux ls work` comes to look as though it
    // filtered. Exit 2 is the argument refusal, distinct from 1, which is a
    // request that was understood and could not be carried out.
    const RunResult extra = run_ckmux({"ls", "work"});
    CK_CHECK(extra.exit_code == 2);
    CK_CHECK(extra.err.find("takes no arguments") != std::string::npos);
    CK_CHECK(extra.err.find("Usage: ckmux ls") != std::string::npos);

    const RunResult missing = run_ckmux({"kill-session"});
    CK_CHECK(missing.exit_code == 2);
    CK_CHECK(missing.err.find("needs the session to act on") != std::string::npos);

    const RunResult flag_without_value = run_ckmux({"new", "-s"});
    CK_CHECK(flag_without_value.exit_code == 2);
    CK_CHECK(flag_without_value.err.find("needs a session name after it") != std::string::npos);
}

CK_TEST(asking_what_exists_never_brings_a_server_into_existence) {
    // `ckmux ls` on a machine with no server says so and stops. A question
    // that starts its own subject answers itself — and a reader who typed it
    // to find out whether anything was running would be told "yes" by their
    // own asking.
    const std::filesystem::path socket = private_socket("ls-none");
    forget(socket);
    const RunResult result = run_ckmux_against(socket, {"ls"});
    CK_CHECK(result.exit_code == 1);
    CK_CHECK(result.err.find("no server is running") != std::string::npos);
    CK_CHECK(!std::filesystem::exists(socket));
    forget(socket);
}

CK_TEST(attaching_to_a_session_that_is_not_there_is_answered_before_any_screen_is_touched) {
    // The lookup happens before `PosixTerminal` is constructed, which is what
    // makes this a sentence and an exit status rather than an abort on a
    // missing tty — `ckmux attach typo` in a script has to be answerable.
    const std::filesystem::path socket = private_socket("attach-none");
    forget(socket);
    const RunResult result = run_ckmux_against(socket, {"attach", "nope"});
    CK_CHECK(result.exit_code == 1);
    // Exit 134 here would be SIGABRT: the terminal built before the answer.
    CK_CHECK(result.exit_code != 134);
    CK_CHECK(!std::filesystem::exists(socket));
    forget(socket);
}

CK_TEST(a_session_made_from_the_cli_is_a_session_the_cli_can_see_and_end) {
    // The round trip WP-11 exists for, over the real protocol with
    // `client_kind = cli`: make one, list it, end it, and find it gone.
    const std::filesystem::path socket = private_socket("round-trip");
    forget(socket);

    const RunResult made = run_ckmux_against(socket, {"new", "-s", "work"});
    CK_CHECK(made.exit_code == 0);
    // The name is the answer, so a script can `$(ckmux new)` and hold it.
    CK_CHECK(made.out == "work\n");

    const RunResult listed = run_ckmux_against(socket, {"ls"});
    CK_CHECK(listed.exit_code == 0);
    CK_CHECK(listed.out.find("work") != std::string::npos);
    // One terminal, because `spawn_first` defaults to on and the server now
    // honours it — the shell a reader expects to find when they attach.
    CK_CHECK(listed.out.find("1 terminal") != std::string::npos);

    const RunResult killed = run_ckmux_against(socket, {"kill-session", "work"});
    CK_CHECK(killed.exit_code == 0);

    // And it is gone rather than merely asked to go: `kill-session` waits for
    // the server to stop listing it, which is what makes this assertion sound
    // rather than a race against the grace period.
    const RunResult after = run_ckmux_against(socket, {"ls"});
    CK_CHECK(after.out.find("work") == std::string::npos);

    (void)run_ckmux_against(socket, {"kill-server"});
    forget(socket);
}

CK_TEST(a_name_already_in_use_is_refused_at_the_shell_and_says_so) {
    // This case used to assert the opposite half: it made two sessions called
    // `same` and checked that `attach same` refused to guess between them.
    // That state can no longer be built — the session model's two operation rows were
    // made to agree on 2026-08-20 and the server refuses a duplicate at
    // creation — so the case now asserts the refusal that replaced it. The
    // ambiguity path in `resolve_session` survives as a defensive branch and
    // is documented there as one; what changed is that nothing can reach it.
    const std::filesystem::path socket = private_socket("ambiguous");
    forget(socket);
    CK_CHECK(run_ckmux_against(socket, {"new", "-s", "same"}).exit_code == 0);

    const RunResult again = run_ckmux_against(socket, {"new", "-s", "same"});
    CK_CHECK(again.exit_code == 1);
    // The server's own sentence, not a generic failure: before this, a reader
    // colliding on a name was told "the server did not confirm the new
    // session", which is true, useless, and reads like a crash.
    CK_CHECK(again.err.find("already called") != std::string::npos);
    CK_CHECK(again.err.find("same") != std::string::npos);
    // And the name still names exactly one session, which is the whole point
    // of refusing: `kill-session` by name has something unambiguous to act on.
    const RunResult by_name = run_ckmux_against(socket, {"kill-session", "same"});
    CK_CHECK(by_name.exit_code == 0);

    (void)run_ckmux_against(socket, {"kill-server"});
    forget(socket);
}

CK_TEST(check_config_reports_without_starting_anything) {
    // The whole point of this one: a reader checking a file before trusting it
    // to a server. It touches no socket, so it cannot start one.
    const std::filesystem::path socket = private_socket("check");
    forget(socket);
    const RunResult result = run_ckmux_against(socket, {"check-config"});
    CK_CHECK(result.exit_code == 0);
    CK_CHECK(!std::filesystem::exists(socket));
    // The second half, which is what makes it worth more than a parse: the
    // keys that are spelled correctly, accepted, and reach nothing yet.
    //
    // Asserted against `keys_not_honoured_yet()` itself rather than against a
    // key named here. This assertion named `[general] on-exit` for exactly one
    // hour: WP-11 picked it as the example, WP-13 wired the key up and removed
    // it from the list, and the test then asserted the very claim the removal
    // was made to stop making — failing LOUDLY because somebody improved the
    // code, which trains the next reader to weaken it rather than fix it.
    // Naming `[printer] ask-cache` instead would only move the trap: PRINT-1…6
    // retires all four remaining keys at once.
    //
    // So what is checked here is what this command is actually responsible
    // for — that it prints the list the binary holds, whatever that list says.
    // Whether the list itself is honest is test_config's audit case, which is
    // where it belongs.
    const std::vector<std::string> waiting = ckm::keys_not_honoured_yet();
    CK_CHECK(!waiting.empty());  // else the loop below asserts nothing
    if (!waiting.empty()) CK_CHECK(result.out.find("not yet honoured") != std::string::npos);
    for (const std::string& key : waiting) CK_CHECK(result.out.find(key) != std::string::npos);
    // And the key that left the list must not be printed: the server reads
    // `on-exit` to decide whether an exited window stays on screen, so naming
    // it here would tell a reader it was dead. The positive partner for this
    // negative is the loop above — it fails if the section stops printing at
    // all, so this cannot pass merely because the output went empty.
    CK_CHECK(result.out.find("on-exit") == std::string::npos);
    forget(socket);
}

CK_TEST(a_command_given_to_new_reaches_the_session_it_made) {
    // `NewSession` has always carried `command` and `spawn_first`, and the
    // handler read neither — so this used to make an empty session and discard
    // what was typed, which reads exactly like success. Found by WP-11 and
    // fixed server-side; this is the test that keeps it fixed.
    const std::filesystem::path socket = private_socket("new-command");
    forget(socket);
    const RunResult made = run_ckmux_against(socket, {"new", "-s", "build", "sleep", "30"});
    CK_CHECK(made.exit_code == 0);
    CK_CHECK(made.out == "build\n");

    // The command runs in the session's first terminal, so the session has
    // one — an empty session here would be the old silent drop.
    const RunResult listed = run_ckmux_against(socket, {"ls"});
    CK_CHECK(listed.out.find("build") != std::string::npos);
    CK_CHECK(listed.out.find("1 terminal") != std::string::npos);

    (void)run_ckmux_against(socket, {"kill-server"});
    forget(socket);
}
