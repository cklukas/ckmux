// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include "scratch_directory.hpp"

namespace ckmtest {

// One owned, unique namespace per suite process. Windows resolves the simple
// endpoint name into its user-scoped pipe namespace; Unix binds an actual
// socket inside this directory. Neither uses the host's default temp root.
inline const ScratchDirectory& server_scratch() {
    static const ScratchDirectory scratch("w");
    return scratch;
}

inline std::filesystem::path server_endpoint(std::string_view name) {
#if defined(_WIN32)
    return server_scratch().path().filename().string() + "-" + std::string(name);
#else
    return server_scratch().path() / (std::string(name) + ".sock");
#endif
}

inline void forget_server_endpoint(const std::filesystem::path& endpoint) {
#if defined(_WIN32)
    // Named pipes/mutexes are lifetime-owned native objects, not disk files.
    (void)endpoint;
#else
    if (endpoint.parent_path() != server_scratch().path())
        throw std::invalid_argument("server fixture endpoint escaped its scratch directory");
    std::error_code ignored;
    std::filesystem::remove(endpoint, ignored);
    std::filesystem::remove(std::filesystem::path(endpoint.string() + ".lock"), ignored);
#endif
}

inline std::string server_fixture_shell() {
#if defined(_WIN32)
    return CKMUX_TEST_CHILD_PATH;
#else
    return "/bin/sh";
#endif
}

inline std::string server_idle_command() {
#if defined(_WIN32)
    return "--idle";
#else
    return "/bin/sh";
#endif
}

inline std::string server_echo_command() {
#if defined(_WIN32)
    return "--echo";
#else
    return "/bin/cat";
#endif
}

inline std::string server_working_directory() {
    const auto path = server_scratch().path().u8string();
    return std::string(path.begin(), path.end());
}

} // namespace ckmtest
