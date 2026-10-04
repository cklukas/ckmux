// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/shell.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cvision/testing/cktest.hpp"

namespace {
ckm::ShellHost windows_host() {
    ckm::ShellHost host;
    host.platform = ckm::ShellPlatform::Windows;
    host.environment_shell = "C:/Windows/System32/cmd.exe";
    host.system_shell = "C:/Windows/System32/cmd.exe";
    host.account_shell = "/bin/sh"; // Must not leak POSIX fallback into Windows.
    host.usable_executable = [](std::string_view path) {
        return path == "C:/Windows/System32/cmd.exe" ||
               path == "D:\\Tools\\PowerShell\\pwsh.exe" ||
               path == "D:/Tools/PowerShell/PowerShell.EXE" ||
               path == "D:/Windows/System32/CMD.EXE";
    };
    return host;
}
}

CK_TEST(windows_shell_selection_prefers_valid_configuration_then_comspec_then_system_cmd) {
    auto host = windows_host();
    CK_CHECK(ckm::resolve_shell(host, "D:\\Tools\\PowerShell\\pwsh.exe") == "D:\\Tools\\PowerShell\\pwsh.exe");
    CK_CHECK(ckm::resolve_shell(host, "missing.exe") == host.environment_shell);
    CK_CHECK(ckm::resolve_shell(host) == host.environment_shell);
    host.environment_shell = "missing.exe";
    CK_CHECK(ckm::resolve_shell(host) == host.system_shell);
    host.system_shell = "missing.exe";
    CK_CHECK(ckm::resolve_shell(host).empty());
    host.usable_executable = {};
    CK_CHECK(ckm::resolve_shell(host, "D:\\Tools\\PowerShell\\pwsh.exe").empty());
}

CK_TEST(windows_shell_selection_observes_changed_environment_and_executable_availability) {
    auto host = windows_host();
    std::set<std::string> executable{host.system_shell, "D:\\Tools\\PowerShell\\pwsh.exe"};
    host.usable_executable = [&executable](std::string_view path) { return executable.contains(std::string(path)); };
    CK_CHECK(ckm::resolve_shell(host) == host.system_shell);
    host.environment_shell = "D:\\Tools\\PowerShell\\pwsh.exe";
    CK_CHECK(ckm::resolve_shell(host) == host.environment_shell);
    executable.erase(host.environment_shell);
    CK_CHECK(ckm::resolve_shell(host) == host.system_shell);
    executable.clear();
    CK_CHECK(ckm::resolve_shell(host).empty());
}

CK_TEST(windows_interactive_cmd_never_receives_posix_login_argv0_or_dash_i) {
    const auto host = windows_host();
    for (bool login : {false, true}) {
        const auto launch = ckm::shell_launch(host, "", login);
        CK_CHECK(launch.executable == host.system_shell);
        CK_CHECK(launch.argv0.empty());
        CK_CHECK(launch.arguments.empty());
        CK_CHECK(launch.windows_command.has_value());
        if (!launch.windows_command) return;
        CK_CHECK(!launch.windows_command->command);
        const auto spec = ckm::terminal_launch_spec(launch);
        CK_CHECK(spec.argv0.empty());
        CK_CHECK(spec.arguments.empty());
        CK_CHECK(spec.windows_command.has_value());
        if (!spec.windows_command) return;
        CK_CHECK(!spec.windows_command->command);
        CK_CHECK(spec.exit_policy == ckv::core::TerminalExitPolicy::Unspecified);
    }
}

CK_TEST(windows_command_preserves_syntax_for_the_explicit_ckvision_factory) {
    const auto host = windows_host();
    const std::string command = "echo \"Grüße 日本語\" & echo second";
    const auto launch = ckm::shell_launch(host, "D:/Windows/System32/CMD.EXE", true, command);
    CK_CHECK(launch.executable == "D:/Windows/System32/CMD.EXE");
    CK_CHECK(launch.argv0.empty());
    CK_CHECK(launch.arguments.empty());
    CK_CHECK(launch.windows_command.has_value());
    if (!launch.windows_command) return;
    CK_CHECK(launch.windows_command->command == command);
    const auto spec = ckm::terminal_launch_spec(launch);
    CK_CHECK(spec.windows_command.has_value());
    if (!spec.windows_command) return;
    CK_CHECK(spec.windows_command->command == command);
    CK_CHECK(spec.arguments.empty());
    CK_CHECK(spec.argv0.empty());
    CK_CHECK(spec.exit_policy == ckv::core::TerminalExitPolicy::Unspecified);
}

CK_TEST(windows_powershell_commands_remain_native_arguments_not_cmd_syntax) {
    const auto host = windows_host();
    const std::string command = "Write-Output 'Grüße 日本語'; exit 0";
    for (const std::string shell : {"D:\\Tools\\PowerShell\\pwsh.exe", "D:/Tools/PowerShell/PowerShell.EXE"}) {
        const auto launch = ckm::shell_launch(host, shell, false, command);
        CK_CHECK(launch.argv0.empty());
        CK_CHECK(!launch.windows_command);
        CK_CHECK(launch.arguments == std::vector<std::string>({"-NoLogo", "-NoProfile", "-Command", command}));
        const auto spec = ckm::terminal_launch_spec(launch);
        CK_CHECK(spec.arguments == launch.arguments);
        CK_CHECK(!spec.windows_command);
        CK_CHECK(spec.argv0.empty());
    }
    const auto interactive = ckm::shell_launch(host, "D:\\Tools\\PowerShell\\pwsh.exe", true);
    CK_CHECK(interactive.arguments == std::vector<std::string>({"-NoLogo"}));
}

CK_TEST(posix_shell_policy_preserves_login_interactive_and_command_launches) {
    ckm::ShellHost host;
    host.environment_shell = "/bin/zsh";
    host.account_shell = "/bin/bash";
    host.system_shell = "/bin/sh";
    host.usable_executable = [](std::string_view path) { return path == "/bin/zsh" || path == "/bin/bash"; };
    const auto login = ckm::shell_launch(host, "", true);
    CK_CHECK(login.executable == "/bin/zsh");
    CK_CHECK(login.argv0 == "-zsh");
    CK_CHECK(login.arguments.empty());
    const auto interactive = ckm::shell_launch(host, "", false);
    CK_CHECK(interactive.arguments == std::vector<std::string>({"-i"}));
    CK_CHECK(interactive.argv0.empty());
    const std::string text = "echo 'hello' && exit 0";
    const auto command = ckm::shell_launch(host, "", true, text);
    CK_CHECK(command.arguments == std::vector<std::string>({"-c", text}));
    CK_CHECK(command.argv0.empty());
    CK_CHECK(!command.windows_command);
    const auto spec = ckm::terminal_launch_spec(command);
    CK_CHECK(spec.arguments == command.arguments);
    CK_CHECK(!spec.windows_command);
    host.environment_shell = "relative";
    CK_CHECK(ckm::resolve_shell(host) == host.account_shell);
    host.account_shell.clear();
    CK_CHECK(ckm::resolve_shell(host) == "/bin/sh");
    CK_CHECK(ckm::resolve_shell(host, "custom") == "custom");
}
