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
#include "common/proto.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/ui/standard_roles.hpp"
#include "cvision/widgets/common_components.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/terminal_view.hpp"

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckm::client::Context;
using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;

namespace {

// 09:41 on 17 August 2026, wherever this test is run. Injected exactly where
// main.cpp injects the host's own reading, so the menu-bar clock and the
// calendar under it are as deterministic as everything else here — a test that
// asserted on the real time would pass until midnight.
constexpr ckv::widgets::DateValue kToday{2026, 8, 17};
constexpr ckv::widgets::TimeValue kNow{9, 41, 0};

ClientOptions test_options() {
    ClientOptions options;
    // A program that simply stays alive, so a window has a live child
    // without the run depending on whose shell is installed. The login
    // form only renames argv[0], which cat does not mind.
    options.settings.shell = "/bin/cat";
    options.local_now = [] { return ckm::client::LocalMoment{kToday, kNow}; };
    return options;
}

struct Fixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, test_options()};
};

// The same fixture with the settings a test wants to change first.
struct ConfiguredFixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client;

    explicit ConfiguredFixture(ClientOptions options) : client{app, std::move(options)} {}
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
        if (item.command() == command) return true;
    return false;
}

ckv::widgets::MenuBar* menu_bar(ClientApp& client) {
    return dynamic_cast<ckv::widgets::MenuBar*>(client.desktop().top_dock());
}

// The calendar currently hanging off the clock, if any. It is a desktop popup
// like a menu is, and dismissing it destroys it, so its absence is the whole
// of "closed".
ckv::widgets::CalendarDropdown* dropped_calendar(ClientApp& client) {
    for (ckv::ui::View* popup : client.desktop().popups())
        if (auto* const calendar = dynamic_cast<ckv::widgets::CalendarDropdown*>(popup)) return calendar;
    return nullptr;
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

// Walks the focus to the combo box currently showing `showing`, then chooses
// the entry `steps` further down its list — Down opens the list, each further
// Down moves one entry, Enter chooses. The reader's own path: a closed combo
// does not step in place while there is room to drop a list.
template <typename FixtureType>
ckv::widgets::ComboBox* choose_in_combo(FixtureType& f, std::string_view showing, int steps) {
    ckv::widgets::ComboBox* combo = nullptr;
    for (int step = 0; step < 12 && combo == nullptr; ++step) {
        f.app.focus_next();
        auto* const box = dynamic_cast<ckv::widgets::ComboBox*>(f.app.focused());
        if (box != nullptr && box->text() == showing) combo = box;
    }
    if (combo == nullptr) return nullptr;
    const auto press = [&f](ckv::Key key) {
        f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{key, ckv::Modifier::None, {}}});
        f.app.step(0);
    };
    press(ckv::Key::Down);  // opens the list on the entry it is showing
    for (int step = 0; step < steps; ++step) press(ckv::Key::Down);
    press(ckv::Key::Enter);  // chosen; the list closes and the combo keeps focus
    return combo;
}

bool any_label_contains(const std::vector<std::string>& labels, std::string_view needle) {
    for (const std::string& label : labels)
        if (label.find(needle) != std::string::npos) return true;
    return false;
}

// --- WP-29: what the client reports about where its windows are -------------

// One turn of the layout timer. The client samples the arrangement this often
// and reports it only once two consecutive samples agree, so a change reaches
// the seam on the second turn after it — and a desktop nobody is touching
// reaches it never, however many turns go by.
constexpr std::int64_t kLayoutSettleNanos = 150'000'000;

template <typename FixtureType>
void layout_tick(FixtureType& f) {
    f.clock.advance(kLayoutSettleNanos);
    f.app.step(f.clock.now_nanos());
}

// The terminal a window is showing. That is how a placement names one: the
// window layer has no wire ids and never will — `run_client.cpp` is where a
// subsession becomes an id (client_app.hpp's WindowPlacement).
const ckv::core::TerminalSubsession* terminal_of(ckv::widgets::Window* window) {
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(window->content());
    return view == nullptr ? nullptr : &view->session();
}

// The same handle again, as the CLIENT's own map is keyed by it. A
// `TerminalView` borrows the base class, so the launched subsession behind one
// is recovered with a cast rather than by widening anything: every terminal
// window in this fixture is showing one the application launched.
const ckv::term::TerminalSubsession* launched_terminal_of(ckv::widgets::Window* window) {
    return dynamic_cast<const ckv::term::TerminalSubsession*>(terminal_of(window));
}

