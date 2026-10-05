// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

// Sample the actual acceptance processes, not their source or a substitute
// renderer. This is sampled evidence, not a continuous kernel trace: handles
// or children shorter-lived than a sample interval can escape observation.
#include <processsnapshot.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ckmtest {

inline std::optional<std::wstring> file_object_name(HANDLE process, HANDLE remote) {
    // PSS enumerates File handles but does not supply their object names on
    // the native host. Duplicate only a File handle, never close its source,
    // and query the name in this process. This is Microsoft's documented
    // diagnostic pattern, not a production dependency on NT internals:
    // https://devblogs.microsoft.com/scripting/weekend-scripter-determine-process-that-locks-a-file/
    HANDLE duplicate = nullptr;
    if (!::DuplicateHandle(process, remote, ::GetCurrentProcess(), &duplicate, 0,
                           FALSE, DUPLICATE_SAME_ACCESS)) {
        const DWORD error = ::GetLastError();
        DWORD exit_code = 0;
        if (error == ERROR_INVALID_HANDLE && ::GetProcessId(process) != 0 &&
            ::GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
            // PSS recorded this handle before we queried the live table.
            // A closed handle has no live endpoint to inspect. This observer
            // is sampled, not a kernel trace; other errors remain fatal.
            return std::wstring{};
        }
        std::fprintf(stderr, "Observer duplicate File handle %p failed: %lu\n", remote,
                     static_cast<unsigned long>(error));
        return std::nullopt;
    }
    DWORD console_mode = 0;
    const DWORD kind = ::GetFileType(duplicate);
    const BOOL console = ::GetConsoleMode(duplicate, &console_mode);
    if (kind == FILE_TYPE_CHAR && console) {
        // A positively identified console handle is not a printer port. NT
        // object-name queries can reject ConDrv's console pseudo-handles.
        (void)::CloseHandle(duplicate);
        return std::wstring{};
    }
    using Query = NTSTATUS (NTAPI*)(HANDLE, OBJECT_INFORMATION_CLASS, void*, ULONG, ULONG*);
    const auto module = ::GetModuleHandleW(L"ntdll.dll");
    const FARPROC address = module ? ::GetProcAddress(module, "NtQueryObject") : nullptr;
    Query query = nullptr;
    static_assert(sizeof(query) == sizeof(address));
    std::memcpy(&query, &address, sizeof(query));
    alignas(UNICODE_STRING) std::array<std::byte, 65536> buffer{};
    ULONG length = 0;
    // ObjectNameInformation is class 1 in the Microsoft diagnostic pattern.
    const auto status = query ? query(duplicate, static_cast<OBJECT_INFORMATION_CLASS>(1),
        buffer.data(), static_cast<ULONG>(buffer.size()), &length) : static_cast<NTSTATUS>(-1);
    if (status != 0 || length > buffer.size()) {
        DWORD pipe_flags = 0;
        if (kind == FILE_TYPE_PIPE && ::GetNamedPipeInfo(duplicate, &pipe_flags, nullptr, nullptr, nullptr)) {
            using QueryFile = NTSTATUS (NTAPI*)(HANDLE, IO_STATUS_BLOCK*, void*, ULONG,
                                                FILE_INFORMATION_CLASS);
            const FARPROC file_address = module ? ::GetProcAddress(module, "NtQueryInformationFile") : nullptr;
            QueryFile query_file = nullptr;
            static_assert(sizeof(query_file) == sizeof(file_address));
            std::memcpy(&query_file, &file_address, sizeof(query_file));
            IO_STATUS_BLOCK io{};
            // Microsoft's FILE_INFORMATION_CLASS FileNameInformation is 9;
            // its layout is the byte count followed by WCHAR filename data.
            const auto file_status = query_file ? query_file(duplicate, &io, buffer.data(),
                static_cast<ULONG>(buffer.size()), static_cast<FILE_INFORMATION_CLASS>(9)) :
                static_cast<NTSTATUS>(-1);
            const auto* file_name = reinterpret_cast<const FILE_NAME_INFO*>(buffer.data());
            if (file_status == 0 && io.Information >= offsetof(FILE_NAME_INFO, FileName) &&
                io.Information <= buffer.size() && file_name->FileNameLength % sizeof(wchar_t) == 0 &&
                file_name->FileNameLength <= io.Information - offsetof(FILE_NAME_INFO, FileName)) {
                (void)::CloseHandle(duplicate);
                if (file_name->FileNameLength == 0) return std::wstring{};
                return L"\\Device\\NamedPipe" + std::wstring(file_name->FileName,
                    file_name->FileNameLength / sizeof(wchar_t));
            }
        }
        std::fprintf(stderr, "Observer File name rejected handle=%p status=%08lx length=%lu\n",
            remote, static_cast<unsigned long>(status), static_cast<unsigned long>(length));
        (void)::CloseHandle(duplicate);
        return std::nullopt;
    }
    (void)::CloseHandle(duplicate);
    const auto* name = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
    if (name->Length == 0) return std::wstring{};
    const auto start = reinterpret_cast<std::uintptr_t>(buffer.data());
    const auto text = reinterpret_cast<std::uintptr_t>(name->Buffer);
    if (name->Length % sizeof(wchar_t) != 0 || text < start ||
        text - start > buffer.size() || name->Length > buffer.size() - (text - start))
        return std::nullopt;
    return std::wstring(name->Buffer, name->Length / sizeof(wchar_t));
}

