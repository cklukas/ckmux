// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

// Sample the actual acceptance processes, not their source or a substitute
// renderer. This is sampled evidence, not a continuous kernel trace: handles
// or children shorter-lived than a sample interval can escape observation.
#include <processsnapshot.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ckmtest {

inline std::optional<std::vector<std::wstring>> named_handles(DWORD pid, HANDLE inspect = nullptr) {
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                        PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) return std::nullopt;
    HPSS snapshot = nullptr;
    const auto flags = static_cast<PSS_CAPTURE_FLAGS>(PSS_CAPTURE_HANDLES |
                                                     PSS_CAPTURE_HANDLE_NAME_INFORMATION);
    const DWORD captured = ::PssCaptureSnapshot(process, flags, 0, &snapshot);
    (void)::CloseHandle(process);
    if (captured != ERROR_SUCCESS) return std::nullopt;
    HPSSWALK marker = nullptr;
    if (::PssWalkMarkerCreate(nullptr, &marker) != ERROR_SUCCESS) {
        (void)::PssFreeSnapshot(::GetCurrentProcess(), snapshot);
        return std::nullopt;
    }
    std::vector<std::wstring> names;
    DWORD walked = ERROR_SUCCESS;
    for (;;) {
        PSS_HANDLE_ENTRY entry{};
        walked = ::PssWalkSnapshot(snapshot, PSS_WALK_HANDLES, marker, &entry, sizeof(entry));
        if (walked != ERROR_SUCCESS) break;
        if (entry.Handle == inspect) {
            std::fprintf(stderr, "Observer witness flags=%u type=%u type-name-bytes=%u name-bytes=%u\n",
                static_cast<unsigned int>(entry.Flags), static_cast<unsigned int>(entry.ObjectType),
                static_cast<unsigned int>(entry.TypeNameLength),
                static_cast<unsigned int>(entry.ObjectNameLength));
            if (entry.TypeName) std::fprintf(stderr, "Observer witness type=%.*ls\n",
                static_cast<int>(entry.TypeNameLength / sizeof(wchar_t)), entry.TypeName);
        }
        if ((entry.Flags & PSS_HANDLE_HAVE_NAME) && entry.ObjectName) {
            // PSS lengths are bytes, and these buffers need not be NUL terminated.
            if (entry.ObjectNameLength % sizeof(wchar_t) != 0) {
                walked = ERROR_INVALID_DATA;
                break;
            }
            names.emplace_back(entry.ObjectName, entry.ObjectNameLength / sizeof(wchar_t));
        }
    }
    const DWORD marker_freed = ::PssWalkMarkerFree(marker);
    const DWORD snapshot_freed = ::PssFreeSnapshot(::GetCurrentProcess(), snapshot);
    if (walked != ERROR_NO_MORE_ITEMS || marker_freed != ERROR_SUCCESS ||
        snapshot_freed != ERROR_SUCCESS) return std::nullopt;
    return names;
}

inline bool printer_endpoint(std::wstring name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    // Windows printer-port devices and the named spooler RPC endpoints. The
    // final prefix is a harmless fixture pipe used to prove this detector live.
    for (const auto prefix : {L"\\device\\parallel", L"\\device\\usbprint",
         L"\\device\\namedpipe\\spoolss", L"\\device\\namedpipe\\spooler",
         L"\\device\\namedpipe\\ckmux-observer-printer-"})
        if (name.starts_with(prefix)) return true;
    return false;
}

using ProcessParents = std::map<DWORD, DWORD>;

inline std::optional<ProcessParents> process_parents() {
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    ProcessParents parents;
    BOOL more = ::Process32FirstW(snapshot, &entry);
    while (more) {
        parents.emplace(entry.th32ProcessID, entry.th32ParentProcessID);
        more = ::Process32NextW(snapshot, &entry);
    }
    const DWORD error = ::GetLastError();
    (void)::CloseHandle(snapshot);
    if (error != ERROR_NO_MORE_FILES || parents.empty()) return std::nullopt;
    return parents;
}

inline std::set<DWORD> descendants(const ProcessParents& parents, std::set<DWORD> roots) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [pid, parent] : parents)
            if (roots.contains(parent) && roots.insert(pid).second) changed = true;
    }
    return roots;
}

struct PrintObserver {
    Reader& reader;
    std::set<DWORD> roots;
    std::set<DWORD> baseline;
    std::set<DWORD> unexpected_children;
    std::set<std::wstring> unexpected_endpoints;
    std::size_t samples = 0;
    std::size_t named_handle_samples = 0;
    bool healthy = true;

    PrintObserver(Reader& source, DWORD server)
        : reader(source), roots{server, static_cast<DWORD>(source.client->process_id())} {
        const auto census = process_parents();
        healthy = census.has_value();
        if (census) baseline = descendants(*census, roots);
        reader.after_drain = [this] { sample(); };
        sample();
    }
    ~PrintObserver() { reader.after_drain = {}; }
    PrintObserver(const PrintObserver&) = delete;
    PrintObserver& operator=(const PrintObserver&) = delete;

    void sample() {
        ++samples;
        const auto census = process_parents();
        if (!census) healthy = false;
        else for (const DWORD pid : descendants(*census, roots))
            if (!baseline.contains(pid)) unexpected_children.insert(pid);
        for (const DWORD pid : roots) {
            const auto handles = named_handles(pid);
            if (!handles || handles->empty()) { healthy = false; continue; }
            ++named_handle_samples;
            for (const auto& name : *handles)
                if (printer_endpoint(name)) unexpected_endpoints.insert(name);
        }
    }
};

} // namespace ckmtest