using LayoutReports = std::vector<std::vector<ckm::client::WindowPlacement>>;

// The client with somebody listening to its arrangement, which is what an
// attached client is: with no `report_layout` there is no server to tell, the
// timer is never started, and this is the M1 client unchanged.
ClientOptions watching_layout(LayoutReports& reports) {
    ClientOptions options = test_options();
    options.layout_settle_nanos = kLayoutSettleNanos;
    options.report_layout = [&reports](const std::vector<ckm::client::WindowPlacement>& arrangement) {
        reports.push_back(arrangement);
    };
    return options;
}

}  // namespace

CK_TEST(the_client_opens_with_a_menu_bar_a_footer_and_one_terminal) {
    Fixture f;
    f.app.step(0);

    ckv::widgets::MenuBar* const bar = menu_bar(f.client);
    CK_CHECK(bar != nullptr);
    // Session, Terminal, Window, Settings, Help — no View menu: the colour
    // theme is a stored setting, chosen in Settings ▸ General…
    CK_CHECK(bar->menus().size() == 5U);
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

    // Session holds what happens to sessions; a terminal is the Terminal
    // menu's to make. New Terminal used to sit under both titles, and the
    // same item under two names is a reader wondering which one they used.
    CK_CHECK(menu_has_command(bar->menus()[0], id_of(f.app, ckm::client::commands::kNewSession)));
    CK_CHECK(!menu_has_command(bar->menus()[0], id_of(f.app, ckm::client::commands::kNewTerminal)));
    CK_CHECK(menu_has_command(bar->menus()[0], id_of(f.app, ckm::client::commands::kDetach)));
    CK_CHECK(menu_has_command(bar->menus()[0], id_of(f.app, ckv::ui::std_command_keys::kQuit)));
    CK_CHECK(menu_has_command(bar->menus()[1], id_of(f.app, ckm::client::commands::kNewTerminal)));
    CK_CHECK(menu_has_command(bar->menus()[1], id_of(f.app, ckm::client::commands::kCloseTerminal)));
    CK_CHECK(menu_has_command(bar->menus()[1], id_of(f.app, ckv::ui::std_command_keys::kNextWindow)));
    // The window list is a list of windows, so it lives under Window with the
    // commands that arrange them — not under Terminal, where M1 left it.
    CK_CHECK(!menu_has_command(bar->menus()[1], id_of(f.app, ckv::ui::std_command_keys::kWindowList)));
    CK_CHECK(menu_has_command(bar->menus()[2], id_of(f.app, ckv::ui::std_command_keys::kWindowList)));
    CK_CHECK(menu_has_command(bar->menus()[2], id_of(f.app, ckv::ui::std_command_keys::kTile)));
    CK_CHECK(menu_has_command(bar->menus()[2], id_of(f.app, ckv::ui::std_command_keys::kCascade)));
    CK_CHECK(menu_has_command(bar->menus()[3], id_of(f.app, ckm::client::commands::kSettings)));
    CK_CHECK(menu_has_command(bar->menus()[4], id_of(f.app, ckm::client::commands::kAbout)));
}

CK_TEST(the_menu_bar_carries_the_clock_at_its_right_end_and_keeps_it_there) {
    Fixture f;
    f.app.step(0);

    ckv::widgets::MenuBar* const bar = menu_bar(f.client);
    ckv::widgets::ClockView* const clock = f.client.clock();
    CK_CHECK(bar != nullptr);
    CK_CHECK(clock != nullptr);
    CK_CHECK(bar->trailing_view() == clock);
    // What the reader reads, at the end of the row they read it at — seconds
    // included, which is what ckmux ships with.
    CK_CHECK(clock->show_seconds());
    CK_CHECK(clock->text() == std::string("09:41:00"));
    CK_CHECK(clock->absolute_bounds().right() == bar->absolute_bounds().right());
    // And it reached the frame, not merely the widget tree.
    std::string top_row;
    const ckv::FrameView frame = f.app.current_frame();
    for (int x = 0; x < frame.size().width; ++x) top_row += frame.at(ckv::Point{x, 0}).grapheme();
    CK_CHECK(top_row.find("09:41:00") != std::string::npos);

    // A narrower terminal moves the right end, and the clock with it — the
    // failure this pins is a clock left sitting where the edge used to be.
    f.terminal.resize(Size{72, 24});
    f.app.step(0);
    CK_CHECK(bar->absolute_bounds().right() == 72);
    CK_CHECK(clock->absolute_bounds().right() == 72);
}

