// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ckmtest {

// Test-owned process environment, restored before the next case. Windows
// production paths read the Unicode OS environment, not the CRT's snapshot.
class ScopedEnvironment final {
public:
    explicit ScopedEnvironment(std::string name) : name_(std::move(name)) {
#if defined(_WIN32)
        native_name_.assign(name_.begin(), name_.end());
        ::SetLastError(ERROR_SUCCESS);
        const DWORD count = ::GetEnvironmentVariableW(native_name_.c_str(), nullptr, 0);
        const DWORD error = ::GetLastError();
        present_ = count != 0 || error == ERROR_SUCCESS;
        if (count == 0 && !present_ && error != ERROR_ENVVAR_NOT_FOUND)
            throw std::runtime_error("cannot read test environment");
        if (count != 0) {
            previous_.resize(count);
            const DWORD copied = ::GetEnvironmentVariableW(native_name_.c_str(), previous_.data(), count);
            if (copied >= count) throw std::runtime_error("test environment changed during capture");
            previous_.resize(copied);
        }
#else
        const char* value = std::getenv(name_.c_str());
        present_ = value != nullptr;
        if (present_) previous_ = value;
#endif
    }
    ~ScopedEnvironment() {
#if defined(_WIN32)
        (void)::SetEnvironmentVariableW(native_name_.c_str(), present_ ? previous_.c_str() : nullptr);
#else
        if (present_) (void)::setenv(name_.c_str(), previous_.c_str(), 1);
        else (void)::unsetenv(name_.c_str());
#endif
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    void set(const std::string& value) const {
#if defined(_WIN32)
        const std::u8string bytes(value.begin(), value.end());
        const auto native = std::filesystem::path(bytes).native();
        if (!::SetEnvironmentVariableW(native_name_.c_str(), native.c_str()))
            throw std::runtime_error("cannot set test environment");
#else
        if (::setenv(name_.c_str(), value.c_str(), 1) != 0)
            throw std::runtime_error("cannot set test environment");
#endif
    }
    void set_path(const std::filesystem::path& value) const {
        const auto bytes = value.u8string();
        set(std::string(bytes.begin(), bytes.end()));
    }
    void clear() const {
#if defined(_WIN32)
        if (!::SetEnvironmentVariableW(native_name_.c_str(), nullptr))
            throw std::runtime_error("cannot clear test environment");
#else
        if (::unsetenv(name_.c_str()) != 0)
            throw std::runtime_error("cannot clear test environment");
#endif
    }

private:
    std::string name_;
    bool present_ = false;
#if defined(_WIN32)
    std::wstring native_name_;
    std::wstring previous_;
#else
    std::string previous_;
#endif
};

} // namespace ckmtest
