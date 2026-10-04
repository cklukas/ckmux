// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "common/shell.hpp"
#include "cvision/term/terminal_subsession.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"

namespace {
std::string path_text(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

class ScopedComspec final {
public:
    ScopedComspec() : previous_(32768, L'\0') {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD count = ::GetEnvironmentVariableW(L"ComSpec", previous_.data(), static_cast<DWORD>(previous_.size()));
        if (count >= previous_.size()) throw std::runtime_error("ComSpec exceeds the test's native bound");
        had_value_ = count > 0 || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        previous_.resize(count);
    }
    ~ScopedComspec() { (void)::SetEnvironmentVariableW(L"ComSpec", had_value_ ? previous_.c_str() : nullptr); }
    ScopedComspec(const ScopedComspec&) = delete;
    ScopedComspec& operator=(const ScopedComspec&) = delete;
private:
    std::wstring previous_;
    bool had_value_ = false;
};

std::string screen_text(ckv::term::TerminalSubsession& session) {
    std::string text;
    for (const auto& cell : session.snapshot().cell_buffer)
        if (!cell.is_continuation()) text += cell.grapheme();
    return text;
}

bool pump(ckv::term::TerminalSubsession& session, std::string_view marker, bool exited = false) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<HANDLE, 8> handles{};
        DWORD count = 0;
        for (const auto handle : session.wait_handles()) {
            if (handle.kind != ckv::term::WaitHandleKind::WindowsHandle) continue;
            CK_CHECK(count < handles.size());
            if (count >= handles.size()) return false;
            handles[count++] = reinterpret_cast<HANDLE>(handle.value);
        }
        if (count) (void)::WaitForMultipleObjects(count, handles.data(), FALSE, 20);
        else ::Sleep(1);
        (void)session.drain(64 * 1024);
        if (session.state() == ckv::term::TerminalSubsessionState::Failed) return false;
        if (exited) {
            if (session.state() == ckv::term::TerminalSubsessionState::Exited) return true;
        } else if (screen_text(session).find(marker) != std::string::npos) return true;
    }
    return false;
}

ckv::term::TerminalLaunchSpec native_spec(const ckm::ShellLaunch& shell, const std::filesystem::path& directory) {
    auto spec = ckm::terminal_launch_spec(shell);
    spec.working_directory = path_text(directory);
    spec.profile.cells = {120, 12};
    spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
    return spec;
}
}

CK_TEST(native_shell_host_validates_executables_and_rejects_relative_invalid_and_recursive_paths) {
    const auto host = ckm::platform::shell_host();
    CK_CHECK(host.platform == ckm::ShellPlatform::Windows);
    CK_CHECK(!host.system_shell.empty());
    CK_CHECK(host.usable_executable(host.system_shell));
    CK_CHECK(!host.usable_executable("cmd.exe"));
    CK_CHECK(!host.usable_executable("C:/missing/cmd.exe"));
    CK_CHECK(!host.usable_executable("C:/Windows/System32"));
    CK_CHECK(!host.usable_executable(host.system_shell + std::string("\0other", 6)));
    CK_CHECK(!host.usable_executable(std::string("\xff", 1)));
    ckmtest::ScratchDirectory scratch("native-shell-validation");
    const auto cmd = std::filesystem::path(std::u8string(host.system_shell.begin(), host.system_shell.end()));
    const auto recursive = scratch.path() / L"CKMUX.EXE";
    std::filesystem::copy_file(cmd, recursive);
    CK_CHECK(!host.usable_executable(path_text(recursive)));
}

CK_TEST(native_ui_fixture_has_a_real_silent_live_child_and_observes_its_response) {
    ckmtest::ScratchDirectory scratch("native-ui-child");
    const auto shell = ckm::shell_launch(CKMUX_TEST_CHILD_PATH, false);
    auto spec = native_spec(shell, scratch.path());
    auto child = ckv::term::launch_terminal_subsession(spec);
    CK_CHECK(child != nullptr);
    if (!child) return;
    // The portable emulator is Ready until the child emits its first bytes.
    // A silent live process must not be required to print a prompt just to
    // satisfy the fixture; the process id and transformed reply prove life.
    CK_CHECK(child->state() == ckv::term::TerminalSubsessionState::Ready);
    CK_CHECK(child->process_id() > 0);
    child->send_input("native-fixture-response\r");
    CK_CHECK(pump(*child, "NATIVE-FIXTURE-RESPONSE"));
    CK_CHECK(child->state() == ckv::term::TerminalSubsessionState::Running);
}