CK_TEST(clicking_the_clock_drops_a_calendar_opened_on_today) {
    Fixture f;
    f.app.step(0);
    ckv::widgets::ClockView* const clock = f.client.clock();
    CK_CHECK(clock != nullptr);
    CK_CHECK(dropped_calendar(f.client) == nullptr);

    // The reader's own path: press and release on the clock.
    const ckv::Rect on_screen = clock->absolute_bounds();
    const ckv::Point hit{on_screen.x + on_screen.width / 2, on_screen.y};
    f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Left, hit, std::nullopt,
                                   ckv::Modifier::None});
    f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Up, ckv::MouseButton::Left, hit, std::nullopt,
                                   ckv::Modifier::None});
    f.app.step(0);

    ckv::widgets::CalendarDropdown* const calendar = dropped_calendar(f.client);
    CK_CHECK(calendar != nullptr);
    // It hangs under the clock, right edges aligned, and the clock draws as an
    // open title while it does — the way a menu title does under its menu.
    CK_CHECK(calendar->absolute_bounds().right() == on_screen.right());
    CK_CHECK(calendar->absolute_bounds().y == on_screen.bottom());
    CK_CHECK(clock->open());
    // Opened on today, with today chosen: a calendar that opens on a month the
    // reader has to go and find is a calendar they have to operate.
    CK_CHECK(calendar->calendar().selected() == kToday);
    CK_CHECK(calendar->year_field().text() == std::string("2026"));
    CK_CHECK(calendar->month_picker().selected_index() == std::optional<std::size_t>{7});

    // Esc closes it, gives the keyboard back, and the clock stops drawing as
    // open — the last of which is the wiring that is easy to leave out.
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, {}}});
    f.app.step(0);
    CK_CHECK(dropped_calendar(f.client) == nullptr);
    CK_CHECK(!clock->open());
    CK_CHECK(f.app.input_capture() == nullptr);
}

CK_TEST(the_keyboard_reaches_the_clock_by_walking_the_menu_bar) {
    // A ckmux reader gets to the bar with the prefix and then the menu key, so
    // the clock has to be reachable from there too: one step left of the first
    // menu is the trailing title, and Enter opens what hangs from it.
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kMenu)));
    f.app.step(0);
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Left, ckv::Modifier::None, {}}});
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, {}}});
    f.app.step(0);

    CK_CHECK(dropped_calendar(f.client) != nullptr);
    CK_CHECK(f.client.clock()->open());
}

CK_TEST(a_reader_who_turns_the_clock_off_gets_a_bar_without_one) {
    ClientOptions options = test_options();
    options.settings.clock = ckm::ClockMode::Off;
    ConfiguredFixture f{std::move(options)};
    f.app.step(0);

    CK_CHECK(f.client.clock() == nullptr);
    CK_CHECK(menu_bar(f.client)->trailing_view() == nullptr);
}

CK_TEST(a_clock_without_seconds_shows_hours_and_minutes_only) {
    ClientOptions options = test_options();
    options.settings.clock = ckm::ClockMode::Minutes;
    ConfiguredFixture f{std::move(options)};
    f.app.step(0);

    CK_CHECK(f.client.clock() != nullptr);
    CK_CHECK(!f.client.clock()->show_seconds());
    CK_CHECK(f.client.clock()->text() == std::string("09:41"));
}

