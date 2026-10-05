// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdio>
#include <string>
#include <vector>
#include "platform/windows_pipe.hpp"
#include "cvision/term/windows_argv.hpp"

namespace {
struct OwnedHandle {
    HANDLE value = nullptr;
    OwnedHandle() = default;
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() { close(); }
    void close() { if (value != nullptr) (void)::CloseHandle(value); value = nullptr; }
};
struct OwnedChild : OwnedHandle {
    ~OwnedChild() {
        if (value != nullptr && ::WaitForSingleObject(value, 0) == WAIT_TIMEOUT) {
            (void)::TerminateProcess(value, 91);
            (void)::WaitForSingleObject(value, 5000);
        }
    }
};
bool configure_job(HANDLE job, bool breakaway) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        (breakaway ? JOB_OBJECT_LIMIT_BREAKAWAY_OK : 0);
    return ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE;
}
bool invoke(const wchar_t* binary, const std::vector<std::wstring_view>& arguments,
            HANDLE outer, HANDLE inner, DWORD& exit) {
    auto command = ckv::term::windows_argv_command_line(arguments);
    if (!command) return false;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    // Remove uncontrolled ancestry before installing this test's real jobs.
    if (!::CreateProcessW(binary, command->data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
        nullptr, nullptr, &startup, &child)) return false;
    OwnedChild process;
    OwnedHandle thread;
    process.value = child.hProcess; thread.value = child.hThread;
    BOOL member = TRUE;
    if (!::IsProcessInJob(process.value, nullptr, &member) || member != FALSE ||
        (outer != nullptr && !::AssignProcessToJobObject(outer, process.value)) ||
        (inner != nullptr && !::AssignProcessToJobObject(inner, process.value)) ||
        ::ResumeThread(thread.value) == static_cast<DWORD>(-1) ||
        ::WaitForSingleObject(process.value, 15000) != WAIT_OBJECT_0) return false;
    return ::GetExitCodeProcess(process.value, &exit) != FALSE;
}
bool run_case(const wchar_t* binary, const wchar_t* label, bool outer_permissive) {
    if (!::SetEnvironmentVariableW(L"CKMUX_SOCKET", label)) return false;
    OwnedHandle outer, inner;
    outer.value = ::CreateJobObjectW(nullptr, nullptr);
    inner.value = ::CreateJobObjectW(nullptr, nullptr);
    if (outer.value == nullptr || inner.value == nullptr ||
        !configure_job(outer.value, outer_permissive) || !configure_job(inner.value, true)) return false;
    DWORD exit = 99;
    if (!invoke(binary, {binary, L"new", L"-s", L"ancestry-session"}, outer.value, inner.value, exit)) return false;
    std::printf("controlled ancestry: permissive=%d CLI_exit=%lu\n", outer_permissive ? 1 : 0, exit);
    std::string instance;
    for (const wchar_t letter : std::wstring_view(label)) {
        if (letter > 127) return false;
        instance.push_back(static_cast<char>(letter));
    }
    ckm::platform::WindowsPipeEndpoint endpoint;
    std::string problem;
    if (!ckm::platform::windows_pipe_endpoint(instance, endpoint, problem)) return false;
    auto connection = ckm::platform::WindowsPipeConnect::open(endpoint);
    if (!outer_permissive) {
        // Original production code returns 0 and exposes a doomed server here.
        const bool refused = exit != 0 && connection.status == ckm::platform::WindowsPipeConnect::Status::NoServer;
        inner.close(); outer.close();
        return refused;
    }
    if (exit != 0 || connection.status != ckm::platform::WindowsPipeConnect::Status::Connected) return false;
    ULONG identity = 0;
    if (!::GetNamedPipeServerProcessId(connection.connection.native_handle(), &identity) || identity == 0) return false;
    OwnedChild server;
    server.value = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, identity);
    BOOL member = TRUE;
    if (server.value == nullptr || !::IsProcessInJob(server.value, nullptr, &member) || member != FALSE ||
        ::WaitForSingleObject(server.value, 0) != WAIT_TIMEOUT) return false;
    connection.connection.close();
    // Environment change: close BOTH jobs that really held the creating client.
    inner.close(); outer.close();
    if (::WaitForSingleObject(server.value, 250) != WAIT_TIMEOUT) return false;
    if (!invoke(binary, {binary, L"ls"}, nullptr, nullptr, exit) || exit != 0) return false;
    // Kill only the instance whose named pipe supplied this exact server handle.
    if (!invoke(binary, {binary, L"kill-server"}, nullptr, nullptr, exit) || exit != 0 ||
        ::WaitForSingleObject(server.value, 5000) != WAIT_OBJECT_0) return false;
    std::printf("server pid=%lu survived ancestor closure and discovery; explicit kill completed\n", identity);
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::wstring prefix(argv[2]);
    const bool allowed = run_case(argv[1], (prefix + L"-allow").c_str(), true);
    const bool denied = run_case(argv[1], (prefix + L"-deny").c_str(), false);
    if (!allowed || !denied) {
        std::fprintf(stderr, "native server ancestry failed: allowed=%d denied=%d Windows_error=%lu\n",
                     allowed ? 1 : 0, denied ? 1 : 0, ::GetLastError());
        return 1;
    }
    return 0;
}
