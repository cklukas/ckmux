// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/shell.hpp"

namespace ckm {
namespace {

std::string_view base_name(std::string_view path, ShellPlatform platform) {
    const std::size_t separator = platform == ShellPlatform::Windows
                                      ? path.find_last_of("/\\") : path.rfind('/');
    if (separator == std::string_view::npos) return path;
    const std::string_view tail = path.substr(separator + 1);
    return tail.empty() ? path : tail;
}

bool ascii_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const char c = left[index];
        const char folded = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        if (folded != right[index]) return false;
    }
    return true;
}

bool usable(const ShellHost& host, std::string_view candidate) {
    return !candidate.empty() && host.usable_executable && host.usable_executable(candidate);
}

}  // namespace

std::string resolve_shell(const ShellHost& host, std::string_view configured) {
    if (!configured.empty() && (host.platform == ShellPlatform::Posix || usable(host, configured)))
        return std::string(configured);
    if (usable(host, host.environment_shell)) return host.environment_shell;
    if (host.platform == ShellPlatform::Posix && usable(host, host.account_shell)) return host.account_shell;
    // /bin/sh is POSIX's guaranteed floor; Windows' system executable still
    // needs validation because the host query or file access can fail.
    if (host.platform == ShellPlatform::Posix || usable(host, host.system_shell)) return host.system_shell;
    return {};
}

std::string resolve_shell() { return resolve_shell(platform::shell_host()); }

ShellLaunch shell_launch(const ShellHost& host, const std::string& shell, bool login,
                         std::string_view command) {
    ShellLaunch launch;
    launch.executable = resolve_shell(host, shell);
    if (launch.executable.empty()) return launch;
    if (host.platform == ShellPlatform::Windows) {
        const std::string_view name = base_name(launch.executable, host.platform);
        const bool cmd = ascii_equal(name, "cmd.exe") || ascii_equal(name, "cmd");
        const bool powershell = ascii_equal(name, "powershell.exe") || ascii_equal(name, "powershell") ||
                                ascii_equal(name, "pwsh.exe") || ascii_equal(name, "pwsh");
        if (cmd) {
            launch.windows_command = ckv::core::WindowsCommandProcessorLaunch{};
            if (!command.empty()) launch.windows_command->command = std::string(command);
        } else if (!command.empty()) {
            if (powershell) launch.arguments = {"-NoLogo", "-NoProfile", "-Command", std::string(command)};
            else launch.arguments = {"-c", std::string(command)};
        } else if (powershell) launch.arguments = {"-NoLogo"};
        return launch;
    }
    if (!command.empty()) launch.arguments = {"-c", std::string(command)};
    else if (!login) launch.arguments = {"-i"};
    else launch.argv0 = "-" + std::string(base_name(launch.executable, host.platform));
    return launch;
}

ShellLaunch shell_launch(const std::string& shell, bool login, std::string_view command) {
    return shell_launch(platform::shell_host(), shell, login, command);
}

ckv::core::TerminalLaunchSpec terminal_launch_spec(const ShellLaunch& launch) {
    ckv::core::TerminalLaunchSpec spec = launch.windows_command
        ? ckv::core::TerminalLaunchSpec::windows_command_processor(launch.executable, launch.windows_command->command)
        : ckv::core::TerminalLaunchSpec::program(launch.executable, launch.arguments);
    spec.argv0 = launch.argv0;
    return spec;
}

}  // namespace ckm
