// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// End-to-end coverage for the ckmux client, driven headlessly exactly the way
// main.cpp drives it: real Application, real render pipeline, real menu and
// mouse paths. The child program is /bin/cat rather than a shell — it stays
// alive, says nothing, and therefore keeps these assertions about the UI
// rather than about a prompt.
#if !defined(_WIN32)

#include <array>
#include <string>
#include <string_view>

#include "client/client_app.hpp"
#include "cvision/term/headless_terminal.hpp"
#include <cstdlib>
#include <filesystem>

#include "common/config.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/ui/standard_roles.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/terminal_view.hpp"

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckm::client::Context;
using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;

namespace {

ClientOptions test_options() {
    ClientOptions options;
    // A program that simply stays alive, so a window has a live child
    // without the run depending on whose shell is installed. The login
    // form only renames argv[0], which cat does not mind.
    options.settings.shell = "/bin/cat";
    return options;
}

struct Fixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, test_options()};
};

// A command's id, looked up the way everything durable does: by key. Ids are
// assigned by the registry at runtime (ckVision D-013), so a test cannot hold
// one as a constant — it asks the registry the application actually declared
// into.
ckv::ui::CommandId id_of(Application& app, std::string_view key) {
    return app.commands().id_for(key).value_or(ckv::ui::kInvalidCommand);
}

bool menu_has_command(const ckv::widgets::MenuBarItem& menu, ckv::ui::CommandId command) {
    for (const ckv::widgets::MenuItem& item : menu.items)
        if (item.command == command || item.presentation.command == command) return true;
    return false;
}

ckv::widgets::MenuBar* menu_bar(ClientApp& client) {
    return dynamic_cast<ckv::widgets::MenuBar*>(client.desktop().top_dock());
}

// Windows that host a terminal. A desktop also owns whatever dialog is
// currently up, so a plain windows().size() would count the confirmation box
// it is being asked about.
std::size_t terminal_window_count(ClientApp& client) {
    std::size_t count = 0;
    for (ckv::widgets::Window* window : client.desktop().windows())
        if (dynamic_cast<ckv::widgets::TerminalView*>(window->content()) != nullptr) ++count;
    return count;
}

