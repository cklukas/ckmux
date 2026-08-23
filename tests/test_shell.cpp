// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The programs ckmux starts: the shell a terminal runs, and the helper a copy
// is handed to.
//
// The shell's name and the shape it is started in are tmux's answers
// (the configuration spec), so those tests pin the behaviour a reader
// arriving from tmux already expects rather than our own invention. The helper
// is the other fork ckmux does, and the cases here are about what a forked
// child may and may not inherit — a descriptor, a signal disposition, or the
// screen the client is drawing on.
#include "common/shell.hpp"
#include "platform/clipboard.hpp"

#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "cvision/testing/cktest.hpp"

using ckm::resolve_shell;
using ckm::ShellLaunch;
using ckm::shell_launch;

namespace {

// Restores $SHELL however the test leaves, so one case cannot decide the
// next one's environment.
class ScopedShellEnvironment {
public:
    ScopedShellEnvironment() {
        const char* const current = std::getenv("SHELL");
        had_value_ = current != nullptr;
        if (had_value_) previous_ = current;
    }
    ~ScopedShellEnvironment() {
        if (had_value_) (void)::setenv("SHELL", previous_.c_str(), 1);
        else (void)::unsetenv("SHELL");
    }
    ScopedShellEnvironment(const ScopedShellEnvironment&) = delete;
    ScopedShellEnvironment& operator=(const ScopedShellEnvironment&) = delete;

