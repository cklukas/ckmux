// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/windows_pipe.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 2;
    const HANDLE go = ::OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
    const HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
    const HANDLE stop = ::OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
    if (!go || !ready || !stop) return 3;
    if (::WaitForSingleObject(go, 5000) != WAIT_OBJECT_0) return 4;
    const std::wstring wide_instance(argv[1]);
    std::string instance;
    for (const wchar_t c : wide_instance) {
        if (c > 127) return 7;
        instance.push_back(static_cast<char>(c));
    }
    ckm::platform::WindowsPipeListener listener;
    const auto status = listener.listen(instance);
    (void)::SetEvent(ready);
    const DWORD waited = ::WaitForSingleObject(stop, 5000);
    ::CloseHandle(go);
    ::CloseHandle(ready);
    ::CloseHandle(stop);
    if (waited != WAIT_OBJECT_0) return 5;
    return status == ckm::platform::WindowsPipeListener::Status::Listening ? 10
         : status == ckm::platform::WindowsPipeListener::Status::Racing ? 11 : 6;
}