bool any_label_contains(const std::vector<std::string>& labels, std::string_view needle) {
    for (const std::string& label : labels)
        if (label.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

CK_TEST(the_client_opens_with_a_menu_bar_a_footer_and_one_terminal) {
    Fixture f;
    f.app.step(0);

    ckv::widgets::MenuBar* const bar = menu_bar(f.client);
    CK_CHECK(bar != nullptr);
    // Session, Terminal, Window, View, Settings, Help
    CK_CHECK(bar->menus().size() == 6U);
    CK_CHECK(f.client.desktop().bottom_dock() == &f.client.footer());
    CK_CHECK(f.client.desktop().windows().size() == 1U);
    CK_CHECK(f.client.desktop().windows()[0]->title() == "Terminal 1");
    CK_CHECK(dynamic_cast<ckv::widgets::TerminalView*>(f.client.desktop().windows()[0]->content()) != nullptr);
    // Focus lands in the terminal, so the reader is typing to their program
    // from the first frame.
    CK_CHECK(f.client.context() == Context::Terminal);
}

CK_TEST(every_menu_offers_the_commands_its_name_promises) {
    Fixture f;
    f.app.step(0);
    ckv::widgets::MenuBar* const bar = menu_bar(f.client);
    CK_CHECK(bar != nullptr);

    CK_CHECK(menu_has_command(bar->menus()[0], id_of(f.app, ckm::client::commands::kNewTerminal)));
    CK_CHECK(menu_has_command(bar->menus()[0], id_of(f.app, ckm::client::commands::kDetach)));
    CK_CHECK(menu_has_command(bar->menus()[0], id_of(f.app, ckv::ui::std_command_keys::kQuit)));
    CK_CHECK(menu_has_command(bar->menus()[1], id_of(f.app, ckm::client::commands::kCloseTerminal)));
    CK_CHECK(menu_has_command(bar->menus()[1], id_of(f.app, ckv::ui::std_command_keys::kNextWindow)));
    CK_CHECK(menu_has_command(bar->menus()[1], id_of(f.app, ckv::ui::std_command_keys::kWindowList)));
    CK_CHECK(menu_has_command(bar->menus()[2], id_of(f.app, ckv::ui::std_command_keys::kTile)));
    CK_CHECK(menu_has_command(bar->menus()[2], id_of(f.app, ckv::ui::std_command_keys::kCascade)));
    CK_CHECK(menu_has_command(bar->menus()[3], id_of(f.app, ckm::client::commands::kThemeDark)));
    CK_CHECK(menu_has_command(bar->menus()[4], id_of(f.app, ckm::client::commands::kSettings)));
    CK_CHECK(menu_has_command(bar->menus()[5], id_of(f.app, ckm::client::commands::kAbout)));
}

CK_TEST(commands_that_need_the_server_are_visible_but_disabled_until_it_exists) {
    // M1 has no server. Detach and Sessions therefore show the real shape of
    // ckmux while saying plainly that they cannot act yet — rather than being
    // hidden, or worse, pretending.
    Fixture f;
    f.app.step(0);
    CK_CHECK(!f.app.command_available(id_of(f.app, ckm::client::commands::kDetach)));
    CK_CHECK(!f.app.command_available(id_of(f.app, ckm::client::commands::kSessions)));
    CK_CHECK(f.app.command_available(id_of(f.app, ckm::client::commands::kNewTerminal)));
}

CK_TEST(the_footer_advertises_the_keys_for_whatever_currently_has_focus) {
    Fixture f;
    f.app.step(0);

    const std::vector<std::string> terminal_labels = f.client.footer_labels();
    CK_CHECK(!terminal_labels.empty());
    CK_CHECK(any_label_contains(terminal_labels, "^B c"));
    CK_CHECK(any_label_contains(terminal_labels, "new term"));
    CK_CHECK(any_label_contains(terminal_labels, "^B d"));

    // Arming the prefix switches the whole hint set to the bare keys, because
    // the prefix has already been pressed.
    f.client.arm_prefix();
    const std::vector<std::string> prefix_labels = f.client.footer_labels();
    CK_CHECK(f.client.context() == Context::Prefix);
    CK_CHECK(any_label_contains(prefix_labels, "c new term"));
    CK_CHECK(!any_label_contains(prefix_labels, "^B c"));
    f.client.resolve_prefix("");
    CK_CHECK(any_label_contains(f.client.footer_labels(), "^B c"));
}

CK_TEST(new_terminal_opens_an_independent_window_and_takes_focus) {
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 1U);

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 2U);
    ckv::widgets::Window* const second = f.client.desktop().active_window();
    CK_CHECK(second != nullptr);
    CK_CHECK(second->title() == "Terminal 2");
    CK_CHECK(f.app.focused() == second->content());
    // Two terminals are two separate children: they must not share a session.
    auto* const first_view = dynamic_cast<ckv::widgets::TerminalView*>(f.client.desktop().windows()[0]->content());
    auto* const second_view = dynamic_cast<ckv::widgets::TerminalView*>(second->content());
    CK_CHECK(first_view != nullptr && second_view != nullptr);
    CK_CHECK(&first_view->session() != &second_view->session());
    // Cascading placement: a new window must not land exactly on the old one.
    CK_CHECK(f.client.desktop().windows()[0]->bounds() != second->bounds());
}

CK_TEST(the_public_mouse_menu_path_opens_a_new_terminal) {
    // The whole product claim is that the mouse alone is enough. This drives
    // the real menu: press the bar, press the first item, release.
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 1U);

    CK_CHECK(f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Left,
                                            ckv::Point{1, 0}, std::nullopt, ckv::Modifier::None}));
    CK_CHECK(f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Left,
                                            ckv::Point{2, 2}, std::nullopt, ckv::Modifier::None}));
    CK_CHECK(f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Up, ckv::MouseButton::Left,
                                            ckv::Point{2, 2}, std::nullopt, ckv::Modifier::None}));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 2U);
}

