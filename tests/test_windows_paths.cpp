// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/paths.hpp"
#include "cvision/testing/cktest.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

namespace {

class ScopedVariable {
public:
    ScopedVariable(const wchar_t* name, const wchar_t* value) : name_(name) {
        std::wstring before(32768, L'\0');
        ::SetLastError(ERROR_SUCCESS);
        const DWORD count = ::GetEnvironmentVariableW(name, before.data(),
                                                      static_cast<DWORD>(before.size()));
        present_ = count != 0 || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        before.resize(count);
        before_ = std::move(before);
        CK_CHECK(::SetEnvironmentVariableW(name, value) != 0);
    }
    ~ScopedVariable() { (void)::SetEnvironmentVariableW(name_.c_str(), present_ ? before_.c_str() : nullptr); }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
private:
    std::wstring name_;
    std::wstring before_;
    bool present_ = false;
};

std::filesystem::path folder(REFKNOWNFOLDERID id) {
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CK_CHECK(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE);
    struct ComScope {
        bool owned;
        ~ComScope() { if (owned) ::CoUninitialize(); }
    } com{SUCCEEDED(initialized)};
    PWSTR value = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &value);
    CK_CHECK(SUCCEEDED(result));
    const std::filesystem::path path(value ? value : L"");
    ::CoTaskMemFree(value);
    CK_CHECK(path.is_absolute());
    return path;
}

std::string utf8(std::wstring_view text) {
    const auto value = std::filesystem::path(text).u8string();
    return std::string(value.begin(), value.end());
}

}  // namespace

CK_TEST(windows_config_override_reads_unicode_and_changes_after_the_first_lookup) {
    ScopedVariable config(L"CKMUX_CONFIG", L"C:\\設定\\é-reader.conf");
    CK_CHECK(ckm::platform::config_file_path() == std::filesystem::path(L"C:\\設定\\é-reader.conf"));
    CK_CHECK(::SetEnvironmentVariableW(L"CKMUX_CONFIG", L"relative\\設定.conf") != 0);
    CK_CHECK(ckm::platform::config_file_path() == std::filesystem::path(L"relative\\設定.conf"));
}

CK_TEST(windows_config_defaults_to_absolute_local_app_data_not_xdg_or_home) {
    ScopedVariable config(L"CKMUX_CONFIG", nullptr);
    ScopedVariable data(L"LOCALAPPDATA", L"C:\\資料\\local");
    ScopedVariable xdg(L"XDG_CONFIG_HOME", L"C:\\wrong-xdg");
    ScopedVariable home(L"HOME", L"C:\\wrong-home");
    CK_CHECK(ckm::platform::config_file_path() == std::filesystem::path(L"C:\\資料\\local\\ckmux\\ckmux.conf"));
}

CK_TEST(windows_relative_missing_and_empty_local_data_use_the_real_known_folder) {
    ScopedVariable config(L"CKMUX_CONFIG", L"");
    ScopedVariable data(L"LOCALAPPDATA", L"relative\\local");
    const auto expected = folder(FOLDERID_LocalAppData) / L"ckmux" / L"ckmux.conf";
    CK_CHECK(ckm::platform::config_file_path() == expected);
    CK_CHECK(::SetEnvironmentVariableW(L"LOCALAPPDATA", L"") != 0);
    CK_CHECK(ckm::platform::config_file_path() == expected);
    CK_CHECK(::SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr) != 0);
    CK_CHECK(ckm::platform::config_file_path() == expected);
}

CK_TEST(windows_home_is_the_account_profile_not_a_client_environment_override) {
    ScopedVariable profile(L"USERPROFILE", L"C:\\not-the-account");
    CK_CHECK(ckm::platform::home_directory() == folder(FOLDERID_Profile));
}

CK_TEST(windows_tilde_accepts_both_separators_and_unicode_but_not_other_users) {
    ScopedVariable profile(L"USERPROFILE", L"C:\\讀者");
    CK_CHECK(ckm::platform::expand_user_path("~") == std::filesystem::path(L"C:\\讀者"));
    CK_CHECK(ckm::platform::expand_user_path(utf8(L"~/文書/é.txt")) == std::filesystem::path(L"C:\\讀者/文書/é.txt"));
    CK_CHECK(ckm::platform::expand_user_path(utf8(L"~\\文書\\é.txt")) == std::filesystem::path(L"C:\\讀者\\文書\\é.txt"));
    CK_CHECK(ckm::platform::expand_user_path("~other/file") == std::filesystem::path(L"~other/file"));
    CK_CHECK(ckm::platform::expand_user_path(utf8(L"relative\\文書.txt")) == std::filesystem::path(L"relative\\文書.txt"));
}

CK_TEST(windows_tilde_rejects_drive_relative_profiles_and_rooted_suffixes) {
    ScopedVariable profile(L"USERPROFILE", L"C:relative");
    CK_CHECK(ckm::platform::expand_user_path("~/Documents") == folder(FOLDERID_Profile) / L"Documents");
    CK_CHECK(ckm::platform::expand_user_path("~//outside").empty());
    CK_CHECK(ckm::platform::expand_user_path("~/D:relative").empty());
    CK_CHECK(ckm::platform::expand_user_path("~\\\\outside").empty());
}

CK_TEST(windows_paths_reject_invalid_utf8_and_embedded_nuls_without_throwing) {
    CK_CHECK(ckm::platform::expand_user_path(std::string("bad\0file", 8)).empty());
    CK_CHECK(ckm::platform::expand_user_path(std::string("bad\xff", 4)).empty());
    ScopedVariable profile(L"USERPROFILE", L"C:\\讀者");
    CK_CHECK(ckm::platform::expand_user_path(std::string("~/bad\xff", 6)).empty());
}

CK_TEST(windows_environment_paths_preserve_unc_and_ignore_empty_values) {
    ScopedVariable value(L"CKMUX_PATH_TEST", L"\\\\server\\share\\資料");
    CK_CHECK(ckm::platform::environment_path("CKMUX_PATH_TEST") == std::filesystem::path(L"\\\\server\\share\\資料"));
    CK_CHECK(::SetEnvironmentVariableW(L"CKMUX_PATH_TEST", L"") != 0);
    CK_CHECK(ckm::platform::environment_path("CKMUX_PATH_TEST").empty());
}