CK_TEST(a_client_with_no_way_to_read_the_time_shows_no_clock_at_all) {
    // A clock that guesses is worse than no clock, so the absence of an
    // injected reading is the absence of the widget — not a 00:00 in the
    // chrome.
    ClientOptions options = test_options();
    CK_CHECK(options.settings.clock != ckm::ClockMode::Off);  // on, the way it ships
    options.local_now = nullptr;                              // and no way to know the time
    ConfiguredFixture f{std::move(options)};
    f.app.step(0);

    CK_CHECK(f.client.clock() == nullptr);
    CK_CHECK(menu_bar(f.client)->trailing_view() == nullptr);
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

    // The Terminal menu's title, then its first item — New Terminal moved
    // there when the Session menu became about sessions. The bar draws
    // "  Session  Terminal  …", so the second title starts at column 11.
    CK_CHECK(f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Left,
                                            ckv::Point{12, 0}, std::nullopt, ckv::Modifier::None}));
    CK_CHECK(f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Left,
                                            ckv::Point{13, 2}, std::nullopt, ckv::Modifier::None}));
    CK_CHECK(f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Up, ckv::MouseButton::Left,
                                            ckv::Point{13, 2}, std::nullopt, ckv::Modifier::None}));
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

CK_TEST(the_client_renders_under_every_built_in_theme_and_uses_the_configured_one) {
    // The theme is a stored setting rather than a menu item, so this drives it
    // the way a reader's configuration file does. What it also pins is that the
    // chrome does not overrule the file: the shell used to be handed a Dark
    // theme of its own after the configured one had been applied, so `theme =
    // light` drew dark and the setting looked broken rather than ignored.
    const std::array<ckm::Theme, 3> themes{ckm::Theme::Dark, ckm::Theme::Light, ckm::Theme::Mono};
    for (const ckm::Theme theme : themes) {
        ClientOptions options = test_options();
        options.settings.theme = theme;
        ConfiguredFixture f{std::move(options)};
        f.app.step(0);
        // A real frame reached the terminal, and it is not the too-small
        // refusal state.
        CK_CHECK(!f.terminal.written_bytes().empty());
        CK_CHECK(!f.app.terminal_too_small());
        CK_CHECK(f.client.desktop().windows().size() == 1U);
    }

    // Two of them, compared: the same frame drawn under Dark and under Light
    // cannot be identical, which is what "the setting reached the renderer"
    // means when the assertion cannot name a colour.
    const auto first_frame_bytes = [](ckm::Theme theme) {
        ClientOptions options = test_options();
        options.settings.theme = theme;
        ConfiguredFixture f{std::move(options)};
        f.app.step(0);
        return std::string(f.terminal.written_bytes());
    };
    CK_CHECK(first_frame_bytes(ckm::Theme::Dark) != first_frame_bytes(ckm::Theme::Light));
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

CK_TEST(the_settings_dialog_still_fits_an_eighty_by_twentyfour_terminal) {
    // Two more settings are two more rows, and 80x24 is a real size for a
    // multiplexer to be run in: the desktop is 22 rows between the menu bar and
    // the footer, which is exactly what this dialog asks for. The failure this
    // pins is a form whose last field or whose buttons are off the bottom.
    Fixture f;
    f.terminal.resize(Size{80, 24});
    f.app.step(0);
    CK_CHECK(f.app.commands().execute(id_of(f.app, ckm::client::commands::kSettings)));
    f.app.step(0);
    CK_CHECK(!f.app.terminal_too_small());

    std::string screen;
    const ckv::FrameView frame = f.app.current_frame();
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) screen += frame.at(ckv::Point{x, y}).grapheme();
    // Top of the form, both new fields, and the actions: nothing pushed off.
    CK_CHECK(screen.find("Start terminals") != std::string::npos);
    CK_CHECK(screen.find("megapixels") != std::string::npos);
    CK_CHECK(screen.find("Clock in the menu bar") != std::string::npos);
    CK_CHECK(screen.find("Colour theme") != std::string::npos);
    CK_CHECK(screen.find("Save") != std::string::npos);
    CK_CHECK(screen.find("Cancel") != std::string::npos);
}

