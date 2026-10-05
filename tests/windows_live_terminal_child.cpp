// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
int job_descendant(const wchar_t* event_name) {
    constexpr SIZE_T bytes = 4 * 1024 * 1024;
    auto* memory = static_cast<volatile unsigned char*>(::VirtualAlloc(nullptr, bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return 10;
    for (SIZE_T offset = 0; offset < bytes; offset += 4096) memory[offset] = 1;
    const HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);
    if (!ready || !::SetEvent(ready)) return 11;
    ::CloseHandle(ready);
    // A bounded detached child, still inside its inherited explicit job. The
    // terminal's close normally kills it; the deadline also bounds a failed rig.
    ::Sleep(30000);
    (void)::VirtualFree(const_cast<unsigned char*>(memory), 0, MEM_RELEASE);
    return 0;
}

int job_root() {
    wchar_t executable[32768]{};
    if (!::GetModuleFileNameW(nullptr, executable, 32768)) return 12;
    const std::wstring name = L"Local\\ckmux-resource-ready-" + std::to_wstring(::GetCurrentProcessId());
    const HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    if (!ready) return 13;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --job-descendant " + name;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!::CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                         DETACHED_PROCESS, nullptr, nullptr, &startup, &child)) {
        ::CloseHandle(ready);
        return 14;
    }
    const DWORD signaled = ::WaitForSingleObject(ready, 5000);
    ::CloseHandle(ready);
    ::CloseHandle(child.hThread);
    ::CloseHandle(child.hProcess);
    if (signaled != WAIT_OBJECT_0) return 15;
    if (std::puts("OWNED-DESCENDANT-READY") == EOF || std::fflush(stdout) != 0) return 16;
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    const bool command = argc == 3 && std::wstring(argv[1]) == L"-c";
    const std::wstring mode = command ? argv[2] : (argc == 2 ? argv[1] : L"");
    if (mode == L"--idle") {
        ::Sleep(3600000);
        return 0;
    }
    const bool echo = mode == L"--echo";
    if (echo && (std::puts("CKMUX-ECHO-READY") == EOF || std::fflush(stdout) != 0)) return 17;
    if ((argc == 2 && std::wstring(argv[1]) == L"--exit-zero") ||
        (argc == 3 && std::wstring(argv[1]) == L"-c" && std::wstring(argv[2]) == L"--exit-zero"))
        return 0;
    if (argc == 3 && std::wstring(argv[1]) == L"--job-descendant") return job_descendant(argv[2]);
    if ((argc == 2 && std::wstring(argv[1]) == L"--job-root") ||
        (argc == 3 && std::wstring(argv[1]) == L"-c" && std::wstring(argv[2]) == L"--job-root"))
        return job_root();
    for (;;) {
        const int byte = std::getchar();
        if (byte == EOF) return std::ferror(stdin) != 0 ? 1 : 0;
        // An ASCII transformation makes the response distinguishable from
        // the pseudoconsole's own input echo in the native fixture contract.
        const int response = !echo && byte >= 'a' && byte <= 'z' ? byte - 'a' + 'A' : byte;
        if (std::putchar(response) == EOF || std::fflush(stdout) != 0) return 2;
    }
}
