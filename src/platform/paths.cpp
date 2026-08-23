// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/paths.hpp"

#include <cstdlib>
#include <string>

#include <pwd.h>
#include <unistd.h>

namespace ckm::platform {

const char* environment_value(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

const char* environment_directory(const char* name) {
    const char* const value = environment_value(name);
    return value != nullptr && *value == '/' ? value : nullptr;
}

std::filesystem::path config_file_path() {
    if (const char* const explicit_path = environment_value("CKMUX_CONFIG"))
        return std::filesystem::path(explicit_path);
    if (const char* const xdg = environment_directory("XDG_CONFIG_HOME"))
        return std::filesystem::path(xdg) / "ckmux" / "ckmux.conf";
    if (const char* const home = environment_directory("HOME"))
        return std::filesystem::path(home) / ".config" / "ckmux" / "ckmux.conf";
    // No HOME at all: a daemon-like environment. Returning a relative path
    // would write into whatever directory ckmux happened to start in, so
    // return nothing and let the caller treat it as "no configuration".
    return {};
}

std::filesystem::path home_directory() {
    // The database answer, which is about the user rather than about how this
    // process was started. `getpwuid` may fail — a network directory that is
    // unreachable, a uid with no entry — and that is what the fallbacks are.
    if (const ::passwd* const entry = ::getpwuid(::getuid());
        entry != nullptr && entry->pw_dir != nullptr && entry->pw_dir[0] == '/')
        return std::filesystem::path(entry->pw_dir);
    if (const char* const home = environment_directory("HOME"))
        return std::filesystem::path(home);
    // Somewhere that certainly exists, so that a terminal opens rather than
    // failing to launch over a directory nobody can be sure of.
    return std::filesystem::path("/");
}

std::filesystem::path expand_user_path(std::string_view path) {
    if (path.empty() || path.front() != '~') return std::filesystem::path(path);
    // `~user` is not this function's business: only a bare `~` or one followed
    // by a separator names THIS user's home.
    if (path.size() > 1 && path[1] != '/') return std::filesystem::path(path);

    // `$HOME` first — see the header. A shell expands `~` from the
    // environment, so an override that was made on purpose is honoured;
    // `environment_directory` rejects a relative value, which is what stops a
    // save resolving against whatever directory the client was started in.
    std::filesystem::path home;
    if (const char* const from_environment = environment_directory("HOME"))
        home = std::filesystem::path(from_environment);
    else
        home = home_directory();

    if (path.size() == 1) return home;
    // `substr(2)` skips the separator, so this appends a RELATIVE path:
    // `operator/` with an absolute right-hand side would discard `home`
    // entirely and hand back `/Documents`.
    return home / std::filesystem::path(path.substr(2));
}

}  // namespace ckm::platform