CK_TEST(native_comspec_changes_are_observed_and_unicode_shell_paths_run_commands) {
    ScopedComspec guard;
    const auto original = ckm::platform::shell_host();
    CK_CHECK(::SetEnvironmentVariableW(L"ComSpec", L"missing.exe") != 0);
    CK_CHECK(ckm::resolve_shell() == original.system_shell);
    ckmtest::ScratchDirectory scratch("native-comspec");
    const auto directory = scratch.path() / std::filesystem::path(std::u8string(u8"Grüße 日本語 & bin"));
    std::filesystem::create_directory(directory);
    const auto copied = directory / L"cmd.exe";
    const auto system_cmd = std::filesystem::path(std::u8string(original.system_shell.begin(), original.system_shell.end()));
    std::filesystem::copy_file(system_cmd, copied);
    // cmd is not a standalone test executable: keep its installed language
    // resources beside the relocated image. Relocating only the PE image
    // produced native resource errors, even though the same system cmd worked
    // from this Unicode cwd with the identical launch form.
    std::size_t resource_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(system_cmd.parent_path())) {
        std::error_code error;
        if (!entry.is_directory(error) || error) continue;
        const auto resource = entry.path() / L"cmd.exe.mui";
        if (!std::filesystem::is_regular_file(resource, error) || error) continue;
        const auto destination = directory / entry.path().filename();
        std::filesystem::create_directory(destination);
        std::filesystem::copy_file(resource, destination / L"cmd.exe.mui");
        ++resource_files;
    }
    CK_CHECK(resource_files > 0);
    CK_CHECK(::SetEnvironmentVariableW(L"ComSpec", copied.c_str()) != 0);
    CK_CHECK(ckm::resolve_shell() == path_text(copied));
    const auto shell = ckm::shell_launch("", true, "echo CKMUX-NATIVE-CMD & exit /b 37");
    CK_CHECK(shell.executable == path_text(copied));
    CK_CHECK(shell.windows_command.has_value());
    auto session = ckv::term::launch_terminal_subsession(native_spec(shell, directory));
    CK_CHECK(session->state() != ckv::term::TerminalSubsessionState::Failed);
    CK_CHECK(pump(*session, {}, true));
    CK_CHECK(session->status().exit_code == 37);
    const auto output = screen_text(*session);
    if (output.find("CKMUX-NATIVE-CMD") == std::string::npos)
        std::fprintf(stderr, "native cmd captured %zu bytes: <%s>\n", output.size(), output.c_str());
    CK_CHECK(output.find("CKMUX-NATIVE-CMD") != std::string::npos);
    const auto control_shell = ckm::shell_launch(original.system_shell, true, "echo CKMUX-NATIVE-CONTROL & exit /b 37");
    auto control = ckv::term::launch_terminal_subsession(native_spec(control_shell, directory));
    CK_CHECK(pump(*control, {}, true));
    const auto control_output = screen_text(*control);
    CK_CHECK(control->status().exit_code == 37);
    CK_CHECK(control_output.find("CKMUX-NATIVE-CONTROL") != std::string::npos);
}

CK_TEST(native_interactive_default_cmd_accepts_input_resize_and_preserves_exit_status) {
    ScopedComspec guard;
    const auto host = ckm::platform::shell_host();
    const auto cmd = std::filesystem::path(std::u8string(host.system_shell.begin(), host.system_shell.end()));
    CK_CHECK(::SetEnvironmentVariableW(L"ComSpec", cmd.c_str()) != 0);
    const auto shell = ckm::shell_launch("", true);
    CK_CHECK(shell.argv0.empty());
    CK_CHECK(shell.arguments.empty());
    auto session = ckv::term::launch_terminal_subsession(native_spec(shell, cmd.parent_path()));
    CK_CHECK(session->state() != ckv::term::TerminalSubsessionState::Failed);
    session->send_input("echo CKMUX-INTERACTIVE\r");
    CK_CHECK(pump(*session, "CKMUX-INTERACTIVE"));
    session->resize({80, 20}, {9, 18});
    CK_CHECK(session->snapshot().cells.width == 80);
    CK_CHECK(session->snapshot().cells.height == 20);
    session->send_input("exit 19\r");
    CK_CHECK(pump(*session, {}, true));
    CK_CHECK(session->status().exit_code == 19);
}