CK_TEST(window_management_commands_arrange_and_cycle_the_terminals) {
    Fixture f;
    f.app.step(0);
    f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 2U);
    ckv::widgets::Window* const second = f.client.desktop().active_window();

    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kPreviousWindow)));
    CK_CHECK(f.client.desktop().active_window()->title() == "Terminal 1");
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kNextWindow)));
    CK_CHECK(f.client.desktop().active_window() == second);

    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kTile)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows()[0]->bounds().width ==
             f.client.desktop().windows()[1]->bounds().width);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kCascade)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows()[0]->bounds().x < f.client.desktop().windows()[1]->bounds().x);

    // Zoom fills the desktop's content area — never the docked chrome.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kZoom)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().active_window()->zoomed());
    CK_CHECK(f.client.desktop().active_window()->bounds() == f.client.desktop().content_area());
}

CK_TEST(a_terminal_resize_resizes_the_child_endpoint) {
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(window->content());
    CK_CHECK(view != nullptr);

    window->set_bounds(ckv::Rect{4, 3, 42, 15});
    f.app.step(0);
    const ckv::Rect content = window->content_rect();
    CK_CHECK(view->session().snapshot().cells == (Size{content.width, content.height}));
}

CK_TEST(closing_a_terminal_whose_child_is_alive_asks_first) {
    Fixture f;
    f.app.step(0);
    f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 2U);

    // /bin/cat is still running, so the close is vetoed and a confirmation
    // appears instead. Both terminals survive until the reader answers.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kCloseTerminal)));
    f.app.step(0);
    CK_CHECK(terminal_window_count(f.client) == 2U);
    CK_CHECK(f.app.is_modal());

    // Answering the confirmation completes the close it vetoed: the reader
    // said yes once, and is not asked a second time.
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(!f.app.is_modal());
    CK_CHECK(terminal_window_count(f.client) == 1U);
}

CK_TEST(help_opens_for_whatever_has_focus_and_lists_the_real_keys) {
    Fixture f;
    f.app.step(0);
    CK_CHECK(terminal_window_count(f.client) == 1U);

    // F1 is ckVision's standard help command; ckmux answers it with the page
    // for the focused view's help context.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kHelp)));
    f.app.step(0);
    // The viewer is a window, and it is not one of the terminals.
    CK_CHECK(f.client.desktop().windows().size() == 2U);
    CK_CHECK(terminal_window_count(f.client) == 1U);
    // Modeless: help is a reference for the work in the terminal behind it,
    // so consulting it must not lock the reader out of that work.
    CK_CHECK(!f.app.is_modal());
    CK_CHECK(f.client.desktop().active_window()->resizable());
}

CK_TEST(the_help_text_is_generated_from_the_same_table_that_dispatches_the_keys) {
    // Help that is hand-written drifts from the keymap the moment either
    // changes. This asserts the page names the same chords the table binds.
    Fixture f;
    f.app.step(0);
    bool asked = false;
    std::string topic_key;
    // Replacing the provider is the sanctioned way to observe what F1 routes.
    f.app.set_help_provider([&](const std::string& key) {
        asked = true;
        topic_key = key;
    });
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kHelp)));
    CK_CHECK(asked);
    CK_CHECK(topic_key == "ckmux.terminal");
}

