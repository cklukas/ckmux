// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// What `~` means, asserted in the one place that decides it.
//
// This suite exists because the answer used to be decided at a call site. The
// printer's save path defaulted to `~/Documents`, nothing expanded it, and the
// first working save landed in `<cwd>/~/Documents/` — a directory literally
// named `~`, holding a byte-perfect document the reader would never have
// found. It was repaired inline, in `run_client.cpp`, which is the one file in
// ckmux with no test coverage at all: fixed, and unguarded, so a refactor that
// dropped the five lines would have failed nothing.
//
// The cases below are the header's stated rules, one test each. Two of them
// are the residual holes the inline version still had.
#include "platform/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "cvision/testing/cktest.hpp"

using ckm::platform::expand_user_path;

namespace {

// `setenv`/`unsetenv` around one case, restored on the way out so suites
// cannot leak an environment into each other.
class ScopedHome {
public:
    explicit ScopedHome(const char* value) {
        if (const char* const previous = std::getenv("HOME")) {
            had_ = true;
            previous_ = previous;
        }
        if (value == nullptr)
            ::unsetenv("HOME");
        else
            ::setenv("HOME", value, 1);
    }
    ~ScopedHome() {
        if (had_)
            ::setenv("HOME", previous_.c_str(), 1);
        else
            ::unsetenv("HOME");
    }
    ScopedHome(const ScopedHome&) = delete;
    ScopedHome& operator=(const ScopedHome&) = delete;

private:
    bool had_ = false;
    std::string previous_;
};

}  // namespace

CK_TEST(a_path_without_a_tilde_is_handed_back_exactly_as_written) {
    ScopedHome home("/home/reader");
    CK_CHECK(expand_user_path("/tmp/out.txt") == std::filesystem::path("/tmp/out.txt"));
    CK_CHECK(expand_user_path("relative/out.txt") == std::filesystem::path("relative/out.txt"));
    CK_CHECK(expand_user_path("") == std::filesystem::path(""));
    // A tilde that is not leading is an ordinary character in a filename.
    CK_CHECK(expand_user_path("/tmp/back~up") == std::filesystem::path("/tmp/back~up"));
}

CK_TEST(a_bare_tilde_is_the_home_directory_itself) {
    // Worth its own case because the inline version's string test was
    // `rfind("~/", 0) == 0`, which a bare `~` does not satisfy — it only
    // worked because of a separate equality check beside it. Nothing stated
    // that, so it was an accident rather than a decision.
    ScopedHome home("/home/reader");
    CK_CHECK(expand_user_path("~") == std::filesystem::path("/home/reader"));
}

CK_TEST(a_tilde_with_a_path_under_it_lands_under_the_home_directory) {
    ScopedHome home("/home/reader");
    CK_CHECK(expand_user_path("~/Documents") == std::filesystem::path("/home/reader/Documents"));
    CK_CHECK(expand_user_path("~/Documents/ckmux-print-0-2.txt") ==
             std::filesystem::path("/home/reader/Documents/ckmux-print-0-2.txt"));
    // The joining trap: `home / "/Documents"` discards `home` and answers
    // `/Documents`, so the separator must be consumed rather than kept.
    CK_CHECK(expand_user_path("~/Documents").string().rfind("/home/reader/", 0) == 0);
}

CK_TEST(another_users_home_is_left_alone_rather_than_guessed_at) {
    // `~other` is a different and more dangerous feature — resolving another
    // account's home out of a configuration string — and refusing it is a
    // decision. Asserted so that a later "improvement" has to argue with a
    // test rather than with nobody.
    ScopedHome home("/home/reader");
    CK_CHECK(expand_user_path("~root/secrets") == std::filesystem::path("~root/secrets"));
    CK_CHECK(expand_user_path("~other") == std::filesystem::path("~other"));
}

CK_TEST(a_relative_home_is_refused_rather_than_used) {
    // The first residual hole. `HOME=relative` and a naive `getenv` produces
    // `relative/Documents`, resolved against wherever the client happened to
    // be started — the same unfindable-document defect one notch milder, and
    // the case `environment_directory` exists to reject.
    ScopedHome home("not/absolute");
    const std::filesystem::path resolved = expand_user_path("~/Documents");
    CK_CHECK(resolved.is_absolute());
    CK_CHECK(resolved.string().rfind("not/absolute", 0) != 0);
}

CK_TEST(an_absent_home_still_answers_with_an_absolute_path_and_never_a_literal_tilde) {
    // The second residual hole, and the worse of the two: the inline guard
    // fell back to leaving `~` in the string, so a daemon-like environment
    // with no HOME reproduced the original `<cwd>/~/Documents/` bug exactly.
    // A guard whose failure mode is the defect it guards against is not one.
    // `config_file_path()` already contemplates "no HOME at all", so this
    // environment is documented in the same header.
    ScopedHome home(nullptr);
    const std::filesystem::path resolved = expand_user_path("~/Documents");
    CK_CHECK(resolved.is_absolute());
    CK_CHECK(resolved.string().find('~') == std::string::npos);
}
