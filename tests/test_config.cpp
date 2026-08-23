// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The configuration file (the configuration spec). Two of that plan's
// promises are what these tests are really about: an empty or absent config
// is the normal case and never an error, and the file belongs to the reader —
// a dialog writing one key back leaves their comments, their ordering and
// every other key exactly where they were.
#include "common/config.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "cvision/testing/cktest.hpp"
#include "platform/paths.hpp"

using ckm::bool_setting;
using ckm::load_settings;
using ckm::save_setting;
using ckm::Settings;

namespace {

// Its own directory per test, removed when the test leaves however it leaves,
// so nothing here depends on — or disturbs — the machine's real configuration.
//
// The name carries this process's id. A fixed path is shared state between
// every run of the suite on the machine: two at once (`ctest -j`, a sanitizer
// build beside a plain one) would each remove_all the other's directory
// mid-test, and the failure that produces looks like a config bug rather than
// like the collision it is.
class ScratchConfig {
public:
    explicit ScratchConfig(const std::string& name)
        : directory_(std::filesystem::temp_directory_path() /
                     ("ckmux-config-" + name + "-" +
                      std::to_string(static_cast<long long>(::getpid())))) {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
        std::filesystem::create_directories(directory_, ignored);
    }
    ~ScratchConfig() {
        // A test that took the permissions off the file to make it unreadable
        // leaves them off; the directory still has to go, on the failing path
        // as much as on the passing one.
        std::error_code ignored;
        std::filesystem::permissions(path(), std::filesystem::perms::owner_read,
                                     std::filesystem::perm_options::add, ignored);
        std::filesystem::remove_all(directory_, ignored);
    }
    ScratchConfig(const ScratchConfig&) = delete;
    ScratchConfig& operator=(const ScratchConfig&) = delete;

