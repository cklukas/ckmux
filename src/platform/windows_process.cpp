// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/process.hpp"
#include "platform/paths.hpp"
#include "cvision/term/windows_argv.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <share.h>

namespace ckm::platform {

bool daemonize(const std::filesystem::path& log) {
    // start_server already creates a detached process with no inherited
    // handles/job. A manually invoked --server leaves its console here too.
    (void)::FreeConsole();
    std::error_code error;
    std::filesystem::create_directories(log.parent_path(), error);
    if (error) return false;
    // Secure freopen opens an exclusive writer: independently reopening both
    // stdout and stderr on the log closes stderr when the second open fails.
    // Open one shared append stream, then duplicate its descriptor instead.
    // A detached CRT initially reports -2 for standard streams, so attach all
    // three to NUL before asking dup2 for their actual descriptors.
    std::FILE* stream = nullptr;
    if (::_wfreopen_s(&stream, L"NUL", L"rbN", stdin) != 0) return false;
    if (::_wfreopen_s(&stream, L"NUL", L"wbN", stdout) != 0) return false;
    if (::_wfreopen_s(&stream, L"NUL", L"wbN", stderr) != 0) return false;
    std::FILE* output = ::_wfsopen(log.c_str(), L"abN", _SH_DENYNO);
    if (!output) return false;
    const int descriptor = ::_fileno(output);
    const bool redirected = ::_dup2(descriptor, ::_fileno(stdout)) == 0 &&
                            ::_dup2(descriptor, ::_fileno(stderr)) == 0;
    (void)std::fclose(output);
    if (!redirected) return false;
    for (const auto standard : {stdin, stdout, stderr}) {
        const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(standard)));
        if (handle == INVALID_HANDLE_VALUE ||
            !::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) return false;
    }
    (void)::SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(stdin))));
    (void)::SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(stdout))));
    (void)::SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(stderr))));
    return true;
}

bool start_server(const std::filesystem::path& executable, const std::filesystem::path& socket,
                  std::string& problem) {
    problem.clear();
    const auto& image = executable.native();
    const auto& endpoint = socket.native();
    const std::array<std::wstring_view, 3> arguments{image, L"--server", endpoint};
    auto command = ckv::term::windows_argv_command_line(arguments);
    if (!command || !executable.is_absolute()) {
        problem = "cannot encode an absolute native server executable and endpoint";
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    const DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP |
                        CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;
    // Never fall back to inheriting the caller's job: a successful start that
    // dies with the client would violate persistence rather than recover it.
    if (!::CreateProcessW(image.c_str(), command->data(), nullptr, nullptr, FALSE,
                         flags, nullptr, nullptr, &startup, &child)) {
        problem = "cannot start a detached server (Windows error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    ::CloseHandle(child.hThread);
    ::CloseHandle(child.hProcess);
    return true;
}

std::filesystem::path executable_path(const char* argv0) {
    std::wstring image(1024, L'\0');
    for (;;) {
        const DWORD count = ::GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
        if (count == 0) break;
        if (count < image.size()) { image.resize(count); return std::filesystem::path(image); }
        if (image.size() >= 32768) break;
        image.resize(std::min<std::size_t>(image.size() * 2, 32768));
    }
    return argv0 ? expand_user_path(argv0) : std::filesystem::path{};
}

std::filesystem::path server_log_path(const std::filesystem::path& socket) {
    // The pipe namespace is not a filesystem. Logs live beside configuration,
    // not under \\.\pipe, and each explicit instance has a separate filename.
    auto name = socket.filename().native();
    if (name.empty()) name = L"default";
    return config_file_path().parent_path() / (name + L".log");
}

} // namespace ckm::platform