CK_TEST(cancel_closes_the_settings_dialog_and_stores_nothing) {
    // Reported from a running ckmux: Cancel did nothing at all and the dialog
    // could only be left with Esc. A button that is on screen and inert is
    // worse than one that is not there.
    const std::filesystem::path config =
        std::filesystem::temp_directory_path() / "ckmux-settings-cancel" / "ckmux.conf";
    std::filesystem::remove_all(config.parent_path());
    (void)::setenv("CKMUX_CONFIG", config.c_str(), 1);

    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.commands().execute(id_of(f.app, ckm::client::commands::kSettings)));
    f.app.step(0);
    CK_CHECK(f.app.is_modal());

    // Untick the login-shell box on the way, so a Cancel that stored anything
    // would leave evidence.
    f.terminal.inject_bytes(" ", 0);
    f.app.step(0);

    // Tab to Cancel and press it, the way a reader who dislikes Esc does.
    ckv::widgets::Button* cancel = nullptr;
    for (int step = 0; step < 12 && cancel == nullptr; ++step) {
        f.app.focus_next();
        auto* const button = dynamic_cast<ckv::widgets::Button*>(f.app.focused());
        if (button != nullptr && button->text() == "&Cancel") cancel = button;
    }
    CK_CHECK(cancel != nullptr);
    f.terminal.inject_bytes(" ", 0);
    f.app.step(0);
    f.app.step(0);

    // The dialog is gone, the setting is untouched, and nothing was written.
    CK_CHECK(!f.app.is_modal());
    CK_CHECK(!std::filesystem::exists(config));
    std::string screen;
    const ckv::FrameView frame = f.app.current_frame();
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) screen += frame.at(ckv::Point{x, y}).grapheme();
    CK_CHECK(screen.find(".zprofile") == std::string::npos);

    (void)::unsetenv("CKMUX_CONFIG");
    std::filesystem::remove_all(config.parent_path());
}

CK_TEST(the_settings_dialog_chooses_the_clock_and_the_theme_and_keeps_both) {
    // Both are chrome the reader is looking at while they choose, so this pins
    // the two halves that matter: the bar changes as the dialog closes, and the
    // answer is in the file for tomorrow.
    const std::filesystem::path config =
        std::filesystem::temp_directory_path() / "ckmux-settings-chrome" / "ckmux.conf";
    std::filesystem::remove_all(config.parent_path());
    (void)::setenv("CKMUX_CONFIG", config.c_str(), 1);

    Fixture f;
    f.app.step(0);
    CK_CHECK(f.client.clock() != nullptr);
    CK_CHECK(f.client.clock()->show_seconds());
    CK_CHECK(f.app.commands().execute(id_of(f.app, ckm::client::commands::kSettings)));
    f.app.step(0);

    // Both choices are on screen with their current values in them.
    std::string screen;
    const ckv::FrameView frame = f.app.current_frame();
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) screen += frame.at(ckv::Point{x, y}).grapheme();
    CK_CHECK(screen.find("Clock in the menu bar") != std::string::npos);
    CK_CHECK(screen.find("With seconds") != std::string::npos);
    CK_CHECK(screen.find("Colour theme") != std::string::npos);
    CK_CHECK(screen.find("Dark") != std::string::npos);

    // One entry down in each list: seconds → without seconds, Dark → Light.
    ckv::widgets::ComboBox* const clock_choice = choose_in_combo(f, "With seconds", 1);
    CK_CHECK(clock_choice != nullptr);
    CK_CHECK(clock_choice->text() == std::string("Without seconds"));
    ckv::widgets::ComboBox* const theme_choice = choose_in_combo(f, "Dark", 1);
    CK_CHECK(theme_choice != nullptr);
    CK_CHECK(theme_choice->text() == std::string("Light"));

    f.terminal.inject_bytes("\r", 0);  // Save
    f.app.step(0);
    f.app.step(0);

    // On screen at once...
    CK_CHECK(f.client.clock() != nullptr);
    CK_CHECK(!f.client.clock()->show_seconds());
    CK_CHECK(f.client.clock()->text() == std::string("09:41"));
    // ...and in the file for next time.
    const ckm::LoadedSettings stored = ckm::load_settings(config);
    CK_CHECK(stored.settings.clock == ckm::ClockMode::Minutes);
    CK_CHECK(stored.settings.theme == ckm::Theme::Light);
    CK_CHECK(stored.warnings.empty());

    (void)::unsetenv("CKMUX_CONFIG");
    std::filesystem::remove_all(config.parent_path());
}

