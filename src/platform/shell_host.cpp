// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/shell_host.hpp"
#include "cvision/core/utf8.hpp"

#if defined(_WIN32)
#include "cvision/term/windows_process_image.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace ckm::platform {
namespace {

#if defined(_WIN32)
std::string path_text(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool usable_executable(std::string_view text) {
    if (text.empty() || text.find('\0') != std::string_view::npos || !ckv::utf8::is_valid(text)) return false;
    try {
        const std::filesystem::path path(std::u8string(text.begin(), text.end()));
        if (!path.is_absolute()) return false;
        std::wstring name = path.filename().native();
        for (wchar_t& c : name) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
        if (name == L"ckmux.exe" || name == L"ckmux") return false;
        return ckv::term::inspect_windows_process_image(text).compatible();
    } catch (const std::system_error&) { return false; }
}

std::string environment_shell() {
    // The documented maximum includes no terminator. One bounded read avoids
    // a size-query race while allowing the full native environment value.
    std::wstring value(32768, L'\0');
    const DWORD count = ::GetEnvironmentVariableW(L"ComSpec", value.data(), static_cast<DWORD>(value.size()));
    if (count == 0 || count >= value.size()) return {};
    value.resize(count);
    try { return path_text(std::filesystem::path(value)); }
    catch (const std::system_error&) { return {}; }
}

std::string system_shell() {
    std::wstring value(32768, L'\0');
    const UINT count = ::GetSystemDirectoryW(value.data(), static_cast<UINT>(value.size()));
    if (count == 0 || count >= value.size()) return {};
    value.resize(count);
    try { return path_text(std::filesystem::path(value) / L"cmd.exe"); }
    catch (const std::system_error&) { return {}; }
}
#else
bool usable_executable(std::string_view text) {
    if (text.empty() || text.front() != '/' || text.find('\0') != std::string_view::npos) return false;
    const std::size_t separator = text.rfind('/');
    if (text.substr(separator + 1) == "ckmux") return false;
    return ::access(std::string(text).c_str(), X_OK) == 0;
}
#endif

}  // namespace

ShellHost shell_host() {
    ShellHost host;
    host.usable_executable = usable_executable;
#if defined(_WIN32)
    host.platform = ShellPlatform::Windows;
    host.environment_shell = environment_shell();
    host.system_shell = system_shell();
#else
    host.platform = ShellPlatform::Posix;
    if (const char* const value = std::getenv("SHELL")) host.environment_shell = value;
    if (const passwd* const entry = ::getpwuid(::getuid()); entry && entry->pw_shell)
        host.account_shell = entry->pw_shell;
    host.system_shell = "/bin/sh";
#endif
    return host;
}

}  // namespace ckm::platform