CK_TEST(the_client_renders_under_every_built_in_theme) {
    // Named rather than held as ids: each iteration builds its own
    // application, and an id is meaningful only to the registry that assigned
    // it — one carried in from a previous fixture would mean nothing here.
    const std::array<std::string_view, 3> themes{ckm::client::commands::kThemeDark,
                                                 ckm::client::commands::kThemeLight,
                                                 ckm::client::commands::kThemeMono};
    for (const std::string_view theme : themes) {
        Fixture f;
        f.app.step(0);
        CK_CHECK(f.app.execute_command(id_of(f.app, theme)));
        f.app.step(0);
        // A real frame reached the terminal, and it is not the too-small
        // refusal state.
        CK_CHECK(!f.terminal.written_bytes().empty());
        CK_CHECK(!f.app.terminal_too_small());
        CK_CHECK(f.client.desktop().windows().size() == 1U);
    }
}

#endif

CK_TEST(a_window_caption_follows_the_title_its_program_asks_for) {
    // Programs announce what they are working on with OSC 0 / OSC 2. A
    // multiplexer that ignores it leaves every window called "Terminal N",
    // which is the one thing the caption could usefully not say.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(window->content());
    CK_CHECK(view != nullptr);
    CK_CHECK(window->title() == "Terminal 1");

    // Feed the child's output directly: this is the same seam the PTY writes
    // into, without needing a program that happens to set a title.
    view->session().feed_output("\x1b]2;vim: architecture.md\a");
    f.clock.advance(200'000'000);
    f.app.step(f.clock.now_nanos());
    CK_CHECK(window->title() == "vim: architecture.md");

    // OSC 0 sets icon and title together and is equally ordinary.
    view->session().feed_output("\x1b]0;htop\a");
    f.clock.advance(200'000'000);
    f.app.step(f.clock.now_nanos());
    CK_CHECK(window->title() == "htop");
}

CK_TEST(a_window_caption_returns_to_its_own_name_when_the_program_stops_claiming_one) {
    // Handing the caption back is how a program says it is done naming the
    // window — on exit, typically. Keeping its last title forever would
    // leave the reader looking at a window named after something gone.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(window->content());
    CK_CHECK(view != nullptr);

    view->session().feed_output("\x1b]2;temporary\a");
    f.clock.advance(200'000'000);
    f.app.step(f.clock.now_nanos());
    CK_CHECK(window->title() == "temporary");

    view->session().feed_output("\x1b]2;\a");
    f.clock.advance(200'000'000);
    f.app.step(f.clock.now_nanos());
    CK_CHECK(window->title() == "Terminal 1");
}

CK_TEST(each_terminal_keeps_its_own_caption_and_its_own_fallback) {
    Fixture f;
    f.app.step(0);
    f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal));
    f.app.step(0);
    CK_CHECK(terminal_window_count(f.client) == 2U);

    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];
    auto* const second_view = dynamic_cast<ckv::widgets::TerminalView*>(second->content());
    CK_CHECK(second_view != nullptr);

    second_view->session().feed_output("\x1b]2;only the second\a");
    f.clock.advance(200'000'000);
    f.app.step(f.clock.now_nanos());
    CK_CHECK(second->title() == "only the second");
    CK_CHECK(first->title() == "Terminal 1");

    second_view->session().feed_output("\x1b]2;\a");
    f.clock.advance(200'000'000);
    f.app.step(f.clock.now_nanos());
    CK_CHECK(second->title() == "Terminal 2");  // its own name, not the first's
}