    const std::filesystem::path& directory() const { return directory_; }
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

// A warning names its file in full and then the line: "…/ckmux.conf:4: …".
// The whole path, because a reader with a `$CKMUX_CONFIG` beside their real
// file cannot tell two "ckmux.conf:4"s apart, and cannot paste a basename into
// an editor.
bool warns_at(const std::string& text, const std::filesystem::path& path, int line) {
    return text.rfind(path.string() + ":" + std::to_string(line) + ": ", 0) == 0;
}

// Takes the permissions off a file, and answers whether that actually made it
// unreadable. Running as root, or on a filesystem that does not carry
// permissions, it does not — and the two tests that need an unreadable file
// then skip rather than assert something untrue about the machine they are on.
bool make_unreadable(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::permissions(path, std::filesystem::perms::none, ignored);
    std::ifstream probe(path);
    return !probe;
}

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

CK_TEST(the_view_readouts_are_off_until_the_general_section_says_otherwise) {
    // WP-39's three keys: a readout is chrome a reader opts into, and each of
    // the three is its own key because each is its own checkbox — a reader
    // watching memory is not thereby watching CPU.
    ScratchConfig scratch("readouts");
    CK_CHECK(!Settings{}.show_cpu);
    CK_CHECK(!Settings{}.show_memory_rss);
    CK_CHECK(!Settings{}.show_memory_real);

    scratch.write("[general]\nshow-cpu = true\nshow-memory-real = true\n");
    const ckm::LoadedSettings asked = load_settings(scratch.path());
    CK_CHECK(asked.settings.show_cpu);
    CK_CHECK(!asked.settings.show_memory_rss);
    CK_CHECK(asked.settings.show_memory_real);
    CK_CHECK(asked.warnings.empty());

    // A value that is not a bool is refused with the words to fix it, like
    // every other boolean key.
    ScratchConfig noisy("readouts-noisy");
    noisy.write("[general]\nshow-memory-rss = sometimes\n");
    const ckm::LoadedSettings refused = load_settings(noisy.path());
    CK_CHECK(!refused.settings.show_memory_rss);
    CK_CHECK(refused.warnings.size() == 1U);
}

CK_TEST(resize_windows_to_fit_is_off_unless_the_general_section_asks_for_it) {
    // WP-30's one setting, and the whole reason it has a default worth pinning:
    // reattaching on a smaller terminal always MOVES a window to bring it into
    // the desktop, and shrinking one that still does not fit is a change to the
    // size its reader chose. So it happens only when they ask.
    ScratchConfig scratch("fit");
    CK_CHECK(!Settings{}.resize_windows_to_fit);

    scratch.write("[general]\nresize-windows-to-fit = true\n");
    const ckm::LoadedSettings asked = load_settings(scratch.path());
    CK_CHECK(asked.settings.resize_windows_to_fit);
    CK_CHECK(asked.warnings.empty());

    // And it lives in [general] — where the configuration spec documents it, where the struct
    // says it is, and where `Settings ▸ General…` writes it. Parsed anywhere
    // else it would be a second live copy of one setting with the dialog
    // writing to the other, which is exactly M-T1.
    ScratchConfig elsewhere("fit-elsewhere");
    elsewhere.write("[terminal]\nresize-windows-to-fit = true\n");
    const ckm::LoadedSettings misplaced = load_settings(elsewhere.path());
    CK_CHECK(!misplaced.settings.resize_windows_to_fit);
    CK_CHECK(misplaced.warnings.size() == 1U);
    if (misplaced.warnings.size() == 1U)
        CK_CHECK(misplaced.warnings[0].find("belongs in [general], not [terminal]") !=
                 std::string::npos);

    // A key that IS honoured, so `check-config` must not tell a reader who set
    // it that nothing will happen.
    for (const std::string& waiting : ckm::keys_not_honoured_yet())
        CK_CHECK(waiting.find("resize-windows-to-fit") == std::string::npos);
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
    // The line is not obeyed — and the warning says where the key does live,
    // because "unknown key 'login-shell'" would be a lie about a key the plan
    // documents and the reader has clearly read about.
    ScratchConfig scratch("sections");
    scratch.write("[terminal]\nlogin-shell = false\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings.login_shell);  // still the default
    CK_CHECK(loaded.warnings.size() == 1U);
    if (loaded.warnings.size() == 1U) {
        CK_CHECK(warns_at(loaded.warnings[0], scratch.path(), 2));
        CK_CHECK(loaded.warnings[0].find("belongs in [general], not [terminal]") != std::string::npos);
    }
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
        CK_CHECK(warns_at(loaded.warnings[0], scratch.path(), 2));
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
    if (loaded.warnings.size() == 1U) CK_CHECK(warns_at(loaded.warnings[0], scratch.path(), 2));
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
    // every machine, once. The scratch directory is the one that cleans up
    // after itself, including when the check below fails.
    ScratchConfig scratch("mkdir");
    const std::filesystem::path nested = scratch.directory() / "ckmux" / "ckmux.conf";
    CK_CHECK(save_setting(nested, "general", "login-shell", bool_setting(true)));
    CK_CHECK(std::filesystem::exists(nested));
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
    // Whatever the scratch file is called — the name carries a process id, so
    // asking for one spelling would pass by not looking.
    ScratchConfig scratch("atomic");
    CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(true)));
    std::size_t files = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(scratch.directory())) {
        ++files;
        CK_CHECK(entry.path() == scratch.path());
    }
    CK_CHECK(files == 1U);
}