CK_TEST(native_command_processor_preserves_commands_across_image_paths_and_comspec_changes) {
    ScopedComspec guard;
    const auto host = ckm::platform::shell_host();
    const auto system_cmd = std::filesystem::path(std::u8string(host.system_shell.begin(), host.system_shell.end()));
    ckmtest::ScratchDirectory scratch("native-cmd-matrix");
    for (const auto& name : {std::u8string(u8"ASCII plain"), std::u8string(u8"ASCII & quoted"),
                            std::u8string(u8"Unicode ü日本語 plain"), std::u8string(u8"Unicode ü日本語 & quoted")}) {
        const auto directory = scratch.path() / std::filesystem::path(name);
        std::filesystem::create_directory(directory);
        const auto copy = directory / L"cmd.exe";
        std::filesystem::copy_file(system_cmd, copy);
        for (const auto& entry : std::filesystem::directory_iterator(system_cmd.parent_path())) {
            std::error_code error;
            if (!entry.is_directory(error) || error) continue;
            const auto resource = entry.path() / L"cmd.exe.mui";
            if (!std::filesystem::is_regular_file(resource, error) || error) continue;
            const auto destination = directory / entry.path().filename();
            std::filesystem::create_directory(destination);
            std::filesystem::copy_file(resource, destination / L"cmd.exe.mui");
        }
        for (const auto& comspec : {system_cmd, copy}) {
            CK_CHECK(::SetEnvironmentVariableW(L"ComSpec", comspec.c_str()) != 0);
            const auto shell = ckm::shell_launch(path_text(copy), true, "echo CKMUX-MATRIX & exit /b 37");
            const auto spec = native_spec(shell, directory);
            auto session = ckv::term::launch_terminal_subsession(spec);
            const bool exited = pump(*session, {}, true);
            const auto output = screen_text(*session);
            std::fprintf(stderr, "MATRIX image=%s ComSpec=%s argv0=%s command=%s exited=%d exit=%d marker=%d output=<%s>\n",
                spec.executable.c_str(), path_text(comspec).c_str(), spec.argv0.c_str(),
                spec.windows_command && spec.windows_command->command ? spec.windows_command->command->c_str() : "<interactive>",
                exited ? 1 : 0, session->status().exit_code.value_or(-1),
                output.find("CKMUX-MATRIX") != std::string::npos ? 1 : 0, output.c_str());
            CK_CHECK(exited);
            CK_CHECK(session->status().exit_code == 37);
            CK_CHECK(output.find("CKMUX-MATRIX") != std::string::npos);
            // Positive comparison spelling, not a fallback: the original
            // mixed-separator launch above must independently pass.
            auto native_path_spec = spec;
            auto native_path = copy;
            native_path.make_preferred();
            native_path_spec.executable = path_text(native_path);
            auto native_path_session = ckv::term::launch_terminal_subsession(native_path_spec);
            CK_CHECK(pump(*native_path_session, {}, true));
            const auto native_output = screen_text(*native_path_session);
            std::fprintf(stderr, "NATIVE-SEPARATORS image=%s exit=%d marker=%d\n",
                native_path_spec.executable.c_str(), native_path_session->status().exit_code.value_or(-1),
                native_output.find("CKMUX-MATRIX") != std::string::npos ? 1 : 0);
            CK_CHECK(native_path_session->status().exit_code == 37);
            CK_CHECK(native_output.find("CKMUX-MATRIX") != std::string::npos);
        }
    }
}

CK_TEST(native_explicit_powershell_runs_quoted_unicode_command_with_no_cmd_reencoding) {
    const auto host = ckm::platform::shell_host();
    const auto cmd = std::filesystem::path(std::u8string(host.system_shell.begin(), host.system_shell.end()));
    const auto powershell = cmd.parent_path() / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    CK_CHECK(host.usable_executable(path_text(powershell)));
    const auto shell = ckm::shell_launch(path_text(powershell), true, "Write-Output 'CKMUX-PS 日本語'; exit 0");
    CK_CHECK(shell.executable == path_text(powershell));
    CK_CHECK(!shell.windows_command);
    auto session = ckv::term::launch_terminal_subsession(native_spec(shell, cmd.parent_path()));
    CK_CHECK(session->state() != ckv::term::TerminalSubsessionState::Failed);
    CK_CHECK(pump(*session, {}, true));
    CK_CHECK(session->status().exit_code == 0);
    CK_CHECK(screen_text(*session).find("CKMUX-PS 日本語") != std::string::npos);
}
