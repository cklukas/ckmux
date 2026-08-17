// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The configuration file (plans/08-configuration.md). Two of that plan's
// promises are what these tests are really about: an empty or absent config
// is the normal case and never an error, and the file belongs to the reader —
// a dialog writing one key back leaves their comments, their ordering and
// every other key exactly where they were.
#include "common/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cvision/testing/cktest.hpp"
#include "platform/paths.hpp"

using ckm::bool_setting;
using ckm::load_settings;
using ckm::save_setting;
using ckm::Settings;

namespace {

// Its own directory per test, removed when the test leaves, so nothing here
// depends on — or disturbs — the machine's real configuration.
class ScratchConfig {
public:
    explicit ScratchConfig(const std::string& name)
        : directory_(std::filesystem::temp_directory_path() / ("ckmux-config-" + name)) {
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
    }
    ~ScratchConfig() { std::filesystem::remove_all(directory_); }
    ScratchConfig(const ScratchConfig&) = delete;
    ScratchConfig& operator=(const ScratchConfig&) = delete;

    std::filesystem::path path() const { return directory_ / "ckmux.conf"; }

    void write(const std::string& contents) const {
        std::ofstream out(path(), std::ios::trunc);
        out << contents;
    }

    std::string read() const {
        std::ifstream in(path());
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

private:
    std::filesystem::path directory_;
};

// Restores whichever of the location variables a test changes.
class ScopedEnvironment {
public:
    explicit ScopedEnvironment(std::string name) : name_(std::move(name)) {
        const char* const current = std::getenv(name_.c_str());
        had_value_ = current != nullptr;
        if (had_value_) previous_ = current;
    }
    ~ScopedEnvironment() {
        if (had_value_) (void)::setenv(name_.c_str(), previous_.c_str(), 1);
        else (void)::unsetenv(name_.c_str());
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    void set(const std::string& value) const { (void)::setenv(name_.c_str(), value.c_str(), 1); }
    void clear() const { (void)::unsetenv(name_.c_str()); }

private:
    std::string name_;
    bool had_value_ = false;
    std::string previous_;
};

}  // namespace

// --- Reading ----------------------------------------------------------

CK_TEST(a_config_file_that_does_not_exist_is_every_default_and_no_complaint) {
    // The plan's first promise: ckmux is excellent with an empty config.
    // Warning about a file the reader never created would be noise.
    ScratchConfig scratch("absent");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings == Settings{});
    CK_CHECK(loaded.settings.login_shell);
    CK_CHECK(loaded.warnings.empty());
}

CK_TEST(login_shell_false_is_read_from_the_general_section) {
    ScratchConfig scratch("false");
    scratch.write("[general]\nlogin-shell = false\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(!loaded.settings.login_shell);
    CK_CHECK(loaded.warnings.empty());
}

CK_TEST(comments_blank_lines_and_loose_spacing_are_all_read_through) {
    ScratchConfig scratch("shapes");
    scratch.write(
        "# ckmux configuration\n"
        "\n"
        "[general]\n"
        "   login-shell   =   false      # I start ckmux from a login shell already\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(!loaded.settings.login_shell);
    CK_CHECK(loaded.warnings.empty());
}

CK_TEST(the_same_key_in_another_section_is_not_this_key) {
    // Sections are what make a one-word key like login-shell safe to have.
    ScratchConfig scratch("sections");
    scratch.write("[terminal]\nlogin-shell = false\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings.login_shell);  // still the default
    CK_CHECK(loaded.warnings.size() == 1U);
}

CK_TEST(an_unknown_key_warns_with_its_line_and_leaves_everything_else_read) {
    // The plan's second promise: a bad key never aborts, and the rest of the
    // file is still read. A typo is the case that matters — a key the plan
    // documents is read even before the milestone that acts on it, so
    // "unknown" now means "no such key", not "not yet".
    ScratchConfig scratch("unknown");
    scratch.write(
        "[general]\n"
        "shel = /bin/sh\n"
        "login-shell = false\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(!loaded.settings.login_shell);
    CK_CHECK(loaded.warnings.size() == 1U);
    if (loaded.warnings.size() == 1U) {
        CK_CHECK(loaded.warnings[0].find("ckmux.conf:2") != std::string::npos);
        CK_CHECK(loaded.warnings[0].find("shel") != std::string::npos);
    }
}

CK_TEST(a_value_that_is_not_true_or_false_warns_and_keeps_the_default) {
    // Refusing to start over one mistyped word would be worse than the
    // mistype, and silently guessing what they meant would be worse still.
    ScratchConfig scratch("bogus");
    scratch.write("[general]\nlogin-shell = yes\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings.login_shell);
    CK_CHECK(loaded.warnings.size() == 1U);
    CK_CHECK(loaded.warnings[0].find("true or false") != std::string::npos);
}

CK_TEST(a_line_that_is_neither_a_section_nor_an_assignment_warns_by_line_number) {
    ScratchConfig scratch("malformed");
    scratch.write("[general]\nthis is not a setting\nlogin-shell = false\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(!loaded.settings.login_shell);
    CK_CHECK(loaded.warnings.size() == 1U);
    CK_CHECK(loaded.warnings[0].find("ckmux.conf:2") != std::string::npos);
}

CK_TEST(the_picture_limit_is_read_from_the_terminal_section) {
    ScratchConfig scratch("sixel");
    scratch.write("[terminal]\nsixel-max-megapixels = 128\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings.sixel_max_megapixels == 128);
    CK_CHECK(loaded.warnings.empty());
}

CK_TEST(a_picture_limit_that_is_not_a_number_warns_and_keeps_the_default) {
    ScratchConfig scratch("sixel-bogus");
    scratch.write("[terminal]\nsixel-max-megapixels = plenty\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings.sixel_max_megapixels == Settings{}.sixel_max_megapixels);
    CK_CHECK(loaded.warnings.size() == 1U);
    CK_CHECK(loaded.warnings[0].find("sixel-max-megapixels") != std::string::npos);
}

CK_TEST(a_picture_limit_outside_the_accepted_range_is_refused_rather_than_clamped) {
    // Clamping would store a number the reader never chose and give them no
    // sign that the one they did choose meant nothing.
    for (const char* const value : {"0", "-8", "100000", "64.0", "64 megapixels", "0x40"}) {
        ScratchConfig scratch("sixel-range");
        scratch.write(std::string("[terminal]\nsixel-max-megapixels = ") + value + "\n");
        const ckm::LoadedSettings loaded = load_settings(scratch.path());
        CK_CHECK(loaded.settings.sixel_max_megapixels == Settings{}.sixel_max_megapixels);
        CK_CHECK(loaded.warnings.size() == 1U);
    }
    CK_CHECK(ckm::parse_sixel_megapixels("1").value_or(0) == ckm::kSixelMegapixelsMin);
    CK_CHECK(ckm::parse_sixel_megapixels("  64  ").value_or(0) == 64);
    CK_CHECK(!ckm::parse_sixel_megapixels("").has_value());
}

// --- Writing ----------------------------------------------------------

CK_TEST(saving_into_an_absent_file_creates_it_with_its_section) {
    ScratchConfig scratch("create");
    std::filesystem::remove(scratch.path());
    CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(false)));
    CK_CHECK(scratch.read() == "[general]\nlogin-shell = false\n");
    CK_CHECK(!load_settings(scratch.path()).settings.login_shell);
}

CK_TEST(saving_creates_the_directory_the_file_belongs_in) {
    // First run on a machine that has no ~/.config/ckmux yet — which is
    // every machine, once.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "ckmux-config-mkdir";
    std::filesystem::remove_all(root);
    const std::filesystem::path nested = root / "ckmux" / "ckmux.conf";
    CK_CHECK(save_setting(nested, "general", "login-shell", bool_setting(true)));
    CK_CHECK(std::filesystem::exists(nested));
    std::filesystem::remove_all(root);
}

CK_TEST(saving_rewrites_the_key_in_place_and_leaves_the_rest_of_the_file_alone) {
    // The heart of "key-targeted": this file is the reader's, and a dialog
    // is a visitor in it.
    ScratchConfig scratch("in-place");
    scratch.write(
        "# my ckmux\n"
        "\n"
        "[general]\n"
        "prefix = C-a\n"
        "login-shell = true\n"
        "\n"
        "[terminal]\n"
        "mouse = true\n");
    CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(false)));
    CK_CHECK(scratch.read() ==
             "# my ckmux\n"
             "\n"
             "[general]\n"
             "prefix = C-a\n"
             "login-shell = false\n"
             "\n"
             "[terminal]\n"
             "mouse = true\n");
}

CK_TEST(a_note_the_reader_wrote_beside_the_setting_survives_the_rewrite) {
    ScratchConfig scratch("comment");
    scratch.write("[general]\nlogin-shell = true   # ckmux is not my login shell\n");
    CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(false)));
    CK_CHECK(scratch.read() == "[general]\nlogin-shell = false  # ckmux is not my login shell\n");
}

CK_TEST(saving_a_key_its_section_does_not_have_yet_adds_it_to_that_section) {
    ScratchConfig scratch("add-key");
    scratch.write("[general]\nprefix = C-a\n\n[terminal]\nmouse = true\n");
    CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(false)));
    // Added inside [general], not at the end of the file where it would
    // silently land in [terminal] and mean nothing.
    CK_CHECK(scratch.read() ==
             "[general]\nprefix = C-a\nlogin-shell = false\n\n[terminal]\nmouse = true\n");
    CK_CHECK(!load_settings(scratch.path()).settings.login_shell);
}

CK_TEST(saving_into_a_file_with_no_such_section_appends_one) {
    ScratchConfig scratch("add-section");
    scratch.write("[terminal]\nmouse = true\n");
    CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(false)));
    CK_CHECK(scratch.read() == "[terminal]\nmouse = true\n\n[general]\nlogin-shell = false\n");
}