CK_TEST(two_savers_at_once_do_not_share_one_scratch_file) {
    // The name of the file save_setting writes beside the target carries this
    // process's id. Two ckmux clients saving at the same moment would
    // otherwise write one scratch file between them, and the loser of that
    // race would rename half of the winner's bytes over the reader's
    // configuration. The id is what this asserts; the race is what it is for.
    ScratchConfig scratch("scratch-name");
    scratch.write("[general]\nlogin-shell = true\n");
    const std::string mine = std::to_string(static_cast<long long>(::getpid()));
    // A leftover from a *different* process must not be touched by this one:
    // it is another client's save in flight.
    const std::filesystem::path theirs = scratch.path().string() + ".tmp.999999";
    {
        std::ofstream out(theirs, std::ios::trunc);
        out << "not mine\n";
    }
    CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(false)));
    CK_CHECK(std::filesystem::exists(theirs));
    CK_CHECK(!std::filesystem::exists(scratch.path().string() + ".tmp." + mine));
    CK_CHECK(!load_settings(scratch.path()).settings.login_shell);
    std::error_code ignored;
    std::filesystem::remove(theirs, ignored);
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
    // The configuration spec's annotated example claims to show the
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
kill-grace-seconds = 30
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
    // Written `C-a` in the file above, shown and dispatched as `^A`: two
    // spellings a reader may write, one they are ever shown (the interface spec).
    CK_CHECK(ckm::chord_spelling(s.prefix) == "^A");
    CK_CHECK(s.shell == "/bin/zsh");
    CK_CHECK(!s.login_shell);
    CK_CHECK(s.scrollback == 500);
    CK_CHECK(s.theme == ckm::Theme::Light);
    CK_CHECK(s.on_exit == ckm::ExitPolicy::Hold);
    CK_CHECK(!s.confirm_kill);
    CK_CHECK(!s.kill_empty_session);
    CK_CHECK(s.kill_grace_seconds == 30);
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
    // The empty config, key for key against the configuration spec's annotated example.
    const Settings s;
    CK_CHECK(ckm::chord_spelling(s.prefix) == "^B");
    CK_CHECK(s.shell.empty());  // $SHELL, resolved at launch
    CK_CHECK(s.login_shell);
    CK_CHECK(s.scrollback == 10'000);
    CK_CHECK(s.theme == ckm::Theme::Dark);
    CK_CHECK(s.on_exit == ckm::ExitPolicy::HoldOnError);
    CK_CHECK(s.confirm_kill);
    CK_CHECK(s.kill_empty_session);
    CK_CHECK(s.kill_grace_seconds == 5);
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
    if (loaded.warnings.size() != 3U) return;
    CK_CHECK(warns_at(loaded.warnings[0], scratch.path(), 2));
    // All four parts: what was wanted, what was written, and what is in force.
    CK_CHECK(loaded.warnings[0].find("dark | light | mono") != std::string::npos);
    CK_CHECK(loaded.warnings[0].find("'drak'") != std::string::npos);
    CK_CHECK(loaded.warnings[0].find("keeping dark") != std::string::npos);
    CK_CHECK(warns_at(loaded.warnings[1], scratch.path(), 3));
    CK_CHECK(warns_at(loaded.warnings[2], scratch.path(), 5));
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
    // A key that really is unread today. This was `on-exit` until WP-13 wired
    // that one up — which is exactly the rot the test below guards, and the
    // reason this one must not assume any given key stays unread forever.
    scratch.write("[printer]\nask-cache = 4\n");
    CK_CHECK(load_settings(scratch.path()).warnings.empty());
    const std::vector<std::string> pending = ckm::keys_not_honoured_yet();
    const auto names = [&pending](const std::string& key) {
        for (const std::string& entry : pending)
            if (entry.find(key) != std::string::npos) return true;
        return false;
    };
    CK_CHECK(names("ask-cache"));
    // Every entry names the package it waits for, so the list cannot rot into
    // "someday".
    for (const std::string& entry : pending) CK_CHECK(entry.find('(') != std::string::npos);
}