CK_TEST(turning_the_clock_off_in_the_dialog_takes_it_off_the_bar) {
    const std::filesystem::path config =
        std::filesystem::temp_directory_path() / "ckmux-settings-clock-off" / "ckmux.conf";
    std::filesystem::remove_all(config.parent_path());
    (void)::setenv("CKMUX_CONFIG", config.c_str(), 1);

    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.commands().execute(id_of(f.app, ckm::client::commands::kSettings)));
    f.app.step(0);

    ckv::widgets::ComboBox* const clock_choice = choose_in_combo(f, "With seconds", 2);
    CK_CHECK(clock_choice != nullptr);
    CK_CHECK(clock_choice->text() == std::string("No clock"));
    f.terminal.inject_bytes("\r", 0);  // Save
    f.app.step(0);
    f.app.step(0);

    CK_CHECK(f.client.clock() == nullptr);
    CK_CHECK(menu_bar(f.client)->trailing_view() == nullptr);
    CK_CHECK(ckm::load_settings(config).settings.clock == ckm::ClockMode::Off);

    (void)::unsetenv("CKMUX_CONFIG");
    std::filesystem::remove_all(config.parent_path());
}

CK_TEST(leaving_a_session_takes_its_windows_down_and_leaves_the_programs_alone) {
    // Switching sessions, and being detached from one, are the same act on this
    // side: the windows belong to the session being left. What must NOT happen
    // is the close path — that ends the terminals, and the programs in a
    // session a reader has merely stopped watching go on running.
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 1U);
    f.client.open_terminal({});
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 2U);

    f.client.forget_terminals();
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().empty());
    // And off the screen, which is the half a reader can see. Gone from
    // `windows()` is not gone from the frame: a view that is detached but still
    // painted leaves a terminal a reader can look at, click on, and not use.
    {
        std::string screen;
        const ckv::FrameView frame = f.app.current_frame();
        for (int y = 0; y < frame.size().height; ++y)
            for (int x = 0; x < frame.size().width; ++x)
                screen += frame.at(ckv::Point{x, y}).grapheme();
        CK_CHECK(screen.find("Terminal 1") == std::string::npos);
    }

    // And the client is usable afterwards: a session attached next opens its
    // windows into the same desktop, numbered from the start.
    f.client.open_terminal({});
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 1U);
    CK_CHECK(f.client.desktop().windows().front()->title() == "Terminal 1");
}

CK_TEST(an_attached_close_asks_the_server_and_the_window_waits_for_it) {
    // The reworked close dialog's accept does not close the window: it asks
    // the server — force from the checkbox, the wait from the reader's
    // setting — and the window falls when the TermClosed comes back. The
    // hooks stand in for the server on both halves of that round trip.
    ClientOptions options = test_options();
    options.settings.kill_grace_seconds = 7;
    bool asked = false;
    bool asked_force = false;
    int asked_grace = 0;
    ckv::term::TerminalSubsession* asked_terminal = nullptr;
    options.close_terminal_in_session = [&](ckv::term::TerminalSubsession& terminal, bool force,
                                            int grace) {
        asked = true;
        asked_force = force;
        asked_grace = grace;
        asked_terminal = &terminal;
    };
    options.move_terminal = [](ckv::term::TerminalSubsession&, std::uint64_t, bool) {};
    ConfiguredFixture f(std::move(options));
    f.app.step(0);
    CK_CHECK(terminal_window_count(f.client) == 1U);

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kCloseTerminal)));
    f.app.step(0);
    CK_CHECK(f.app.is_modal());
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(asked);
    CK_CHECK(asked_force);       // the kill checkbox starts ticked
    CK_CHECK(asked_grace == 7);  // and the wait is the reader's setting, not a constant
    // The window is still here: only the server knows when the program ends.
    CK_CHECK(!f.app.is_modal());
    CK_CHECK(terminal_window_count(f.client) == 1U);
    // The server answers that the terminal ended; now the window goes.
    CK_CHECK(asked_terminal != nullptr);
    if (asked_terminal != nullptr) f.client.close_window_for_terminal(*asked_terminal);
    f.app.step(0);
    CK_CHECK(terminal_window_count(f.client) == 0U);
}

