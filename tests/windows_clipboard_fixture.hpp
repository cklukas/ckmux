// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include "cvision/term/windows_clipboard.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <optional>
#include <string>

namespace ckmtest {

inline std::optional<LUID> authentication_id(HANDLE process) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &token)) return std::nullopt;
    TOKEN_STATISTICS statistics{};
    DWORD required = 0;
    const BOOL queried = ::GetTokenInformation(token, TokenStatistics, &statistics,
                                               sizeof(statistics), &required);
    (void)::CloseHandle(token);
    if (!queried || required != sizeof(statistics)) return std::nullopt;
    return statistics.AuthenticationId;
}

inline bool same_authentication(LUID left, LUID right) {
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

// A default noninteractive logon station has its own clipboard. No fixture
// creates/sets a station and then assumes that a non-inheriting child follows
// it. Check the actual logon identity and the system-created default station.
// https://learn.microsoft.com/en-us/windows/win32/winstation/process-connection-to-a-window-station
// https://learn.microsoft.com/en-us/windows/win32/winstation/window-stations
struct ClipboardIsolation {
    std::optional<LUID> logon;
    std::wstring station_name;
    bool safe = false;

    ClipboardIsolation() {
        logon = authentication_id(::GetCurrentProcess());
        if (!logon) return;
        const HWINSTA station = ::GetProcessWindowStation();
        USEROBJECTFLAGS flags{};
        DWORD required = 0;
        std::array<wchar_t, 256> name{};
        if (!station || !::GetUserObjectInformationW(station, UOI_FLAGS, &flags,
                sizeof(flags), &required) || (flags.dwFlags & WSF_VISIBLE) != 0 ||
            !::GetUserObjectInformationW(station, UOI_NAME, name.data(),
                static_cast<DWORD>(sizeof(name)), &required)) return;
        std::array<wchar_t, 256> expected{};
        if (std::swprintf(expected.data(), expected.size(), L"Service-0x%x-%x$",
                static_cast<unsigned int>(logon->HighPart),
                static_cast<unsigned int>(logon->LowPart)) < 0) return;
        station_name = name.data();
        safe = station_name == expected.data();
        std::fprintf(stderr, "Clipboard fixture station=%ls visible=0 exact-logon-station=%d\n",
                     station_name.c_str(), safe ? 1 : 0);
    }

    bool includes(DWORD pid) const {
        if (!safe || !logon) return false;
        const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return false;
        const auto other = authentication_id(process);
        (void)::CloseHandle(process);
        const bool matches = other && same_authentication(*logon, *other);
        std::fprintf(stderr, "Clipboard fixture actual PID=%lu same-noninteractive-logon=%d\n",
                     static_cast<unsigned long>(pid), matches ? 1 : 0);
        return matches;
    }
};

inline std::optional<std::wstring> native_clipboard_text() {
    if (!::OpenClipboard(nullptr)) return std::nullopt;
    const HANDLE memory = ::GetClipboardData(CF_UNICODETEXT);
    const auto* text = memory ? static_cast<const wchar_t*>(::GlobalLock(memory)) : nullptr;
    std::optional<std::wstring> result;
    if (text) {
        const auto count = ::GlobalSize(memory) / sizeof(wchar_t);
        const auto* end = std::find(text, text + count, L'\0');
        if (end != text + count) result = std::wstring(text, end);
        (void)::GlobalUnlock(memory);
    }
    if (!::CloseClipboard()) return std::nullopt;
    return result;
}

struct ClipboardLock {
    bool held = ::OpenClipboard(nullptr) != FALSE;
    ~ClipboardLock() { if (held) (void)::CloseClipboard(); }
    bool release() {
        if (!held) return false;
        if (!::CloseClipboard()) return false;
        held = false;
        return true;
    }
};

} // namespace ckmtest
