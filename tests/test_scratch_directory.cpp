// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"

CK_TEST(scratch_directories_are_unique_children_of_the_selected_root) {
    const auto root = std::filesystem::path(CKMUX_TEST_TEMP_ROOT);
    std::filesystem::path first_path;
    {
        ckmtest::ScratchDirectory first("fixture");
        ckmtest::ScratchDirectory second("fixture");
        first_path = first.path();
        CK_CHECK(first.path().parent_path() == root);
        CK_CHECK(second.path().parent_path() == root);
        CK_CHECK(first.path() != second.path());
        CK_CHECK(std::filesystem::is_directory(first.path()));
        CK_CHECK(std::filesystem::is_directory(second.path()));
    }
    CK_CHECK(!std::filesystem::exists(first_path));
    CK_CHECK(std::filesystem::is_directory(root));
}

CK_TEST(scratch_directory_rejects_prefixes_that_escape_its_root) {
    for (const char* prefix : {"", "../parent", "a/b", "a\\b", "."}) {
        bool rejected = false;
        try { ckmtest::ScratchDirectory invalid(prefix); }
        catch (const std::invalid_argument&) { rejected = true; }
        CK_CHECK(rejected);
    }
    CK_CHECK(std::filesystem::is_directory(ckmtest::scratch_root()));
    CK_CHECK(ckmtest::shell_quote("a'b $c") == "'a'\\''b $c'");
}