CK_TEST(what_was_saved_is_what_is_read_back) {
    // The round trip is the promise the dialog actually makes.
    ScratchConfig scratch("round-trip");
    for (const bool value : {false, true, false}) {
        CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(value)));
        const ckm::LoadedSettings loaded = load_settings(scratch.path());
        CK_CHECK(loaded.settings.login_shell == value);
        CK_CHECK(loaded.warnings.empty());
    }
}

CK_TEST(the_picture_limit_round_trips_through_the_file_the_dialog_writes) {
    // What Settings does, in the order it does it: save the number, then find
    // it again on the next start.
    ScratchConfig scratch("sixel-round-trip");
    scratch.write("[general]\nlogin-shell = true\n");
    CK_CHECK(save_setting(scratch.path(), "terminal", "sixel-max-megapixels", std::to_string(256)));
    CK_CHECK(scratch.read() ==
             "[general]\nlogin-shell = true\n\n[terminal]\nsixel-max-megapixels = 256\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings.sixel_max_megapixels == 256);
    CK_CHECK(loaded.settings.login_shell);
    CK_CHECK(loaded.warnings.empty());
}

CK_TEST(saving_leaves_no_temporary_file_behind) {
    ScratchConfig scratch("atomic");
    CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(true)));
    CK_CHECK(!std::filesystem::exists(scratch.path().string() + ".tmp"));
}

