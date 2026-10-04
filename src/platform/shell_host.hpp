// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace ckm {

enum class ShellPlatform { Posix, Windows };

// Native observations and an injected executable query. The common policy
// consumes these without reading the host environment or filesystem itself.
struct ShellHost {
    ShellPlatform platform = ShellPlatform::Posix;
    std::string environment_shell;
    std::string account_shell;
    std::string system_shell;
    std::function<bool(std::string_view)> usable_executable;
};

namespace platform {
// Fresh observations on every call, never a permanently cached environment.
ShellHost shell_host();
}  // namespace platform
}  // namespace ckm