CK_TEST(the_move_picker_offers_the_others_and_always_a_new_session) {
    ClientOptions options = test_options();
    std::uint64_t moved_to = 99;
    bool moved_new = false;
    int moves = 0;
    options.move_terminal = [&](ckv::term::TerminalSubsession&, std::uint64_t destination,
                                bool to_new_session) {
        ++moves;
        moved_to = destination;
        moved_new = to_new_session;
    };
    ConfiguredFixture f(std::move(options));
    f.app.step(0);
    f.client.set_attached_session(1, "one");
    f.client.remember_sessions({{1, "one", 1, true}, {7, "build", 2, false}});
    f.app.step(0);

    // ^B . — the same dialog the close dialog's Move button reaches. The
    // first row is the one OTHER session; the watched one is not a
    // destination.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kMoveTerminal)));
    f.app.step(0);
    CK_CHECK(f.app.is_modal());
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(moves == 1);
    CK_CHECK(moved_to == 7U);
    CK_CHECK(!moved_new);

    // Asked again, the reader arrows past it to "a new session" — the row
    // that is always there, which is what keeps the picker workable when
    // nothing else is running at all.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kMoveTerminal)));
    f.app.step(0);
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Down, ckv::Modifier::None, ""}});
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(moves == 2);
    CK_CHECK(moved_new);
}

CK_TEST(the_close_dialogs_move_button_swaps_to_the_move_picker) {
    ClientOptions options = test_options();
    int closes = 0;
    options.close_terminal_in_session = [&](ckv::term::TerminalSubsession&, bool, int) {
        ++closes;
    };
    bool move_offered = false;
    options.move_terminal = [&](ckv::term::TerminalSubsession&, std::uint64_t destination,
                                bool to_new_session) {
        move_offered = true;
        CK_CHECK(destination == 0U);
        CK_CHECK(to_new_session);
    };
    ConfiguredFixture f(std::move(options));
    f.app.step(0);
    f.client.set_attached_session(1, "one");
    f.client.remember_sessions({{1, "one", 1, true}});
    f.app.step(0);

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kCloseTerminal)));
    f.app.step(0);
    CK_CHECK(f.app.is_modal());
    // Alt+M: the Move button dismisses the close question and opens the
    // picker in its place. With no other session running, its one row is "a
    // new session" — the button never leads anywhere dead.
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Alt, "m"}});
    f.app.step(0);
    f.app.step(0);  // the picker opens from a posted callback
    CK_CHECK(f.app.is_modal());
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(move_offered);
    CK_CHECK(closes == 0);  // nothing was closed that the reader did not choose
    CK_CHECK(terminal_window_count(f.client) == 1U);
}

CK_TEST(a_refusal_from_the_server_is_shown_with_its_code_and_its_request) {
    // The reader's end of "a request is always answered" (plans/05). The server
    // has always sent an `Error` for a request it could not honour; what was
    // missing was anybody to put it in front of the person who asked, so a
    // reader who tried something impossible watched a ckmux that looked hung.
    //
    // The code and the context are on screen as well as the sentence, because
    // those two are what a bug report is written from.
    Fixture f;
    f.app.step(0);
    f.client.show_server_error(
        static_cast<std::uint16_t>(ckm::proto::ErrorCode::NameTaken), "RenameSession",
        "a session called 'work' already exists");
    f.app.step(0);
    f.app.step(0);  // a dialog may be presented from a posted callback

    std::string screen;
    const ckv::FrameView frame = f.app.current_frame();
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) screen += frame.at(ckv::Point{x, y}).grapheme();
    CK_CHECK(screen.find("already exists") != std::string::npos);
    CK_CHECK(screen.find("RenameSession") != std::string::npos);
    CK_CHECK(screen.find("Code: 4") != std::string::npos);  // NameTaken, by number
}

CK_TEST(a_window_being_dragged_is_reported_once_it_settles_and_not_per_frame) {
    // WP-29's whole point. A drag produces a geometry change per frame, and a
    // client that reported each one would put a message on the wire per frame —
    // the cost WP-7 measured on the delta path, which is why the server's own
    // producer coalesces to its tick and why this one coalesces to a settle.
    LayoutReports reports;
    ConfiguredFixture f(watching_layout(reports));
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];

    // The desktop as it opens. One sample says nothing — there is nothing yet
    // to compare it with — and the second, agreeing with the first, is the
    // first arrangement this client has ever had to report.
    layout_tick(f);
    CK_CHECK(reports.empty());
    layout_tick(f);
    CK_CHECK(reports.size() == 1U);
    CK_CHECK(reports.back().size() == 1U);
    CK_CHECK(reports.back().front().terminal == terminal_of(window));
    CK_CHECK(reports.back().front().rect == window->bounds());
    CK_CHECK(reports.back().front().z_order == 0U);
    CK_CHECK(!reports.back().front().zoomed);

    // The drag: a position per turn of the timer, every one of them a position
    // the window does not end up in.
    for (int frame = 1; frame <= 4; ++frame) {
        window->set_bounds(ckv::Rect{10 + frame, 4 + frame, 30, 10});
        layout_tick(f);
    }
    CK_CHECK(reports.size() == 1U);

    // The reader lets go, and exactly one report describes where the window
    // actually is.
    layout_tick(f);
    CK_CHECK(reports.size() == 2U);
    CK_CHECK(reports.back().size() == 1U);
    CK_CHECK(reports.back().front().rect == (ckv::Rect{14, 8, 30, 10}));

    // And then nothing, for as long as nobody touches anything. Edge-triggered
    // like the server's own announce: a still desktop is not news.
    for (int idle = 0; idle < 6; ++idle) layout_tick(f);
    CK_CHECK(reports.size() == 2U);
}