// --- Where the file is ------------------------------------------------

CK_TEST(an_explicit_config_path_wins_over_every_convention) {
    ScopedEnvironment config("CKMUX_CONFIG");
    ScopedEnvironment xdg("XDG_CONFIG_HOME");
    config.set("/somewhere/ckmux-test.conf");
    xdg.set("/elsewhere");
    CK_CHECK(ckm::platform::config_file_path() == std::filesystem::path("/somewhere/ckmux-test.conf"));
}

CK_TEST(the_xdg_config_home_is_honoured_when_it_is_set) {
    ScopedEnvironment config("CKMUX_CONFIG");
    ScopedEnvironment xdg("XDG_CONFIG_HOME");
    config.clear();
    xdg.set("/elsewhere");
    CK_CHECK(ckm::platform::config_file_path() ==
             std::filesystem::path("/elsewhere/ckmux/ckmux.conf"));
}

CK_TEST(otherwise_the_file_is_under_dot_config_in_the_readers_home) {
    ScopedEnvironment config("CKMUX_CONFIG");
    ScopedEnvironment xdg("XDG_CONFIG_HOME");
    ScopedEnvironment home("HOME");
    config.clear();
    xdg.clear();
    home.set("/home/reader");
    CK_CHECK(ckm::platform::config_file_path() ==
             std::filesystem::path("/home/reader/.config/ckmux/ckmux.conf"));
}