CK_TEST(the_not_yet_honoured_list_does_not_claim_a_key_that_is_honoured) {
    // The list can lie in either direction, and it did: four keys stayed on it
    // after they were wired up, so `check-config` told a reader that setting
    // them would do nothing while they were quietly doing something (m-conf).
    // Each name below is here because something reads it today:
    //   kill-empty-session  server.cpp forget_session, both call sites
    //   [printer] mode      terminals.cpp launch profile (`off` denies)
    //   spool-limit         terminals.cpp max_printer_spool_bytes
    //   max-fps             server.cpp flush_tick period
    // A name reappearing here means the list rotted again, in the direction
    // that is harder to notice.
    const std::vector<std::string> pending = ckm::keys_not_honoured_yet();
    for (const std::string& entry : pending) {
        CK_CHECK(entry.find("kill-empty-session") == std::string::npos);
        CK_CHECK(entry.find("[printer] mode") == std::string::npos);
        CK_CHECK(entry.find("spool-limit") == std::string::npos);
        CK_CHECK(entry.find("max-fps") == std::string::npos);
        // Joined them with WP-13: server.cpp's self-exit path reads on-exit to
        // decide remove-or-hold and sets `TermClosed.hold`. It sat on this list
        // afterwards, so `ckmux check-config` — which prints the list — told
        // readers the key was dead while it was deciding what their windows did.
        CK_CHECK(entry.find("on-exit") == std::string::npos);
    }
    // And a key that is honoured is still read without complaint.
    ScratchConfig scratch("honoured");
    scratch.write("[render]\nmax-fps = 60\n[general]\nkill-empty-session = false\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.max_fps == 60);
    CK_CHECK(!loaded.settings.kill_empty_session);
}

// --- The prefix, in either spelling --------------------------------------

CK_TEST(the_prefix_can_be_written_with_a_caret_or_with_c_dash) {
    // The interface spec writes `^B`, the configuration spec's example file writes `C-b`, and a reader
    // may reasonably write either. Both parse, and both come back as the one
    // spelling every surface shows — so a rebinding cannot become two entries
    // for one key.
    for (const char* const written : {"^A", "C-a", "^a"}) {
        ScratchConfig scratch("prefix-spelling");
        scratch.write(std::string("[general]\nprefix = ") + written + "\n");
        const ckm::LoadedSettings loaded = load_settings(scratch.path());
        CK_CHECK(loaded.warnings.empty());
        CK_CHECK(ckm::chord_spelling(loaded.settings.prefix) == "^A");
    }
}

CK_TEST(a_prefix_that_is_not_a_chord_offers_the_spelling_it_wanted) {
    ScratchConfig scratch("prefix-bogus");
    scratch.write("[general]\nprefix = Ctrl+B\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.size() == 1U);
    if (loaded.warnings.size() == 1U) {
        // The four things, and the offered spelling is the one a reader will
        // then see in the footer rather than a second convention.
        CK_CHECK(loaded.warnings[0].find("^B") != std::string::npos);
        CK_CHECK(loaded.warnings[0].find("'Ctrl+B'") != std::string::npos);
        CK_CHECK(loaded.warnings[0].find("keeping ^B") != std::string::npos);
    }
    CK_CHECK(ckm::chord_spelling(loaded.settings.prefix) == "^B");
}

CK_TEST(a_ctrl_binding_is_stored_under_the_spelling_the_reader_is_shown) {
    // Both spellings in the file, one entry apiece under the canonical
    // spelling — the property that keeps dispatch and the footer agreeing.
    ScratchConfig scratch("bind-caret");
    scratch.write("[keys]\nbind terminal ^X menu-bar\nbind terminal C-y detach\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.binds.size() == 2U);
    if (loaded.settings.binds.size() == 2U) {
        CK_CHECK(loaded.settings.binds[0].chord == "^X");
        CK_CHECK(loaded.settings.binds[1].chord == "^Y");
    }
}

// --- kill-grace-seconds lives in [general] (M-T1) -------------------------

CK_TEST(the_grace_period_is_read_from_the_section_the_dialog_writes_it_to) {
    // The whole round trip the Settings dialog makes, in the order it makes
    // it. It used to be a circle that did not close: the dialog wrote
    // [general] kill-grace-seconds, the parser looked for it under [terminal]
    // only, and the next start warned the reader about a line ckmux had
    // written itself (M-T1).
    ScratchConfig scratch("grace");
    scratch.write("[general]\nlogin-shell = true\n");
    CK_CHECK(save_setting(scratch.path(), "general", "kill-grace-seconds", std::to_string(20)));
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.kill_grace_seconds == 20);
}

CK_TEST(the_grace_period_written_under_terminal_is_told_where_it_belongs) {
    // Silence here would leave two live copies of one setting: the reader's
    // line under [terminal] doing nothing, and the dialog going on writing to
    // [general]. The message names the section that would have worked.
    ScratchConfig scratch("grace-misplaced");
    scratch.write("[terminal]\nkill-grace-seconds = 20\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings.kill_grace_seconds == 5);  // the default stands
    CK_CHECK(loaded.warnings.size() == 1U);
    if (loaded.warnings.size() == 1U) {
        CK_CHECK(warns_at(loaded.warnings[0], scratch.path(), 2));
        CK_CHECK(loaded.warnings[0].find("kill-grace-seconds belongs in [general], not [terminal]") !=
                 std::string::npos);
    }
}

CK_TEST(a_grace_period_outside_its_range_warns_and_keeps_the_default) {
    for (const char* const value : {"-1", "601", "ages", "5.5"}) {
        ScratchConfig scratch("grace-range");
        scratch.write(std::string("[general]\nkill-grace-seconds = ") + value + "\n");
        const ckm::LoadedSettings loaded = load_settings(scratch.path());
        CK_CHECK(loaded.settings.kill_grace_seconds == 5);
        CK_CHECK(loaded.warnings.size() == 1U);
        if (loaded.warnings.size() == 1U) {
            CK_CHECK(loaded.warnings[0].find("between 0 and 600") != std::string::npos);
            CK_CHECK(loaded.warnings[0].find("keeping 5") != std::string::npos);
        }
    }
}

// --- A file that is there and cannot be read ------------------------------

CK_TEST(a_config_that_cannot_be_read_says_so_instead_of_pretending_it_is_absent) {
    // The difference between the two silences: no file is an unconfigured
    // ckmux and says nothing, while an unreadable file is a reader's settings
    // being ignored wholesale. Behaving identically in both cases is how a
    // reader spends an evening wondering why their prefix key changed back.
    ScratchConfig scratch("unreadable");
    scratch.write("[general]\nlogin-shell = false\n");
    if (!make_unreadable(scratch.path())) return;  // root, or a permissionless filesystem
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings == Settings{});  // the built-in defaults, unchanged
    CK_CHECK(loaded.warnings.size() == 1U);
    if (loaded.warnings.size() == 1U) {
        // Named in full, and with what is in force instead.
        CK_CHECK(loaded.warnings[0].rfind(scratch.path().string() + ": ", 0) == 0);
        CK_CHECK(loaded.warnings[0].find("could not be read") != std::string::npos);
        CK_CHECK(loaded.warnings[0].find("keeping the built-in settings") != std::string::npos);
    }
}

CK_TEST(saving_into_a_config_that_cannot_be_read_refuses_rather_than_replacing_it) {
    // An unreadable file read as an empty one would be rewritten as the single
    // key this call was asked to store — the reader's whole configuration
    // replaced by one line, silently, by a dialog they opened to change one
    // setting.
    ScratchConfig scratch("unreadable-save");
    scratch.write("[general]\nlogin-shell = false\nprefix = C-a\n");
    if (!make_unreadable(scratch.path())) return;
    CK_CHECK(!save_setting(scratch.path(), "general", "login-shell", bool_setting(true)));
    std::error_code ignored;
    std::filesystem::permissions(scratch.path(), std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::add, ignored);
    CK_CHECK(scratch.read() == "[general]\nlogin-shell = false\nprefix = C-a\n");
}

// --- A section nobody has ------------------------------------------------

CK_TEST(a_mistyped_section_is_one_warning_at_the_header_not_one_at_every_key) {
    // The reader made one mistake. Four warnings about four keys that are
    // perfectly well spelled would send them looking at the wrong four lines.
    ScratchConfig scratch("bad-section");
    scratch.write(
        "[generall]\n"
        "login-shell = false\n"
        "theme = light\n"
        "clock = off\n"
        "\n"
        "[terminal]\n"
        "mouse = false\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.size() == 1U);
    if (loaded.warnings.size() == 1U) {
        CK_CHECK(warns_at(loaded.warnings[0], scratch.path(), 1));
        CK_CHECK(loaded.warnings[0].find("no such section '[generall]'") != std::string::npos);
        // A reader who copies a word out of the message gets a header that works.
        CK_CHECK(loaded.warnings[0].find("general, terminal, printer, render, keys") !=
                 std::string::npos);
    }
    // Nothing under the header was read...
    CK_CHECK(loaded.settings.login_shell);
    CK_CHECK(loaded.settings.theme == ckm::Theme::Dark);
    CK_CHECK(loaded.settings.clock == ckm::ClockMode::Seconds);
    // ...and the file goes on being read after it.
    CK_CHECK(!loaded.settings.mouse);
}

// --- One key, written twice ----------------------------------------------

CK_TEST(a_key_set_twice_says_so_and_the_last_line_wins) {
    ScratchConfig scratch("duplicate");
    scratch.write("[general]\nlogin-shell = false\ntheme = light\nlogin-shell = true\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.settings.login_shell);  // the last line
    CK_CHECK(loaded.settings.theme == ckm::Theme::Light);
    CK_CHECK(loaded.warnings.size() == 1U);
    if (loaded.warnings.size() == 1U) {
        CK_CHECK(warns_at(loaded.warnings[0], scratch.path(), 4));
        CK_CHECK(loaded.warnings[0].find("login-shell is set again here") != std::string::npos);
    }
}

CK_TEST(saving_a_key_its_section_holds_twice_is_a_save_the_reader_can_see) {
    // The defect: the dialog rewrote the first line, the file was read back
    // last-line-wins, and Save did nothing a reader could observe — twice
    // over, since they would then try again.
    ScratchConfig scratch("duplicate-save");
    scratch.write(
        "[general]\n"
        "login-shell = true   # from when I set this up\n"
        "theme = dark\n"
        "login-shell = true\n");
    CK_CHECK(save_setting(scratch.path(), "general", "login-shell", bool_setting(false)));
    // Both lines rewritten, both notes kept, nothing of the reader's deleted.
    CK_CHECK(scratch.read() ==
             "[general]\n"
             "login-shell = false  # from when I set this up\n"
             "theme = dark\n"
             "login-shell = false\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(!loaded.settings.login_shell);
    // Still their duplicate, and still said out loud.
    CK_CHECK(loaded.warnings.size() == 1U);
}

// --- The one escape ------------------------------------------------------

CK_TEST(a_hash_inside_a_clipboard_command_is_written_with_a_backslash) {
    // Without the escape the command was silently cut at the '#' and a reader
    // got a copy target that ran half of what they wrote.
    ScratchConfig scratch("escape");
    scratch.write("[terminal]\nclipboard = exec:my-copy --tag=\\#1, osc52   # my helper\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.clipboard.size() == 2U);
    if (loaded.settings.clipboard.size() == 2U) {
        CK_CHECK(loaded.settings.clipboard[0].kind == ckm::ClipboardTarget::Kind::Exec);
        CK_CHECK(loaded.settings.clipboard[0].command == "my-copy --tag=#1");
        CK_CHECK(loaded.settings.clipboard[1].kind == ckm::ClipboardTarget::Kind::Osc52);
    }
}

CK_TEST(an_unescaped_hash_is_still_a_comment) {
    // The escape is an addition, not a change of mind: the plan's own annotated
    // example file is full of trailing comments and has to keep working.
    ScratchConfig scratch("escape-comment");
    scratch.write("[terminal]\nclipboard = exec:my-copy # the helper I wrote\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.clipboard.size() == 1U);
    if (loaded.settings.clipboard.size() == 1U)
        CK_CHECK(loaded.settings.clipboard[0].command == "my-copy");
}

CK_TEST(every_other_backslash_in_a_command_is_left_exactly_as_it_was_written) {
    // The reason `\\` and `\n` are not escapes here: these values are shell
    // commands, and a format that rewrote them would break more commands than
    // the escape saves.
    ScratchConfig scratch("escape-shell");
    scratch.write("[terminal]\nclipboard = exec:sed -e 's/\\\\n/ /' | my-copy\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.clipboard.size() == 1U);
    if (loaded.settings.clipboard.size() == 1U)
        CK_CHECK(loaded.settings.clipboard[0].command == "sed -e 's/\\\\n/ /' | my-copy");
}

CK_TEST(a_hash_can_be_bound_as_a_chord) {
    // '#' is a printable character and therefore a chord, so `bind terminal
    // \# …` has to reach parse_chord rather than being read as the start of a
    // comment.
    ScratchConfig scratch("escape-bind");
    scratch.write("[keys]\nbind terminal \\# menu-bar\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.binds.size() == 1U);
    if (loaded.settings.binds.size() == 1U) {
        CK_CHECK(loaded.settings.binds[0].chord == "#");
        CK_CHECK(loaded.settings.binds[0].action == ckm::Action::MenuBar);
    }
}

CK_TEST(a_hash_survives_the_file_a_dialog_writes) {
    // save_setting escapes what it writes, so that what it wrote is what
    // load_settings reads. Nothing a dialog writes today contains a '#'; the
    // round trip should not have to depend on that staying true.
    ScratchConfig scratch("escape-round-trip");
    CK_CHECK(save_setting(scratch.path(), "terminal", "clipboard", "exec:my-copy --tag=#1"));
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.clipboard.size() == 1U);
    if (loaded.settings.clipboard.size() == 1U)
        CK_CHECK(loaded.settings.clipboard[0].command == "my-copy --tag=#1");
}

CK_TEST(the_per_session_terminal_limit_is_read_bounded_and_honoured) {
    // WP-13, from the session model's new-terminal row: "Session at terminal limit
    // (config, default 64) → error". The default is the plan's number, not a
    // number chosen here — a limit the spec states and the code disagrees with
    // is worse than no limit, because both look right in isolation.
    CK_CHECK(ckm::Settings{}.max_terminals == 64);

    ScratchConfig scratch("limit");
    scratch.write("[general]\nmax-terminals = 8\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.max_terminals == 8);

    // Zero is refused rather than accepted as "none": a session that can hold
    // no terminals is a session nothing can be done with, and a reader who
    // wants that closes the session instead.
    ScratchConfig none("limit-zero");
    none.write("[general]\nmax-terminals = 0\n");
    const ckm::LoadedSettings refused = load_settings(none.path());
    CK_CHECK(refused.warnings.size() == 1U);
    if (!refused.warnings.empty())
        CK_CHECK(refused.warnings[0].find("max-terminals") != std::string::npos);
    // Refused means unchanged, not clamped to the floor — a reader who typed
    // something impossible gets the default and a sentence, not a silent
    // reinterpretation of what they asked for.
    CK_CHECK(refused.settings.max_terminals == 64);

    // And it must NOT appear on the not-yet-honoured list: the server refuses
    // NewTerminal past this number the moment the key lands. Landing a key
    // without wiring it, or wiring it without taking it off that list, are the
    // two halves of the defect `on-exit` had — and neither breaks a build.
    for (const std::string& entry : ckm::keys_not_honoured_yet())
        CK_CHECK(entry.find("max-terminals") == std::string::npos);
}

CK_TEST(the_audible_bell_is_off_until_a_reader_asks_and_is_honoured_when_they_do) {
    // WP-19. Only the NOISE is configurable: the border and the footer flag
    // are how a reader learns that a window they are not looking at wants
    // them, which is the whole point of a bell in a multiplexer, so those are
    // always on. What a reader may reasonably not want — especially with
    // several terminals running — is their own terminal ringing.
    CK_CHECK(!ckm::Settings{}.audible_bell);

    ScratchConfig scratch("bell");
    scratch.write("[general]\naudible-bell = true\n");
    const ckm::LoadedSettings loaded = load_settings(scratch.path());
    CK_CHECK(loaded.warnings.empty());
    CK_CHECK(loaded.settings.audible_bell);

    // And it must not be on the not-yet-honoured list: the client rings the
    // host the moment the key lands. Landing a key without wiring it, or
    // wiring it and leaving it on that list, are the two halves of the defect
    // `on-exit` had, and neither breaks a build.
    for (const std::string& entry : ckm::keys_not_honoured_yet())
        CK_CHECK(entry.find("audible-bell") == std::string::npos);
}
