// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The shell a terminal runs and the shape it is started in. Both are tmux's
// answers (plans/08-configuration.md), so these tests pin the behaviour a
// reader arriving from tmux already expects rather than our own invention.
#include "common/shell.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

std::filesystem::path scratch_directory() {
    return std::filesystem::temp_directory_path() / "ckmux-shell-test";
}

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
    const std::filesystem::path recursive = make_executable(scratch_directory(), "ckmux");
    ScopedShellEnvironment::set(recursive.string());

    const std::string resolved = resolve_shell();
    CK_CHECK(resolved != recursive.string());
    CK_CHECK(::access(resolved.c_str(), X_OK) == 0);

    std::filesystem::remove_all(scratch_directory());
}

CK_TEST(a_shell_that_is_merely_named_like_ckmux_elsewhere_is_still_refused_by_name) {
    // The guard is a name test, as tmux's is: two different paths can both
    // be a ckmux, and the one being started is the one that matters.
    ScopedShellEnvironment guard;
    const std::filesystem::path nested = make_executable(scratch_directory() / "nested", "ckmux");
    ScopedShellEnvironment::set(nested.string());
    CK_CHECK(resolve_shell() != nested.string());
    std::filesystem::remove_all(scratch_directory());
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
