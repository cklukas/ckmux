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

int job_root(bool keep_alive = false) {
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
    const DWORD descendant_pid = child.dwProcessId;
    ::CloseHandle(child.hThread);
    ::CloseHandle(child.hProcess);
    if (signaled != WAIT_OBJECT_0) return 15;
    if (std::puts("OWNED-DESCENDANT-READY") == EOF || std::fflush(stdout) != 0) return 16;
    if (std::printf("OWNED-DESCENDANT-PID %lu\r\n", static_cast<unsigned long>(descendant_pid)) < 0 ||
        std::fflush(stdout) != 0) return 18;
    if (keep_alive) ::Sleep(30000);
    return 0;
}

BOOL WINAPI ignore_control(DWORD) { return TRUE; }

int geometry() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE), &info)) return 19;
    if (std::printf("CHILD-GEOMETRY %d %d\r\n", static_cast<int>(info.dwSize.X),
        static_cast<int>(info.dwSize.Y)) < 0 || std::fflush(stdout) != 0) return 20;
    return 0;
}

int flood(const char* line) {
    const ULONGLONG end = ::GetTickCount64() + 30000;
    while (::GetTickCount64() < end) {
        if (std::puts(line) == EOF || std::fflush(stdout) != 0) return 21;
    }
    return 0;
}

int print_controller() {
    if (std::puts("PRINT-FIXTURE-READY") == EOF || std::fflush(stdout) != 0) return 22;
    for (;;) {
        const int byte = std::getchar();
        if (byte == EOF) return std::ferror(stdin) != 0 ? 23 : 0;
        if (byte < '1' || byte > '3') continue;
        if (std::printf("\x1b[5idocument-%c\r\n\x1b[4iPRINT-DONE-%c\r\n", byte, byte) < 0 ||
            std::fflush(stdout) != 0) return 24;
    }
}

int graphics(bool plain) {
    if (plain) {
        if (std::puts("VISIBLE-PAYLOAD #0;2;100;0;0 !36~") == EOF ||
            std::fflush(stdout) != 0) return 27;
        return 0;
    }
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!::GetConsoleMode(output, &mode) ||
        !::SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return 28;
    // Force a raster without probing: the application's no-graphics policy
    // must protect the reader even when a child ignores its advertisement.
    // Clear the earlier printable partner and the shell's command echo first.
    if (std::fputs("\x1b[2J\x1b[H\x1bPq\"1;1;36;18#0;2;100;0;0!36~-!36~-!36~\x1b\\"
                   "\x1b[4;1HNATIVE-GRAPHIC-DONE\r\n", stdout) == EOF ||
        std::fflush(stdout) != 0) return 29;
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    const bool command = argc == 3 && std::wstring(argv[1]) == L"-c";
    const std::wstring mode = command ? argv[2] : (argc == 2 ? argv[1] : L"");
    if (mode == L"--graphics-visible") return graphics(true);
    if (mode == L"--graphics-generator") return graphics(false);
    if (mode == L"--exit-seven") return 7;
    if (mode == L"--ran-exit-three") {
        if (std::puts("ran") == EOF || std::fflush(stdout) != 0) return 25;
        return 3;
    }
    if (mode == L"--read-exit") {
        for (;;) {
            const int byte = std::getchar();
            if (byte == '\n' || byte == '\r') return 0;
            if (byte == EOF) return 26;
        }
    }
    if (mode == L"--geometry-exit") return geometry();
    if (mode == L"--geometry") {
        if (const int code = geometry(); code != 0) return code;
        for (;;) {
            const int byte = std::getchar();
            if (byte == EOF) return 27;
            if (byte == 'g') { if (const int code = geometry(); code != 0) return code; }
        }
    }
    if (mode == L"--flood-first") return flood("first-child");
    if (mode == L"--flood-second") return flood("second-child");
    if (mode == L"--flood-third") return flood("third-child");
    if (mode == L"--print-controller") return print_controller();
    if (mode == L"--stubborn-root") {
        if (!::SetConsoleCtrlHandler(ignore_control, TRUE)) return 28;
        return job_root(true);
    }
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