CK_TEST(an_empty_environment_variable_is_not_a_location) {
    // Set-but-empty is how a shell says nothing, and reading it as a value
    // would send ckmux looking for "/ckmux/ckmux.conf".
    ScopedEnvironment config("CKMUX_CONFIG");
    ScopedEnvironment xdg("XDG_CONFIG_HOME");
    ScopedEnvironment home("HOME");
    config.set("");
    xdg.set("");
    home.set("/home/reader");
    CK_CHECK(ckm::platform::config_file_path() ==
             std::filesystem::path("/home/reader/.config/ckmux/ckmux.conf"));
}

CK_TEST(with_nowhere_to_look_there_is_no_config_file_rather_than_a_relative_one) {
    ScopedEnvironment config("CKMUX_CONFIG");
    ScopedEnvironment xdg("XDG_CONFIG_HOME");
    ScopedEnvironment home("HOME");
    config.clear();
    xdg.clear();
    home.clear();
    CK_CHECK(ckm::platform::config_file_path().empty());
    // ...and an empty path reads as "unconfigured" rather than crashing.
    CK_CHECK(load_settings(std::filesystem::path{}).settings == Settings{});
    CK_CHECK(!save_setting(std::filesystem::path{}, "general", "login-shell", "true"));
}

// --- Config v1: the rest of the key table, and `bind` (WP-12) -------------

