// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ckmtest {

inline std::filesystem::path scratch_root() { return CKMUX_TEST_TEMP_ROOT; }

class ScratchDirectory final {
public:
    explicit ScratchDirectory(std::string_view prefix) : root_(scratch_root()) {
        if (prefix.empty() || prefix.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                std::string_view::npos)
            throw std::invalid_argument("invalid scratch-directory prefix");
        std::error_code error;
        if (!root_.is_absolute() || !std::filesystem::is_directory(root_, error) || error)
            throw std::runtime_error("the selected test scratch root is unavailable");
        std::random_device entropy;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = root_ / (std::string(prefix) + "-" +
                std::to_string(entropy()) + "-" + std::to_string(entropy()));
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error) throw std::runtime_error("cannot create the selected test scratch directory");
        }
        throw std::runtime_error("cannot choose a unique test scratch directory");
    }
    ~ScratchDirectory() {
        if (path_.empty() || path_.parent_path() != root_) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

// POSIX shell literal, including roots with spaces, apostrophes or metacharacters.
inline std::string shell_quote(std::string_view text) {
    std::string result = "'";
    for (const char ch : text) {
        if (ch == '\'') result += "'\\''";
        else result += ch;
    }
    result += '\'';
    return result;
}

}  // namespace ckmtest
