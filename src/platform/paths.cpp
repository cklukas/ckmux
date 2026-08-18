// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "platform/paths.hpp"

#include <cstdlib>
#include <string>

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

}  // namespace ckm::platform