CK_TEST(every_key_the_plans_example_file_documents_is_read) {
    // plans/08-configuration.md's annotated example claims to show the
    // defaults. This is that file, with every value changed, so a key that
    // was documented and never wired up fails here rather than in a reader's
    // hands. It also pins that the shipped defaults ARE what the plan says:
    // each assertion below differs from Settings{}'s value.
    ScratchConfig scratch("v1-keys");
    scratch.write(R"(
[general]
prefix = C-a
shell = /bin/zsh
login-shell = false
scrollback = 500
theme = light
on-exit = hold
confirm-kill = false
kill-empty-session = false
clock = minutes

[terminal]
term = screen-256color
mouse = false
alternate-scroll = false
sixel = off
sixel-max-megapixels = 128
osc52 = false
clipboard = exec:pbcopy -x, osc52

[printer]
mode = capture
ask-cache = 64K
spool-limit = 2M
save-format = ansi
save-folder = /tmp/prints
save-ask-name = false

[render]
max-fps = 60
)");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    for (const std::string& warning : loaded.warnings) std::fprintf(stderr, "unexpected: %s\n", warning.c_str());
    CK_CHECK(loaded.warnings.empty());

    const Settings& s = loaded.settings;
    CK_CHECK(ckm::chord_spelling(s.prefix) == "C-a");
    CK_CHECK(s.shell == "/bin/zsh");
    CK_CHECK(!s.login_shell);
    CK_CHECK(s.scrollback == 500);
    CK_CHECK(s.theme == ckm::Theme::Light);
    CK_CHECK(s.on_exit == ckm::ExitPolicy::Hold);
    CK_CHECK(!s.confirm_kill);
    CK_CHECK(!s.kill_empty_session);
    CK_CHECK(s.clock == ckm::ClockMode::Minutes);
    CK_CHECK(s.term == "screen-256color");
    CK_CHECK(!s.mouse);
    CK_CHECK(!s.alternate_scroll);
    CK_CHECK(s.sixel == ckm::SixelMode::Off);
    CK_CHECK(s.sixel_max_megapixels == 128);
    CK_CHECK(!s.osc52);
    CK_CHECK(s.clipboard.size() == 2U);
    CK_CHECK(s.clipboard[0].kind == ckm::ClipboardTarget::Kind::Exec);
    CK_CHECK(s.clipboard[0].command == "pbcopy -x");
    CK_CHECK(s.clipboard[1].kind == ckm::ClipboardTarget::Kind::Osc52);
    CK_CHECK(s.printer_mode == ckm::PrinterMode::Capture);
    CK_CHECK(s.printer_ask_cache_bytes == 64U * 1024U);
    CK_CHECK(s.printer_spool_limit_bytes == 2U * 1024U * 1024U);
    CK_CHECK(s.printer_save_format == ckm::PrinterSaveFormat::Ansi);
    CK_CHECK(s.printer_save_folder == "/tmp/prints");
    CK_CHECK(!s.printer_save_ask_name);
    CK_CHECK(s.max_fps == 60);
}