    static void set(const std::string& value) { (void)::setenv("SHELL", value.c_str(), 1); }
    static void clear() { (void)::unsetenv("SHELL"); }

private:
    bool had_value_ = false;
    std::string previous_;
};

// A real, executable file with a chosen name, for the cases that turn on
// what a path IS rather than what it is called.
std::filesystem::path make_executable(const std::filesystem::path& directory, const std::string& name) {
    std::filesystem::create_directories(directory);
    const std::filesystem::path path = directory / name;
    std::ofstream out(path);
    out << "#!/bin/sh\nexit 0\n";
    out.close();
    ::chmod(path.c_str(), 0755);
    return path;
}

// A scratch directory of this run's own, gone again however the case leaves.
//
// Both halves matter. A fixed name under the temporary directory is a name two
// runs share — the same suite twice over on a build machine, or two people on
// one host — and the second one to arrive either fails on somebody else's
// files or, worse, passes on them. And a case that removes its directory on
// its last line removes nothing at all on the run where an assertion sends it
// somewhere else first, which is exactly the run that leaves the evidence
// behind. A destructor happens on every path there is.
class ScratchDirectory {
public:
    explicit ScratchDirectory(const std::string& tag) {
        const std::string name = "ckmux-shell-" +
                                 std::to_string(static_cast<unsigned long>(::getpid())) + "-" + tag;
        const std::filesystem::path candidate = std::filesystem::temp_directory_path() / name;
        std::error_code ignored;
        std::filesystem::remove_all(candidate, ignored);
        std::filesystem::create_directories(candidate, ignored);
        if (!ignored) path_ = candidate;
    }
    ~ScratchDirectory() {
        if (path_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string contents_of(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// SIGPIPE ignored for as long as this lives, which is how the client and the
// server both run — and the state a forked child must not be left in.
class ScopedIgnoredSigpipe {
public:
    ScopedIgnoredSigpipe() {
        struct sigaction ignore = {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        swapped_ = sigaction(SIGPIPE, &ignore, &previous_) == 0;
    }
    ~ScopedIgnoredSigpipe() {
        if (swapped_) (void)sigaction(SIGPIPE, &previous_, nullptr);
    }
    ScopedIgnoredSigpipe(const ScopedIgnoredSigpipe&) = delete;
    ScopedIgnoredSigpipe& operator=(const ScopedIgnoredSigpipe&) = delete;

    bool swapped() const noexcept { return swapped_; }

private:
    struct sigaction previous_ = {};
    bool swapped_ = false;
};

}  // namespace

// --- How a shell is started -------------------------------------------

CK_TEST(a_login_shell_is_named_with_the_leading_dash_and_given_no_arguments) {
    // This is the whole convention: login(1), every terminal emulator and
    // tmux all say "login shell" by putting a '-' on argv[0], and a shell
    // reads its profile files for no other reason.
    const ShellLaunch launch = shell_launch("/bin/zsh", /*login=*/true);
    CK_CHECK(launch.executable == "/bin/zsh");
    CK_CHECK(launch.argv0 == "-zsh");
    CK_CHECK(launch.arguments.empty());
}

CK_TEST(a_non_login_shell_keeps_its_own_name_and_is_told_it_is_interactive) {
    const ShellLaunch launch = shell_launch("/bin/zsh", /*login=*/false);
    CK_CHECK(launch.executable == "/bin/zsh");
    // Empty argv0 means "the executable path", which is what any program
    // that is not a shell expects to see.
    CK_CHECK(launch.argv0.empty());
    CK_CHECK(launch.arguments == std::vector<std::string>{"-i"});
}

CK_TEST(a_login_shell_named_by_a_deeper_path_is_still_dashed_by_its_own_name) {
    const ShellLaunch launch = shell_launch("/opt/homebrew/bin/fish", /*login=*/true);
    CK_CHECK(launch.argv0 == "-fish");
}

CK_TEST(a_shell_path_with_no_directory_at_all_still_produces_a_usable_name) {
    // Not reachable through resolve_shell(), which insists on an absolute
    // path — but shell_launch is a separate function and must not produce a
    // bare "-" for anything it is handed.
    CK_CHECK(shell_launch("zsh", /*login=*/true).argv0 == "-zsh");
    CK_CHECK(shell_launch("/bin/", /*login=*/true).argv0 == "-/bin/");
}

// --- Which shell that is ----------------------------------------------

CK_TEST(the_shell_is_the_one_the_reader_chose_in_their_environment) {
    ScopedShellEnvironment guard;
    ScopedShellEnvironment::set("/bin/sh");
    CK_CHECK(resolve_shell() == "/bin/sh");
}

CK_TEST(a_relative_or_missing_shell_in_the_environment_is_not_believed) {
    ScopedShellEnvironment guard;
    // tmux insists on an absolute path, because a relative one resolves
    // against a working directory the shell has not got yet.
    ScopedShellEnvironment::set("zsh");
    const std::string relative = resolve_shell();
    CK_CHECK(!relative.empty() && relative.front() == '/');

    ScopedShellEnvironment::set("/nonexistent/shell");
    const std::string missing = resolve_shell();
    CK_CHECK(missing != "/nonexistent/shell");
    CK_CHECK(::access(missing.c_str(), X_OK) == 0);
}

CK_TEST(a_shell_that_is_ckmux_itself_is_refused) {
    // A reader who has made ckmux their login shell would otherwise open a
    // multiplexer inside a multiplexer, without end and without a message
    // saying why. tmux guards the same case, by the same name test.
    ScopedShellEnvironment guard;
    ScratchDirectory scratch("recursive");
    CK_CHECK(!scratch.path().empty());
    const std::filesystem::path recursive = make_executable(scratch.path(), "ckmux");
    ScopedShellEnvironment::set(recursive.string());

    const std::string resolved = resolve_shell();
    CK_CHECK(resolved != recursive.string());
    CK_CHECK(::access(resolved.c_str(), X_OK) == 0);
}

CK_TEST(a_shell_that_is_merely_named_like_ckmux_elsewhere_is_still_refused_by_name) {
    // The guard is a name test, as tmux's is: two different paths can both
    // be a ckmux, and the one being started is the one that matters.
    ScopedShellEnvironment guard;
    ScratchDirectory scratch("named");
    CK_CHECK(!scratch.path().empty());
    const std::filesystem::path nested = make_executable(scratch.path() / "nested", "ckmux");
    ScopedShellEnvironment::set(nested.string());
    CK_CHECK(resolve_shell() != nested.string());
}

CK_TEST(a_shell_that_is_set_but_empty_in_the_environment_is_not_a_shell) {
    // Set-but-empty is how a shell says nothing, and believing it would hand
    // every terminal a launch spec with no program in it.
    ScopedShellEnvironment guard;
    ScopedShellEnvironment::set("");
    const std::string resolved = resolve_shell();
    CK_CHECK(!resolved.empty());
    CK_CHECK(resolved.front() == '/');
    CK_CHECK(::access(resolved.c_str(), X_OK) == 0);
}

CK_TEST(with_no_usable_shell_named_anywhere_the_posix_floor_is_used) {
    ScopedShellEnvironment guard;
    ScopedShellEnvironment::clear();
    // Without $SHELL the passwd entry answers on a normal account, so this
    // asserts the contract rather than one machine's value: whatever comes
    // back is an absolute path to something runnable.
    const std::string resolved = resolve_shell();
    CK_CHECK(!resolved.empty());
    CK_CHECK(resolved.front() == '/');
    CK_CHECK(::access(resolved.c_str(), X_OK) == 0);
}

CK_TEST(no_configured_shell_means_the_readers_own_shell) {
    // The default case, and the one that was broken: with no configuration file
    // there is no `[general] shell`, and a launch spec with an empty executable
    // launches nothing. Every terminal ckmux opened failed — in M1 and in the
    // server — and no test saw it because every test named a shell.
    const ckm::ShellLaunch launch = ckm::shell_launch("", true);
    CK_CHECK(!launch.executable.empty());
    CK_CHECK(launch.executable.front() == '/');
    CK_CHECK(launch.executable == ckm::resolve_shell());
    // And it is still a login shell: the dash on argv[0] names the shell that
    // was actually resolved, not the empty string it was asked with.
    CK_CHECK(launch.argv0.size() > 1);
    CK_CHECK(launch.argv0.front() == '-');
    CK_CHECK(launch.argv0.find('/') == std::string::npos);

    const ckm::ShellLaunch interactive = ckm::shell_launch("", false);
    CK_CHECK(interactive.executable == ckm::resolve_shell());
    CK_CHECK(interactive.arguments.size() == 1U);
}

// --- The other program ckmux starts: the clipboard helper --------------

CK_TEST(the_copied_text_reaches_the_helper_on_its_standard_input) {
    // On stdin and never on the command line: an argument is visible to every
    // process on the machine through `ps`, and what is being copied is by
    // definition something the reader selected.
    ScratchDirectory scratch("clipboard");
    CK_CHECK(!scratch.path().empty());
    const std::filesystem::path copied = scratch.path() / "copied";
    std::string diagnostics;
    CK_CHECK(ckm::platform::write_to_command("cat > '" + copied.string() + "'",
                                             "hello, clipboard", &diagnostics));
    CK_CHECK(contents_of(copied) == "hello, clipboard");
    // A helper that said nothing said nothing: an empty copy is not a message.
    CK_CHECK(diagnostics.empty());
}

CK_TEST(a_helper_that_says_something_says_it_to_ckmux_and_not_to_the_drawn_screen) {
    // The client is drawing a desktop on this process's stdout. A helper that
    // inherited it would print "command not found" into the frame, corrupting
    // whatever cells it landed on until something else redrew them — and the
    // one message a reader needed would be the one they could not read. So it
    // is captured, and comes back where a caller can put it somewhere sensible.
    std::string diagnostics;
    CK_CHECK(!ckm::platform::write_to_command("echo spoken; echo trouble >&2; exit 3", "text",
                                              &diagnostics));
    CK_CHECK(diagnostics.find("trouble") != std::string::npos);
    CK_CHECK(diagnostics.find("spoken") != std::string::npos);
}

CK_TEST(a_helper_that_cannot_be_run_at_all_is_a_failed_copy_rather_than_a_silent_one) {
    std::string diagnostics;
    CK_CHECK(!ckm::platform::write_to_command("exec /nonexistent/clipboard-helper", "text",
                                              &diagnostics));
    // /bin/sh says why, and the reason survives the fork rather than the frame.
    CK_CHECK(!diagnostics.empty());
    // And an empty command is refused before anything is forked at all.
    CK_CHECK(!ckm::platform::write_to_command("", "text"));
}

CK_TEST(a_helper_that_never_finishes_is_given_up_on_rather_than_freezing_the_client) {
    // The client is one thread around one loop: a helper that waits forever is
    // a multiplexer that stops drawing, stops reading the keyboard and stops
    // applying what the server sends, for as long as the helper feels like it.
    // `ssh elsewhere pbcopy` to a machine that has gone away is that helper.
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    CK_CHECK(!ckm::platform::write_to_command("sleep 30", "text", nullptr, /*idle_budget_ms=*/150));
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    // Generous by two orders of magnitude against the 30 seconds it was told to
    // wait: this is asserting that the deadline exists, not how sharp it is.
    CK_CHECK(waited < 5000);
}

CK_TEST(a_program_ckmux_starts_does_not_inherit_the_sigpipe_it_ignores_for_itself) {
    // The client and the server both ignore SIGPIPE so that a peer going away
    // mid-write cannot kill them, and an ignored disposition survives exec. A
    // shell that inherits it is a shell where pipelines stop working the way
    // every Unix program assumes they do: the writer is handed EPIPE forever
    // instead of being killed when the reader goes.
    //
    // The observable difference, and why this is a test and not a claim:
    // `cat /dev/zero` into a reader that stops after one byte is killed
    // silently when SIGPIPE is default, and prints a write error when SIGPIPE
    // is ignored. The helper's stderr is captured, so that error is visible
    // here — an empty capture is the child having been given SIG_DFL back.
    const ScopedIgnoredSigpipe ignored;
    CK_CHECK(ignored.swapped());

    std::string diagnostics;
    (void)ckm::platform::write_to_command(
        "cat /dev/zero | dd bs=1 count=1 of=/dev/null 2>/dev/null", "", &diagnostics);
    CK_CHECK(diagnostics.empty());
}