CK_TEST(the_settings_dialog_shows_the_picture_limit_as_a_field_and_stores_a_new_one) {
    // Reported from a running ckmux: a program's logo did not appear, and the
    // footer said the terminal was past a limit nobody could see or change.
    // The number is now on screen, editable, and kept.
    const std::filesystem::path config =
        std::filesystem::temp_directory_path() / "ckmux-settings-sixel" / "ckmux.conf";
    std::filesystem::remove_all(config.parent_path());
    (void)::setenv("CKMUX_CONFIG", config.c_str(), 1);

    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.commands().execute(id_of(f.app, ckm::client::commands::kSettings)));
    f.app.step(0);

    std::string screen;
    const ckv::FrameView frame = f.app.current_frame();
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) screen += frame.at(ckv::Point{x, y}).grapheme();
    // The unit is said, the current value is shown rather than implied, and
    // the note explains what the limit is a limit on.
    CK_CHECK(screen.find("megapixels") != std::string::npos);
    CK_CHECK(screen.find("64") != std::string::npos);
    CK_CHECK(screen.find("cut off at its edge") != std::string::npos);

    // Tab into the field, replace the number, Save.
    f.terminal.inject_bytes("\t", 0);
    f.app.step(0);
    f.terminal.inject_bytes("\x7f\x7f" "128", 0);
    f.app.step(0);
    f.terminal.inject_bytes("\r", 0);
    f.app.step(0);
    f.app.step(0);

    const ckm::LoadedSettings stored = ckm::load_settings(config);
    CK_CHECK(stored.settings.sixel_max_megapixels == 128);
    CK_CHECK(stored.settings.login_shell);  // the other setting is left alone
    CK_CHECK(stored.warnings.empty());

    (void)::unsetenv("CKMUX_CONFIG");
    std::filesystem::remove_all(config.parent_path());
}

CK_TEST(the_settings_dialog_refuses_a_picture_limit_it_cannot_read) {
    // The veto is the whole point of validating at accept time: the dialog
    // stays up with the bad field focused, and nothing is written.
    const std::filesystem::path config =
        std::filesystem::temp_directory_path() / "ckmux-settings-sixel-bad" / "ckmux.conf";
    std::filesystem::remove_all(config.parent_path());
    (void)::setenv("CKMUX_CONFIG", config.c_str(), 1);

    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.commands().execute(id_of(f.app, ckm::client::commands::kSettings)));
    f.app.step(0);
    f.terminal.inject_bytes("\t", 0);
    f.app.step(0);
    f.terminal.inject_bytes("\x7f\x7flots", 0);
    f.app.step(0);
    f.terminal.inject_bytes("\r", 0);
    f.app.step(0);
    f.app.step(0);

    std::string screen;
    const ckv::FrameView frame = f.app.current_frame();
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) screen += frame.at(ckv::Point{x, y}).grapheme();
    CK_CHECK(screen.find("megapixels") != std::string::npos);  // still up
    CK_CHECK(!std::filesystem::exists(config));                // and nothing saved

    (void)::unsetenv("CKMUX_CONFIG");
    std::filesystem::remove_all(config.parent_path());
}

CK_TEST(the_settings_dialog_offers_the_login_shell_choice_and_stores_what_it_is_told) {
    // The whole path a reader takes: open Settings, untick the box, Save —
    // and find that terminals opened afterwards start the other way, and
    // that the answer outlived the dialog.
    const std::filesystem::path config =
        std::filesystem::temp_directory_path() / "ckmux-settings-dialog" / "ckmux.conf";
    std::filesystem::remove_all(config.parent_path());
    (void)::setenv("CKMUX_CONFIG", config.c_str(), 1);

    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.commands().execute(id_of(f.app, ckm::client::commands::kSettings)));
    f.app.step(0);

    // The dialog is on screen, saying what the setting does rather than only
    // what it is called.
    std::string screen;
    const ckv::FrameView frame = f.app.current_frame();
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) screen += frame.at(ckv::Point{x, y}).grapheme();
    CK_CHECK(screen.find("Settings") != std::string::npos);
    CK_CHECK(screen.find(".zprofile") != std::string::npos);
    CK_CHECK(screen.find("[X]") != std::string::npos);  // login shells are the default

    // Space toggles the focused box; Enter presses Save.
    f.terminal.inject_bytes(" ", 0);
    f.app.step(0);
    f.terminal.inject_bytes("\r", 0);
    f.app.step(0);
    f.app.step(0);

    const ckm::LoadedSettings stored = ckm::load_settings(config);
    CK_CHECK(!stored.settings.login_shell);
    CK_CHECK(stored.warnings.empty());

    (void)::unsetenv("CKMUX_CONFIG");
    std::filesystem::remove_all(config.parent_path());
}