CK_TEST(the_defaults_are_the_ones_the_plan_prints) {
    // The empty config, key for key against plans/08's annotated example.
    const Settings s;
    CK_CHECK(ckm::chord_spelling(s.prefix) == "C-b");
    CK_CHECK(s.shell.empty());  // $SHELL, resolved at launch
    CK_CHECK(s.login_shell);
    CK_CHECK(s.scrollback == 10'000);
    CK_CHECK(s.theme == ckm::Theme::Dark);
    CK_CHECK(s.on_exit == ckm::ExitPolicy::HoldOnError);
    CK_CHECK(s.confirm_kill);
    CK_CHECK(s.kill_empty_session);
    CK_CHECK(s.clock == ckm::ClockMode::Seconds);
    CK_CHECK(s.term == "auto");
    CK_CHECK(s.mouse);
    CK_CHECK(s.alternate_scroll);
    CK_CHECK(s.sixel == ckm::SixelMode::Auto);
    CK_CHECK(s.sixel_max_megapixels == 64);
    CK_CHECK(s.osc52);
    CK_CHECK(s.printer_mode == ckm::PrinterMode::Ask);
    CK_CHECK(s.printer_ask_cache_bytes == 256U * 1024U);
    CK_CHECK(s.printer_spool_limit_bytes == 1024U * 1024U);
    CK_CHECK(s.printer_save_format == ckm::PrinterSaveFormat::Text);
    CK_CHECK(s.max_fps == 30);
    CK_CHECK(s.binds.empty());
}

CK_TEST(a_bad_value_names_its_line_says_what_it_wanted_and_keeps_the_default) {
    ScratchConfig scratch("bad-values");
    scratch.write("[general]\ntheme = drak\nscrollback = -1\n[render]\nmax-fps = 5000\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.size() == 3U);
    CK_CHECK(loaded.warnings[0].find("ckmux.conf:2") == 0);
    // All four parts: what was wanted, what was written, and what is in force.
    CK_CHECK(loaded.warnings[0].find("dark | light | mono") != std::string::npos);
    CK_CHECK(loaded.warnings[0].find("'drak'") != std::string::npos);
    CK_CHECK(loaded.warnings[0].find("keeping dark") != std::string::npos);
    CK_CHECK(loaded.warnings[1].find("ckmux.conf:3") == 0);
    CK_CHECK(loaded.warnings[2].find("ckmux.conf:5") == 0);
    // And nothing was half-applied.
    CK_CHECK(loaded.settings == Settings{});
}

CK_TEST(a_byte_size_is_read_with_its_suffix_and_bounded) {
    CK_CHECK(ckm::parse_byte_size("512") == 512U);
    CK_CHECK(ckm::parse_byte_size("256K") == 256U * 1024U);
    CK_CHECK(ckm::parse_byte_size("1M") == 1024U * 1024U);
    CK_CHECK(ckm::parse_byte_size("1G") == 1024U * 1024U * 1024U);
    CK_CHECK(!ckm::parse_byte_size("2G").has_value());   // past the ceiling
    CK_CHECK(!ckm::parse_byte_size("1m").has_value());   // one spelling only
    CK_CHECK(!ckm::parse_byte_size("K").has_value());
    CK_CHECK(!ckm::parse_byte_size("1 M").has_value());
    CK_CHECK(!ckm::parse_byte_size("").has_value());
}

CK_TEST(bind_and_unbind_are_read_in_file_order) {
    ScratchConfig scratch("binds");
    scratch.write(
        "[keys]\n"
        "bind terminal C new-terminal\n"
        "unbind terminal q\n"
        "bind terminal M-x menu-bar   # a trailing comment is still a comment\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.binds.size() == 3U);
    CK_CHECK(loaded.settings.binds[0].context == ckm::KeyContext::Terminal);
    CK_CHECK(loaded.settings.binds[0].chord == "C");
    CK_CHECK(loaded.settings.binds[0].action == ckm::Action::NewTerminal);
    CK_CHECK(!loaded.settings.binds[1].action.has_value());  // unbind
    CK_CHECK(loaded.settings.binds[1].chord == "q");
    // Stored canonically, so M-x and A-x cannot become two entries for one key.
    CK_CHECK(loaded.settings.binds[2].chord == "A-x");
}

CK_TEST(a_bad_bind_line_lists_what_could_have_been_written_instead) {
    ScratchConfig scratch("bad-binds");
    scratch.write(
        "[keys]\n"
        "bind termianl d detach\n"
        "bind terminal Nosuchkey detach\n"
        "bind terminal d no-such-action\n"
        "bind terminal d\n"
        "unbind terminal\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.size() == 5U);
    CK_CHECK(loaded.warnings[0].find("terminal, desktop") != std::string::npos);
    CK_CHECK(loaded.warnings[1].find("is not a chord") != std::string::npos);
    CK_CHECK(loaded.warnings[2].find("new-terminal") != std::string::npos);
    CK_CHECK(loaded.warnings[3].find("bind <context> <chord> <action>") != std::string::npos);
    CK_CHECK(loaded.warnings[4].find("unbind <context> <chord>") != std::string::npos);
    CK_CHECK(loaded.settings.binds.empty());
}

CK_TEST(a_key_that_is_read_but_not_yet_acted_on_says_so_rather_than_warning) {
    // The two failure modes this avoids: warning "unknown key" at a key the
    // plan documents, and accepting one silently so a reader believes it did
    // something. The list is the third answer.
    ScratchConfig scratch("not-yet");
    scratch.write("[render]\nmax-fps = 60\n");
    CK_CHECK(load_settings(scratch.path()).warnings.empty());
    const std::vector<std::string> pending = ckm::keys_not_honoured_yet();
    bool names_max_fps = false;
    for (const std::string& entry : pending)
        if (entry.find("max-fps") != std::string::npos) names_max_fps = true;
    CK_CHECK(names_max_fps);
    // Every entry names the package it waits for, so the list cannot rot into
    // "someday".
    for (const std::string& entry : pending)
        CK_CHECK(entry.find('(') != std::string::npos);
}
