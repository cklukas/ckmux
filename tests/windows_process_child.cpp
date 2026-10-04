// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/process.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <io.h>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!ckm::platform::daemonize(std::filesystem::path(argv[1]))) return 1;
    if (::GetConsoleCP() != 0 || std::fgetc(stdin) != EOF) return 3;
    for (const auto standard : {stdin, stdout, stderr}) {
        const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(standard)));
        DWORD flags = 0;
        if (!::GetHandleInformation(handle, &flags) || (flags & HANDLE_FLAG_INHERIT) != 0) return 4;
    }
    if (std::fprintf(stdout, "stdout-marker\n") < 0 || std::fflush(stdout) != 0) return 5;
    if (std::fprintf(stderr, "stderr-marker\n") < 0 || std::fflush(stderr) != 0) return 6;
    return 0;
}