inline std::optional<std::vector<std::wstring>> named_handles(DWORD pid) {
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                        PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) return std::nullopt;
    HPSS snapshot = nullptr;
    const auto flags = static_cast<PSS_CAPTURE_FLAGS>(PSS_CAPTURE_HANDLES |
                                                     PSS_CAPTURE_HANDLE_NAME_INFORMATION);
    const DWORD captured = ::PssCaptureSnapshot(process, flags, 0, &snapshot);
    if (captured != ERROR_SUCCESS) { (void)::CloseHandle(process); return std::nullopt; }
    HPSSWALK marker = nullptr;
    if (::PssWalkMarkerCreate(nullptr, &marker) != ERROR_SUCCESS) {
        (void)::PssFreeSnapshot(::GetCurrentProcess(), snapshot);
        (void)::CloseHandle(process);
        return std::nullopt;
    }
    std::vector<std::wstring> names;
    DWORD walked = ERROR_SUCCESS;
    for (;;) {
        PSS_HANDLE_ENTRY entry{};
        walked = ::PssWalkSnapshot(snapshot, PSS_WALK_HANDLES, marker, &entry, sizeof(entry));
        if (walked != ERROR_SUCCESS) break;
        std::wstring_view type;
        if (entry.TypeName && entry.TypeNameLength % sizeof(wchar_t) == 0) {
            type = {entry.TypeName, entry.TypeNameLength / sizeof(wchar_t)};
            // Native PSS may include the terminating NUL in its byte count.
            if (!type.empty() && type.back() == L'\0') type.remove_suffix(1);
        }
        if ((entry.Flags & PSS_HANDLE_HAVE_NAME) && entry.ObjectName) {
            // PSS lengths are bytes, and these buffers need not be NUL terminated.
            if (entry.ObjectNameLength % sizeof(wchar_t) != 0) {
                walked = ERROR_INVALID_DATA;
                break;
            }
            names.emplace_back(entry.ObjectName, entry.ObjectNameLength / sizeof(wchar_t));
            // PSS can include a terminating NUL for named user objects too,
            // including WindowStation. Preserve the name, not its terminator.
            if (!names.back().empty() && names.back().back() == L'\0') names.back().pop_back();
        } else if (type == L"File") {
            const auto name = file_object_name(process, entry.Handle);
            if (!name) { walked = ERROR_INVALID_DATA; break; }
            if (!name->empty()) names.push_back(*name);
        }
    }
    const DWORD marker_freed = ::PssWalkMarkerFree(marker);
    const DWORD snapshot_freed = ::PssFreeSnapshot(::GetCurrentProcess(), snapshot);
    (void)::CloseHandle(process);
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
