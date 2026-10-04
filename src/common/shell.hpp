// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Shell selection and launch policy. Host observations are injected; native
// command-line encoding belongs to ckVision, never to this policy.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/shell_host.hpp"
#include "cvision/core/terminal_subsession.hpp"

namespace ckm {

struct ShellLaunch {
    std::string executable;
    std::string argv0;
    std::vector<std::string> arguments;
    std::optional<ckv::core::WindowsCommandProcessorLaunch> windows_command;
};

// Windows: valid configured executable, ComSpec, system cmd.exe. POSIX:
// configured shell as supplied, otherwise usable SHELL, account shell, /bin/sh.
// Missing/refused candidates yield an empty executable, not a relative guess.
std::string resolve_shell(const ShellHost& host, std::string_view configured = {});
std::string resolve_shell();

// POSIX login mode names argv[0] with '-'; non-login uses '-i'. A requested
// command overrides both with '-c'. Windows never invents a POSIX argv[0]:
// cmd uses the explicit library form for interactive and command launches; PowerShell uses
// -NoLogo -NoProfile -Command; other explicitly selected shells use -c.
// login-shell is POSIX-only; interactive PowerShell keeps native profiles.
ShellLaunch shell_launch(const ShellHost& host, const std::string& shell, bool login,
                         std::string_view command = {});
ShellLaunch shell_launch(const std::string& shell, bool login, std::string_view command = {});
ckv::core::TerminalLaunchSpec terminal_launch_spec(const ShellLaunch& launch);

}  // namespace ckm
