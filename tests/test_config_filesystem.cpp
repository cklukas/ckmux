// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/config.hpp"
#include "scratch_directory.hpp"
#include <functional>
#include "cvision/testing/cktest.hpp"
using ckm::save_setting;
namespace {
class ControlledSettingsFileSystem final : public ckv::FileSystem {
public:
    ckv::MemoryFileSystem files;
    std::function<void()> before_write;
    bool readable = true;
    bool create_allowed = true;
    int writes = 0;
    std::vector<ckv::FileEntry> list_directory(std::string_view path) const override { return files.list_directory(path); }
    bool exists(std::string_view path) const noexcept override { return files.exists(path); }
    bool is_directory(std::string_view path) const noexcept override { return files.is_directory(path); }
    bool create_directories(std::string_view path) override { return create_allowed && files.create_directories(path); }
    std::optional<ckv::FileReadResult> read_file(std::string_view path) const override {
        return readable ? files.read_file(path) : std::nullopt;
    }
    ckv::FileWriteResult write_file_atomic(std::string_view path, std::string_view contents,
                                          ckv::FileWriteExpectation expectation) override {
        ++writes;
        if (before_write) before_write();
        return files.write_file_atomic(path, contents, std::move(expectation));
    }
};
} // namespace
CK_TEST(injected_settings_save_preserves_comments_and_unrelated_keys) {
    ControlledSettingsFileSystem fs;
    fs.files.add_file("/config/ckmux.conf", "# mine\n[general]\ntheme = old # keep\nclock = true\n");
    CK_CHECK(save_setting(fs, "/config/ckmux.conf", "general", "theme", "light"));
    const auto saved = fs.files.read_file("/config/ckmux.conf");
    CK_CHECK(saved && saved->contents == "# mine\n[general]\ntheme = light  # keep\nclock = true\n");
    CK_CHECK(fs.writes == 1);
}

CK_TEST(injected_settings_save_refuses_an_edit_arriving_after_the_read) {
    ControlledSettingsFileSystem fs;
    fs.files.add_file("/config/ckmux.conf", "[general]\ntheme = old\n");
    fs.before_write = [&] { fs.files.add_file("/config/ckmux.conf", "# external\n[general]\ntheme = external\n"); };
    CK_CHECK(!save_setting(fs, "/config/ckmux.conf", "general", "theme", "light"));
    const auto remaining = fs.files.read_file("/config/ckmux.conf");
    CK_CHECK(remaining && remaining->contents == "# external\n[general]\ntheme = external\n");
    CK_CHECK(fs.writes == 1);
}

CK_TEST(injected_settings_save_creates_missing_parents_but_refuses_a_concurrent_creator) {
    ControlledSettingsFileSystem fs;
    CK_CHECK(save_setting(fs, "/config/ckmux.conf", "general", "theme", "dark"));
    const auto created = fs.files.read_file("/config/ckmux.conf");
    CK_CHECK(created && created->contents == "[general]\ntheme = dark\n");
    fs.before_write = [&] { fs.files.add_file("/new/config/ckmux.conf", "# another creator\n"); };
    CK_CHECK(!save_setting(fs, "/new/config/ckmux.conf", "general", "theme", "light"));
    const auto remaining = fs.files.read_file("/new/config/ckmux.conf");
    CK_CHECK(remaining && remaining->contents == "# another creator\n");
}

CK_TEST(injected_settings_save_does_not_replace_an_unreadable_file_or_ignore_directory_failure) {
    ControlledSettingsFileSystem fs;
    fs.files.add_file("/config/ckmux.conf", "# unreadable\n");
    fs.readable = false;
    CK_CHECK(!save_setting(fs, "/config/ckmux.conf", "general", "theme", "light"));
    CK_CHECK(fs.writes == 0);
    fs.readable = true;
    fs.create_allowed = false;
    CK_CHECK(!save_setting(fs, "/config/ckmux.conf", "general", "theme", "light"));
    CK_CHECK(fs.writes == 0);
    const auto remaining = fs.files.read_file("/config/ckmux.conf");
    CK_CHECK(remaining && remaining->contents == "# unreadable\n");
}

CK_TEST(native_settings_save_and_load_support_unicode_filenames) {
    ckmtest::ScratchDirectory scratch("config-native");
    const auto file = scratch.path() / std::filesystem::path(u8"\u4e2d-\U0001f600.conf");
    CK_CHECK(save_setting(file, "general", "theme", "light"));
    const auto loaded = ckm::load_settings(file);
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.theme == ckm::Theme::Light);
    CK_CHECK(save_setting(file, "general", "theme", "dark"));
    CK_CHECK(ckm::load_settings(file).settings.theme == ckm::Theme::Dark);
    CK_CHECK(save_setting(file, "general", "unknown-key", "value"));
    const auto invalid = ckm::load_settings(file);
    const auto bytes = file.u8string();
    const std::string filename(bytes.begin(), bytes.end());
    CK_CHECK(invalid.warnings.size() == 1);
    CK_CHECK(!invalid.warnings.empty() && invalid.warnings.front().starts_with(filename + ":"));
}
