// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "platform/paths.hpp"

#include <cstdlib>
#include <string>

namespace ckm::platform {
namespace {

// An environment variable that is set but empty is not a value. Treating it
// as one is how a program comes to look for its configuration in "/ckmux".
const char* value_of(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

}  // namespace

std::filesystem::path config_file_path() {
    if (const char* const explicit_path = value_of("CKMUX_CONFIG")) return std::filesystem::path(explicit_path);
    if (const char* const xdg = value_of("XDG_CONFIG_HOME"))
        return std::filesystem::path(xdg) / "ckmux" / "ckmux.conf";
    if (const char* const home = value_of("HOME"))
        return std::filesystem::path(home) / ".config" / "ckmux" / "ckmux.conf";
    // No HOME at all: a daemon-like environment. Returning a relative path
    // would write into whatever directory ckmux happened to start in, so
    // return nothing and let the caller treat it as "no configuration".
    return {};
}

}  // namespace ckm::platform
