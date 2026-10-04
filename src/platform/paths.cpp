// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/paths.hpp"

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <system_error>
#include "cvision/core/utf8.hpp"
#include "cvision/term/windows_text.hpp"
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace ckm::platform {

std::string path_text(const std::filesystem::path& path) {
#if defined(_WIN32)
    return ckv::term::windows_utf8(path.native()).value_or(std::string{});
#else
    return path.native();
#endif
}

#if !defined(_WIN32)
const char* environment_value(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

const char* environment_directory(const char* name) {
    const char* const value = environment_value(name);
    return value != nullptr && *value == '/' ? value : nullptr;
}
#endif

std::filesystem::path environment_path(const char* name) {
#if defined(_WIN32)
    // Names in this host policy are ASCII; values are always read as UTF-16.
    // One bounded read avoids a size-query race. Win32's documented maximum
    // is 32767 characters, not including the terminator.
    std::wstring wide_name;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(name); *c; ++c) {
        if (*c > 127) return {};
        wide_name.push_back(static_cast<wchar_t>(*c));
    }
    std::wstring value(32768, L'\0');
    const DWORD count = ::GetEnvironmentVariableW(wide_name.c_str(), value.data(),
                                                 static_cast<DWORD>(value.size()));
    if (count == 0 || count >= value.size()) return {};
    value.resize(count);
    return std::filesystem::path(value);
#else
    const char* const value = environment_value(name);
    return value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}

#if defined(_WIN32)
namespace {

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // An existing MTA is already initialized and must not be uninitialized
    // here. Any other initialization failure makes the lookup unavailable.
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {};
    struct ComScope {
        bool owned;
        ~ComScope() { if (owned) ::CoUninitialize(); }
    } com{SUCCEEDED(initialized)};
    struct FolderStorage {
        PWSTR value = nullptr;
        ~FolderStorage() { ::CoTaskMemFree(value); }
    } raw;
    const HRESULT result = ::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw.value);
    std::filesystem::path folder;
    if (SUCCEEDED(result) && raw.value != nullptr) folder = std::filesystem::path(raw.value);
    return folder.is_absolute() ? folder : std::filesystem::path{};
}

std::filesystem::path native_path(std::string_view text) {
    if (text.find('\0') != std::string_view::npos || !ckv::utf8::is_valid(text)) return {};
    try { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
    catch (const std::system_error&) { return {}; }
}

}  // namespace
#endif

std::filesystem::path config_file_path() {
    if (const auto explicit_path = environment_path("CKMUX_CONFIG"); !explicit_path.empty())
        return explicit_path;
#if defined(_WIN32)
    auto data = environment_path("LOCALAPPDATA");
    if (!data.is_absolute()) data = known_folder(FOLDERID_LocalAppData);
    return data.empty() ? std::filesystem::path{} : data / L"ckmux" / L"ckmux.conf";
#else
    if (const char* const xdg = environment_directory("XDG_CONFIG_HOME"))
        return std::filesystem::path(xdg) / "ckmux" / "ckmux.conf";
    if (const char* const home = environment_directory("HOME"))
        return std::filesystem::path(home) / ".config" / "ckmux" / "ckmux.conf";
    // No HOME at all: a daemon-like environment. Returning a relative path
    // would write into whatever directory ckmux happened to start in, so
    // return nothing and let the caller treat it as "no configuration".
    return {};
#endif
}

std::filesystem::path home_directory() {
#if defined(_WIN32)
    // Like the Unix account database, this describes the current account,
    // not an arbitrary environment left by the client that started a server.
    auto home = known_folder(FOLDERID_Profile);
    if (!home.empty()) return home;
    home = environment_path("USERPROFILE");
    return home.is_absolute() ? home : std::filesystem::path{};
#else
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
#endif
}

std::filesystem::path expand_user_path(std::string_view path) {
#if defined(_WIN32)
    if (path.empty() || path.front() != '~') return native_path(path);
    if (path.size() > 1 && path[1] != '/' && path[1] != '\\') return native_path(path);
    auto home = environment_path("USERPROFILE");
    if (!home.is_absolute()) home = home_directory();
    if (home.empty()) return {};
    if (path.size() == 1) return home;
    const auto rest = native_path(path.substr(2));
    if (rest.empty() && path.size() > 2) return {};
    // A second separator or drive-qualified suffix cannot discard the chosen
    // home. Reject it rather than resolving an unexpected absolute save path.
    if (rest.has_root_path()) return {};
    return home / rest;
#else
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
#endif
}

}  // namespace ckm::platform