CK_TEST(a_raise_renumbers_the_whole_arrangement_and_a_close_leaves_it) {
    // Why a layout is reported as a whole arrangement rather than one window at
    // a time. Raising one window renumbers the ones it passed — nobody moved
    // them and their stored z-order is wrong now — and a window that CLOSES is
    // named nowhere at all, so only "here is everything" can state either.
    LayoutReports reports;
    ConfiguredFixture f(watching_layout(reports));
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    CK_CHECK(terminal_window_count(f.client) == 2U);
    const ckv::core::TerminalSubsession* const first = terminal_of(f.client.desktop().windows()[0]);
    const ckv::core::TerminalSubsession* const second = terminal_of(f.client.desktop().windows()[1]);

    layout_tick(f);
    layout_tick(f);
    CK_CHECK(reports.size() == 1U);
    CK_CHECK(reports.back().size() == 2U);
    // Bottom of the stack first, and the window just opened is on top of the
    // one that was there.
    CK_CHECK(reports.back()[0].terminal == first);
    CK_CHECK(reports.back()[0].z_order == 0U);
    CK_CHECK(reports.back()[1].terminal == second);
    CK_CHECK(reports.back()[1].z_order == 1U);

    // A raise, which moves no window by a single cell.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kPreviousWindow)));
    layout_tick(f);
    layout_tick(f);
    CK_CHECK(reports.size() == 2U);
    CK_CHECK(reports.back().size() == 2U);
    CK_CHECK(reports.back()[0].terminal == second);
    CK_CHECK(reports.back()[1].terminal == first);

    // And a close, which is a window leaving the arrangement rather than
    // anything being said about it. Taken down the way an attached client's
    // window goes — the server said the terminal ended — so no confirmation
    // dialog stands between this and the arrangement that is left.
    const ckv::term::TerminalSubsession* const closing =
        launched_terminal_of(f.client.desktop().windows()[1]);
    CK_CHECK(closing != nullptr);
    if (closing == nullptr) return;
    f.client.close_window_for_terminal(*closing);
    f.app.step(0);
    CK_CHECK(terminal_window_count(f.client) == 1U);
    layout_tick(f);
    layout_tick(f);
    CK_CHECK(reports.size() == 3U);
    CK_CHECK(reports.back().size() == 1U);
    CK_CHECK(reports.back().front().terminal == first);
    CK_CHECK(reports.back().front().z_order == 0U);
}

CK_TEST(a_zoomed_window_is_reported_as_zoomed_and_unzooming_reports_it_back) {
    // Maximized is part of a window's place, not a separate fact about it: a
    // terminal that was zoomed is restored zoomed (WP-30), and the rect beside
    // the flag is the one it fills right now.
    LayoutReports reports;
    ConfiguredFixture f(watching_layout(reports));
    f.app.step(0);
    layout_tick(f);
    layout_tick(f);
    CK_CHECK(reports.size() == 1U);
    CK_CHECK(!reports.back().front().zoomed);

    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kZoom)));
    f.app.step(0);
    layout_tick(f);
    layout_tick(f);
    CK_CHECK(reports.size() == 2U);
    CK_CHECK(reports.back().front().zoomed);
    CK_CHECK(reports.back().front().rect == f.client.desktop().content_area());

    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kZoom)));
    f.app.step(0);
    layout_tick(f);
    layout_tick(f);
    CK_CHECK(reports.size() == 3U);
    CK_CHECK(!reports.back().front().zoomed);
}
