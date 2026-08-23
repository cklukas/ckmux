// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/shell.hpp"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <string_view>

namespace ckm {
namespace {

std::string_view base_name(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) return path;
    const std::string_view tail = path.substr(slash + 1);
    // A path ending in '/' has no name to take; tmux falls back to the whole
    // string there rather than producing an empty argv[0].
    return tail.empty() ? path : tail;
}

// tmux's checkshell(): an absolute path, executable, and not this program.
bool usable_shell(const char* path) {
    if (path == nullptr || *path != '/') return false;
    if (base_name(path) == "ckmux") return false;
    return ::access(path, X_OK) == 0;
}

}  // namespace

std::string resolve_shell() {
    // $SHELL first: it is what the reader chose, and it is what every other
    // terminal on their machine honours.
    if (const char* const from_environment = std::getenv("SHELL"); usable_shell(from_environment))
        return from_environment;
    // Then the account's own shell. This is the case where ckmux was started
    // by something that does not export SHELL — a launcher, cron, an IDE —
    // and guessing /bin/sh would silently demote the reader's shell.
    if (const passwd* const entry = ::getpwuid(::getuid());
        entry != nullptr && usable_shell(entry->pw_shell))
        return entry->pw_shell;
    // Guaranteed to exist on a POSIX system. Not a preference; a floor.
    return "/bin/sh";
}

ShellLaunch shell_launch(const std::string& shell, bool login) {
    ShellLaunch launch;
    // No shell configured means the reader's own, which is what `[general]
    // shell` is documented to default to (the configuration spec) — and resolving it HERE is
    // the point: this is the one function both the client and the server reach
    // a shell through, and a caller that forgot would produce a launch spec with
    // no program in it. That is not a hypothetical: with no configuration file
    // at all, every terminal ckmux opened failed to launch, in M1 and in the
    // server, and nothing noticed because every test set a shell explicitly.
    launch.executable = shell.empty() ? resolve_shell() : shell;
    if (!login) {
        // Explicit rather than relying on the shell noticing its stdin is a
        // terminal: it always is here, but a shell that was told plainly is
        // a shell that cannot be talked out of it.
        launch.arguments = {"-i"};
        return launch;
    }
    // The dash, and nothing else, is what makes this a login shell. No
    // argument does it: -l is bash and zsh but not every shell, and the
    // convention every shell does agree on is the one login(1) established.
    launch.argv0 = "-" + std::string(base_name(launch.executable));
    return launch;
}

}  // namespace ckm
