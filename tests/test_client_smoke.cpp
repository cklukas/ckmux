// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// End-to-end coverage for the ckmux client, driven headlessly exactly the way
// main.cpp drives it: real Application, real render pipeline, real menu and
// mouse paths. The child program is /bin/cat rather than a shell — it stays
// alive, says nothing, and therefore keeps these assertions about the UI
// rather than about a prompt.
#if !defined(_WIN32)

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "client/client_app.hpp"
#include "cvision/core/text.hpp"
#include "cvision/term/headless_terminal.hpp"
#include <cstdlib>
#include <filesystem>

#include "common/config.hpp"
#include "common/proto.hpp"
#include "common/version.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/ui/standard_roles.hpp"
#include "cvision/widgets/common_components.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/terminal_view.hpp"
#include "cvision/widgets/window_switcher_bar.hpp"

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

// --- WP-32: the window switcher bar ----------------------------------------

// Which window is frontmost. `children()` holds the docked chrome and any
// popup too, so the answer is the last child that is a window — the same thing
// ckVision's own removal path looks for.
ckv::widgets::Window* topmost_window(ClientApp& client) {
    const auto& children = client.desktop().children();
    for (auto it = children.rbegin(); it != children.rend(); ++it)
        if (auto* const window = dynamic_cast<ckv::widgets::Window*>(it->get())) return window;
    return nullptr;
}

// The dropdown a right press on the bar left standing, if any. A context menu
// is a desktop popup like the calendar is.
ckv::widgets::DropdownMenu* dropped_menu(ClientApp& client) {
    for (ckv::ui::View* popup : client.desktop().popups())
        if (auto* const menu = dynamic_cast<ckv::widgets::DropdownMenu*>(popup)) return menu;
    return nullptr;
}

// The ABSOLUTE cell in the middle of one switcher entry — where a reader's
// pointer would be. `drawn_entries()` answers in the bar's own local columns
// and a mouse report carries absolute ones, so the bar's own placement is what
// converts between them rather than an assumption about where it was docked.
std::optional<ckv::Point> switcher_cell(ClientApp& client, std::size_t index) {
    const ckv::Rect where = client.window_switcher().absolute_bounds();
    for (const ckv::widgets::WindowSwitcherBar::DrawnEntry& drawn :
         client.window_switcher().drawn_entries())
        if (drawn.index == index)
            return ckv::Point{where.x + drawn.x + drawn.width / 2, where.y};
    return std::nullopt;
}

// A real click on one entry: press and release on its own cells, dispatched
// through the application exactly as a host terminal's mouse report arrives.
// Returns whether there was such an entry to click at all.
template <typename FixtureType>
bool switcher_click(FixtureType& f, std::size_t index) {
    const std::optional<ckv::Point> cell = switcher_cell(f.client, index);
    if (!cell) return false;
    (void)f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Left, *cell,
                                         std::nullopt, ckv::Modifier::None});
    (void)f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Up, ckv::MouseButton::Left, *cell,
                                         std::nullopt, ckv::Modifier::None});
    f.app.step(0);
    return true;
}

// And a right press, which is the whole gesture: the menu opens on the press.
template <typename FixtureType>
bool switcher_right_press(FixtureType& f, std::size_t index) {
    const std::optional<ckv::Point> cell = switcher_cell(f.client, index);
    if (!cell) return false;
    (void)f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Right, *cell,
                                         std::nullopt, ckv::Modifier::None});
    f.app.step(0);
    return true;
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

// --- WP-30: the arrangement the server states back --------------------------

// One placement as the server would state it, composed the way
// `run_client.cpp` composes it off the wire: a terminal is named by the
// subsession its window is showing, because the window layer has no wire ids.
ckm::client::WindowPlacement stored(ckv::widgets::Window* window, ckv::Rect rect,
                                    std::uint16_t z_order = 0, bool zoomed = false,
                                    ckm::client::TileShare tile = {}) {
    return ckm::client::WindowPlacement{launched_terminal_of(window), rect, z_order, zoomed, tile};
}

// And the reverse, which the client keeps to itself: the window a terminal is
// being shown in. Found by asking each window what it shows rather than by
// index, because a restore reorders the stack and an index would then name a
// different window than the one the assertion is about.
ckv::widgets::Window* showing(ClientApp& client, const ckv::term::TerminalSubsession* terminal) {
    if (terminal == nullptr) return nullptr;
    for (ckv::widgets::Window* window : client.desktop().windows())
        if (launched_terminal_of(window) == terminal) return window;
    return nullptr;
}

}  // namespace

CK_TEST(the_client_opens_with_a_menu_bar_a_footer_and_one_terminal) {
    Fixture f;
    f.app.step(0);

    ckv::widgets::MenuBar* const bar = menu_bar(f.client);
    CK_CHECK(bar != nullptr);
    // Session, Terminal, View, Window, Settings, Help. View arrived with
    // WP-39 and holds the three readout toggles — live display switches, not
    // settings dialogs; the colour theme stays a stored setting in
    // Settings ▸ General…, which is why it is still not here.
    CK_CHECK(bar->menus().size() == 6U);
    // The footer is no longer the bottom dock on its own: the window switcher
    // bar sits on top of it (WP-32), and a Desktop holds exactly one docked
    // view per edge — so the two rows are composed into a Column and THAT is
    // what is docked. Both are still there, and both are still chrome.
    CK_CHECK(f.client.desktop().bottom_dock() != nullptr);
    CK_CHECK(f.client.footer().parent() == f.client.desktop().bottom_dock());
    CK_CHECK(f.client.window_switcher().parent() == f.client.desktop().bottom_dock());
    // With one terminal there is nothing to switch between, so the row is not
    // on screen and the desktop keeps the line.
    CK_CHECK(!f.client.window_switcher().visible());
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
    // View (WP-39): the three readout toggles, checkable, and nothing else —
    // in particular no theme items, which stay a stored setting.
    CK_CHECK(menu_has_command(bar->menus()[2], id_of(f.app, ckm::client::commands::kShowCpuUsage)));
    CK_CHECK(
        menu_has_command(bar->menus()[2], id_of(f.app, ckm::client::commands::kShowMemoryRss)));
    CK_CHECK(
        menu_has_command(bar->menus()[2], id_of(f.app, ckm::client::commands::kShowMemoryReal)));
    CK_CHECK(bar->menus()[2].items.size() == 3U);
    CK_CHECK(menu_has_command(bar->menus()[3], id_of(f.app, ckv::ui::std_command_keys::kWindowList)));
    // The three tilings by name (WP-31), and NOT the library's unqualified
    // Tile: it produces exactly what Tile Vertically produces, so a menu
    // carrying both would offer one behaviour twice under two names.
    CK_CHECK(menu_has_command(bar->menus()[3],
                              id_of(f.app, ckv::ui::std_command_keys::kTileHorizontally)));
    CK_CHECK(menu_has_command(bar->menus()[3],
                              id_of(f.app, ckv::ui::std_command_keys::kTileVertically)));
    CK_CHECK(menu_has_command(bar->menus()[3], id_of(f.app, ckv::ui::std_command_keys::kTileGrid)));
    CK_CHECK(!menu_has_command(bar->menus()[3], id_of(f.app, ckv::ui::std_command_keys::kTile)));
    CK_CHECK(menu_has_command(bar->menus()[3], id_of(f.app, ckv::ui::std_command_keys::kCascade)));
    CK_CHECK(menu_has_command(bar->menus()[4], id_of(f.app, ckm::client::commands::kSettings)));
    CK_CHECK(menu_has_command(bar->menus()[5], id_of(f.app, ckm::client::commands::kAbout)));
}

CK_TEST(the_about_box_names_the_program_its_version_and_who_it_belongs_to) {
    // An About box exists to answer "what is this and whose is it", and the
    // second half of that question had no answer at all until WP-35. The
    // attribution is worded exactly as CK Office words its own (see
    // cworks::about_lines) — one hand, one sentence — so a renaming that
    // quietly drops or reshapes it fails here rather than shipping.
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kAbout)));
    f.app.step(0);

    std::string screen;
    const ckv::FrameView frame = f.app.current_frame();
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) screen += frame.at(ckv::Point{x, y}).grapheme();
    // Drawn, not merely composed. Rows are joined end to end, so a line that
    // survives this search is a line that landed whole on one row — which is
    // the claim, since a copyright broken across a wrap is not one a reader
    // can quote back.
    CK_CHECK(screen.find("(c) 2026 by Dr. Christian Klukas") != std::string::npos);
    // The identity it is an attribution FOR, in the same box: an author line
    // over an unnamed program would pass the check above and say nothing.
    CK_CHECK(screen.find("ckmux " CKMUX_VERSION_STRING) != std::string::npos);
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

    // Vertical bands: equal widths, and each band the full content height.
    // This is the arrangement ckmux reached through the unqualified `Tile`
    // until WP-31 named it, and it is unchanged — only what a reader calls it
    // and which chord reaches it changed.
    const ckv::Rect area = f.client.desktop().content_area();
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kTileHorizontally)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows()[0]->bounds().width ==
             f.client.desktop().windows()[1]->bounds().width);
    CK_CHECK(f.client.desktop().windows()[0]->bounds().height == area.height);
    // Vertical bands are its transpose: full width, stacked top to bottom.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kTileVertically)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows()[0]->bounds().width == area.width);
    CK_CHECK(f.client.desktop().windows()[0]->bounds().y <
             f.client.desktop().windows()[1]->bounds().y);
    // And the grid, which for two windows is one row of two columns.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kTileGrid)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows()[0]->bounds().height == area.height);
    CK_CHECK(f.client.desktop().windows()[0]->bounds().x <
             f.client.desktop().windows()[1]->bounds().x);

    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kCascade)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows()[0]->bounds().x < f.client.desktop().windows()[1]->bounds().x);

    // Zoom fills the desktop's content area — never the docked chrome.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kZoom)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().active_window()->zoomed());
    CK_CHECK(f.client.desktop().active_window()->bounds() == f.client.desktop().content_area());
}

CK_TEST(a_terminal_opened_while_another_is_maximized_opens_maximized_too) {
    // WP-33 (U4-c enabled for ckmux's desktop). A reader working full-screen
    // who opens a second terminal used to get a small window cascaded on top
    // of the big one — the arrangement they had just said they did not want.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kZoom)));
    f.app.step(0);
    CK_CHECK(first->zoomed());

    f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal));
    f.app.step(0);
    ckv::widgets::Window* const second = f.client.desktop().active_window();
    CK_CHECK(second != nullptr && second != first);
    if (second == nullptr) return;
    CK_CHECK(second->zoomed());
    CK_CHECK(second->bounds() == f.client.desktop().content_area());
    // The one the reader was looking at is left as it was: this places the new
    // window, it does not rearrange the desktop.
    CK_CHECK(first->zoomed());

    // And the zoom control on its frame restores it to a real rectangle. A
    // window maximized straight from an unplaced 0x0 would record 0x0 as the
    // bounds to come back to, and the reader's first click would make it
    // vanish rather than shrink.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kZoom)));
    f.app.step(0);
    CK_CHECK(!second->zoomed());
    CK_CHECK(second->bounds().width > 0 && second->bounds().height > 0);
    CK_CHECK(second->bounds() != f.client.desktop().content_area());
}

CK_TEST(with_nothing_maximized_a_new_terminal_cascades_exactly_as_it_always_did) {
    // The other half of the same claim: maximize-follows reads the state the
    // reader put the desktop in, so with no window maximized nothing about
    // placement changes at all.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    CK_CHECK(!first->zoomed());
    const ckv::Rect first_bounds = first->bounds();

    f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal));
    f.app.step(0);
    ckv::widgets::Window* const second = f.client.desktop().active_window();
    CK_CHECK(second != nullptr && second != first);
    if (second == nullptr) return;
    CK_CHECK(!second->zoomed());
    CK_CHECK(second->bounds() != f.client.desktop().content_area());
    // Cascaded down and to the right of the one before it, which is what
    // stops a second terminal landing exactly on the first.
    CK_CHECK(second->bounds().x > first_bounds.x);
    CK_CHECK(second->bounds().y > first_bounds.y);
    CK_CHECK(first->bounds() == first_bounds);
}

// --- WP-32: the window switcher bar -----------------------------------------

CK_TEST(the_switcher_bar_lists_every_terminal_by_its_real_caption) {
    Fixture f;
    f.app.step(0);
    // One terminal is nothing to switch BETWEEN, so the row is not there and
    // the desktop keeps the line.
    CK_CHECK(!f.client.window_switcher().visible());

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    CK_CHECK(f.client.window_switcher().visible());
    CK_CHECK(f.client.window_switcher().entries().size() == 2U);
    if (f.client.window_switcher().entries().size() != 2U) return;
    CK_CHECK(f.client.window_switcher().entries()[0].window == f.client.desktop().windows()[0]);
    // ckmux's own numbering, read back off the window rather than re-derived —
    // the caption the reader is looking at IS the label.
    CK_CHECK(f.client.window_switcher().entries()[0].label == "Terminal 1");
    CK_CHECK(f.client.window_switcher().entries()[1].label == "Terminal 2");
    // Exactly one row says which terminal the reader is in, and it is the one
    // that just opened.
    CK_CHECK(!f.client.window_switcher().entries()[0].active);
    CK_CHECK(f.client.window_switcher().entries()[1].active);

    // A third, so the list is a list rather than a pair.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    CK_CHECK(f.client.window_switcher().entries().size() == 3U);
    CK_CHECK(f.client.window_switcher().entries()[2].label == "Terminal 3");

    // A terminal ending — the way an attached client's window goes, the server
    // having said so — takes its row with it, and the ones either side close up.
    const ckv::term::TerminalSubsession* const closing =
        launched_terminal_of(f.client.desktop().windows()[1]);
    CK_CHECK(closing != nullptr);
    if (closing == nullptr) return;
    f.client.close_window_for_terminal(*closing);
    f.app.step(0);
    CK_CHECK(f.client.window_switcher().entries().size() == 2U);
    CK_CHECK(f.client.window_switcher().entries()[0].label == "Terminal 1");
    CK_CHECK(f.client.window_switcher().entries()[1].label == "Terminal 3");

    // And a rename reaches it. The caption follows the child (OSC 2) and the row
    // follows the caption — through the desktop's title notification, not
    // through a sweep of the window set that the bar would otherwise have to do
    // on every frame.
    auto* const renamed_view =
        dynamic_cast<ckv::widgets::TerminalView*>(f.client.desktop().windows()[1]->content());
    CK_CHECK(renamed_view != nullptr);
    if (renamed_view == nullptr) return;
    renamed_view->session().feed_output("\x1b]2;vim: plans/06\a");
    f.clock.advance(200'000'000);
    f.app.step(f.clock.now_nanos());
    CK_CHECK(f.client.desktop().windows()[1]->title() == "vim: plans/06");
    CK_CHECK(f.client.window_switcher().entries()[1].label == "vim: plans/06");
}

CK_TEST(a_maximized_terminal_leaves_the_switcher_bars_row_uncovered) {
    // U4-a's own done-when, in ckmux's chrome and with the numbers written out.
    // 80x24 is a real size to run a multiplexer in: one row of menu bar, one of
    // switcher bar, one of footer, and what is left is the desktop.
    Fixture f;
    f.terminal.resize(Size{80, 24});
    f.app.step(0);
    // One terminal, no bar: the 22 rows the interface spec promises, which is the budget
    // Settings ▸ General… is drawn to fit.
    CK_CHECK((f.client.desktop().content_area() == ckv::Rect{0, 1, 80, 22}));

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    CK_CHECK(f.client.window_switcher().visible());
    // The Column's vertical hint sums its children, so composing the two rows
    // IS the reservation — no second dock slot, and no arithmetic of ckmux's
    // own to keep in step with it.
    CK_CHECK((f.client.desktop().content_area() == ckv::Rect{0, 1, 80, 21}));
    CK_CHECK((f.client.window_switcher().absolute_bounds() == ckv::Rect{0, 22, 80, 1}));
    CK_CHECK((f.client.footer().absolute_bounds() == ckv::Rect{0, 23, 80, 1}));

    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kZoom)));
    f.app.step(0);
    ckv::widgets::Window* const zoomed = f.client.desktop().active_window();
    CK_CHECK(zoomed != nullptr);
    if (zoomed == nullptr) return;
    CK_CHECK(zoomed->zoomed());
    CK_CHECK((zoomed->bounds() == ckv::Rect{0, 1, 80, 21}));
    // The whole point of the arrangement: the maximized terminal stops one row
    // short of the bar, so the bar is still readable and still clickable.
    CK_CHECK(zoomed->bounds().bottom() == f.client.window_switcher().absolute_bounds().y);

    // And dropping back to one terminal gives the row back to the desktop — the
    // maximized window grows into it rather than leaving a blank line behind.
    const ckv::term::TerminalSubsession* const closing =
        launched_terminal_of(f.client.desktop().windows()[0]);
    CK_CHECK(closing != nullptr);
    if (closing == nullptr) return;
    f.client.close_window_for_terminal(*closing);
    f.app.step(0);
    CK_CHECK(!f.client.window_switcher().visible());
    CK_CHECK((f.client.desktop().content_area() == ckv::Rect{0, 1, 80, 22}));
    CK_CHECK((zoomed->bounds() == ckv::Rect{0, 1, 80, 22}));
}

CK_TEST(clicking_a_row_on_the_switcher_bar_brings_that_terminal_forward) {
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];
    CK_CHECK(f.client.desktop().active_window() == second);

    CK_CHECK(switcher_click(f, 0));
    CK_CHECK(f.client.desktop().active_window() == first);
    // Raised, not merely marked active.
    CK_CHECK(topmost_window(f.client) == first);
    // And the keyboard went with it, which is ckmux's own addition to what
    // "activate" means: a reader who picks a terminal off the bar means to type
    // into it, and a raise that left the keys behind would ignore them.
    CK_CHECK(f.app.focused() == first->content());
    CK_CHECK(f.client.context() == Context::Terminal);
    CK_CHECK(f.client.window_switcher().entries()[0].active);
    CK_CHECK(!f.client.window_switcher().entries()[1].active);

    // And back, so this is the bar acting rather than the one click happening
    // to agree with where activation already was.
    CK_CHECK(switcher_click(f, 1));
    CK_CHECK(f.client.desktop().active_window() == second);
    CK_CHECK(topmost_window(f.client) == second);
    CK_CHECK(f.app.focused() == second->content());
}

CK_TEST(a_right_click_on_the_switcher_bar_acts_on_that_window_not_the_active_one) {
    // The correctness point U4-a exists for. Command dispatch has no target:
    // every standard window handler reads active_window(), so a Maximize chosen
    // from a BACKGROUND row would maximize the foreground terminal — and Close
    // would close it. Every row here is bound to the window whose row it is.
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];
    CK_CHECK(f.client.desktop().active_window() == second);

    CK_CHECK(switcher_right_press(f, 0));
    // Right-clicking a background row does not take the reader to it.
    CK_CHECK(f.client.desktop().active_window() == second);
    ckv::widgets::DropdownMenu* const menu = dropped_menu(f.client);
    CK_CHECK(menu != nullptr);
    if (menu == nullptr) return;
    // What a reader is offered, in the order they are offered it — the taskbar
    // vocabulary the request asked for.
    CK_CHECK(menu->items().size() == 9U);
    if (menu->items().size() != 9U) return;
    // Minimize above Maximize, the order the two controls sit in on the
    // window's own frame (WP-34).
    CK_CHECK(menu->items()[0].label() == "Mi&nimize");
    CK_CHECK(menu->items()[1].label() == "Ma&ximize");
    CK_CHECK(menu->items()[2].label() == "&Move / Resize");
    CK_CHECK(menu->items()[3].is_separator());
    // Naming the window sits in a group of its own between what happens to the
    // window on this desktop and what happens to the terminal behind it.
    CK_CHECK(menu->items()[4].label() == "Re&name…");
    CK_CHECK(menu->items()[5].is_separator());
    CK_CHECK(menu->items()[6].label() == "Move to &session…");
    CK_CHECK(menu->items()[7].is_separator());
    CK_CHECK(menu->items()[8].label() == "&Close");
    // With no server there is nowhere for a program to be moved TO, so that row
    // is plainly unavailable rather than quietly inert — the same rule
    // `Terminal ▸ Move terminal…` follows. Rename is not gated with it: a
    // reader may name a window whether or not there is anywhere to move it.
    CK_CHECK(!menu->items()[6].enabled_flag());
    CK_CHECK(!menu->items()[6].disabled_reason().empty());
    // Named as well as indexed. This assertion was left pointing at index 3
    // when Minimize shifted every row down one, and index 3 is a SEPARATOR —
    // which is enabled, so it went on passing while testing nothing about
    // Rename at all. An index alone cannot notice that it has stopped being
    // about what its comment says.
    CK_CHECK(menu->items()[4].label() == "Re&name…");
    CK_CHECK(menu->items()[4].enabled_flag());

    // Maximize, chosen from the BACKGROUND row. Command dispatch would have
    // maximized `second`, which is the window the reader did not point at.
    menu->items()[1].action()();
    f.app.step(0);
    CK_CHECK(first->zoomed());
    CK_CHECK(!second->zoomed());
    CK_CHECK(first->bounds() == f.client.desktop().content_area());
}

CK_TEST(the_switcher_menu_reads_the_window_its_row_belongs_to_not_the_one_in_front) {
    // The other half of the same seam, on the wording rather than the effect: a
    // row's first entry says Restore or Maximize according to THAT window's own
    // state. Reading the active window would offer to maximize a terminal that
    // already fills the desktop.
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];

    // Maximize the FIRST terminal and then go back to the second, which leaves
    // a maximized window in the background and an ordinary one in front.
    CK_CHECK(switcher_click(f, 0));
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kZoom)));
    f.app.step(0);
    CK_CHECK(switcher_click(f, 1));
    CK_CHECK(f.client.desktop().active_window() == second);
    CK_CHECK(first->zoomed());
    CK_CHECK(!second->zoomed());

    CK_CHECK(switcher_right_press(f, 0));
    ckv::widgets::DropdownMenu* const menu = dropped_menu(f.client);
    CK_CHECK(menu != nullptr);
    // Checked, not merely guarded on. This bail-out named 6 items long after
    // the menu had grown to 8, so the whole of what follows it was skipped and
    // the test passed by not running — a guard that is not also an assertion
    // is a test that goes quiet the moment the thing it guards on moves.
    CK_CHECK(menu->items().size() == 9U);
    if (menu == nullptr || menu->items().size() != 9U) return;
    CK_CHECK(menu->items()[1].label() == "&Restore");
    menu->items()[1].action()();
    f.app.step(0);
    CK_CHECK(!first->zoomed());
    CK_CHECK(!second->zoomed());
}

CK_TEST(closing_from_the_switcher_bar_ends_the_terminal_whose_row_was_clicked) {
    // The destructive one, which is why `WindowSwitcherTarget::bind` exists at
    // all: wired as a command, this row would have closed the foreground
    // terminal — silently, and with a program running in it.
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];
    CK_CHECK(f.client.desktop().active_window() == second);

    CK_CHECK(switcher_right_press(f, 0));
    ckv::widgets::DropdownMenu* const menu = dropped_menu(f.client);
    CK_CHECK(menu != nullptr);
    // Asserted rather than silently skipped — see the note in the test above.
    CK_CHECK(menu->items().size() == 9U);
    if (menu == nullptr || menu->items().size() != 9U) return;
    CK_CHECK(menu->items()[8].label() == "&Close");

    // `close()`, so the window's own veto still runs: /bin/cat is alive in it,
    // and the reader is asked — about the terminal they pointed at.
    menu->items()[8].action()();
    f.app.step(0);
    CK_CHECK(f.app.is_modal());
    CK_CHECK(terminal_window_count(f.client) == 2U);
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(terminal_window_count(f.client) == 1U);
    // The one that went is the one whose row was clicked; the terminal the
    // reader was actually working in is untouched, and the row nobody needs any
    // more is gone with it.
    CK_CHECK(f.client.window_switcher().entries().size() == 1U);
    if (f.client.window_switcher().entries().size() != 1U) return;
    CK_CHECK(f.client.window_switcher().entries()[0].window == second);
    CK_CHECK(!f.client.window_switcher().visible());
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
    // The reader's end of "a request is always answered" (the protocol spec). The server
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

CK_TEST(a_window_left_in_the_bottom_right_reattaches_shifted_up_and_left_at_its_own_size) {
    // WP-30's move rule, and the case it was written for: a reader arranges a
    // window near the bottom-right of a large terminal, comes back on a smaller
    // one, and finds it fully visible — moved, and at exactly the size they
    // made it. The checkbox is off, which is the default.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    const ckv::Rect area = f.client.desktop().content_area();
    CK_CHECK(!f.client.settings().resize_windows_to_fit);

    // Where the server says the reader left it: 20 columns past the right edge
    // of the desktop they have now, and 5 rows past the bottom.
    const ckv::Rect where_they_left_it{area.x + area.width - 10, area.y + area.height - 5, 30, 10};
    f.client.apply_layout({stored(window, where_they_left_it)});

    // Shifted up and left by exactly the overhang — no further, which is what
    // "only as far as needed" means — and not resized by a single cell.
    CK_CHECK(window->bounds() ==
             (ckv::Rect{area.x + area.width - 30, area.y + area.height - 10, 30, 10}));
    CK_CHECK(window->bounds().right() == area.right());
    CK_CHECK(window->bounds().bottom() == area.bottom());

    // And a window that was already inside is not touched at all: the rule can
    // lower a coordinate and never raise one, so a window that fits has nothing
    // done to it.
    Fixture fits;
    fits.app.step(0);
    ckv::widgets::Window* const inside = fits.client.desktop().windows()[0];
    const ckv::Rect room = fits.client.desktop().content_area();
    const ckv::Rect comfortable{room.x + 3, room.y + 2, 20, 6};
    fits.client.apply_layout({stored(inside, comfortable)});
    CK_CHECK(inside->bounds() == comfortable);
}

CK_TEST(a_window_still_too_large_after_the_move_is_resized_only_when_the_reader_asked) {
    // The second step, and it is a distinct one. The move above always happens;
    // this decides whether anything follows it — and it is off by default,
    // because a window too big for today's terminal is still the size its
    // reader made it, and coming back on the bigger one restores it exactly.
    // Shrinking it would not be undone by anything.
    const ckv::Rect too_big{4, 4, 140, 40};  // wider and taller than either desktop below

    {
        ConfiguredFixture off(test_options());
        off.app.step(0);
        ckv::widgets::Window* const window = off.client.desktop().windows()[0];
        const ckv::Rect area = off.client.desktop().content_area();
        off.client.apply_layout({stored(window, too_big)});
        // Moved as far as it goes — the content area's own corner, since there
        // is nowhere further — and left at its real size, still hanging off
        // both edges. That overhang is the honest answer, not a bug.
        CK_CHECK(window->bounds() == (ckv::Rect{area.x, area.y, too_big.width, too_big.height}));
    }

    ClientOptions asked = test_options();
    asked.settings.resize_windows_to_fit = true;
    ConfiguredFixture on(asked);
    on.app.step(0);
    ckv::widgets::Window* const window = on.client.desktop().windows()[0];
    const ckv::Rect area = on.client.desktop().content_area();
    on.client.apply_layout({stored(window, too_big)});
    // Moved FIRST and then shrunk: the origin is the corner the move reached,
    // and the size is the largest that fits there. A resize instead of a move
    // would have left it at 4,4 and taken more off the size than it had to.
    CK_CHECK(window->bounds() == area);
}

CK_TEST(an_arrangement_laid_down_under_a_dialog_leaves_the_dialog_in_front) {
    // The regression this pins cost three end-to-end menu tests. A layout
    // statement arriving while the reader had a dialog open raised a terminal
    // window over it and took the keyboard with it — so the modal asking
    // whether to close a terminal ended up behind that very terminal, and the
    // keystrokes opening a menu went to a shell instead.
    //
    // Geometry is safe to lay down whenever: a window that moves under a dialog
    // is still under it. The STACK and the FOCUS are not, because a dialog is
    // above the arrangement whatever the arrangement covers.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    const ckv::Rect area = f.client.desktop().content_area();

    CK_CHECK(f.app.commands().execute(id_of(f.app, ckm::client::commands::kSettings)));
    f.app.step(0);
    CK_CHECK(f.app.is_modal());
    ckv::ui::View* const answering = f.app.focused();

    const ckv::Rect somewhere{area.x + 5, area.y + 3, 24, 8};
    f.client.apply_layout({stored(window, somewhere)});
    f.app.step(0);

    // The window went where the arrangement said — that half is never in doubt.
    CK_CHECK(window->bounds() == somewhere);
    // And the dialog is still in front, still holding the keys, still readable.
    CK_CHECK(f.app.is_modal());
    CK_CHECK(f.app.focused() == answering);
    std::string screen;
    const ckv::FrameView frame = f.app.current_frame();
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) screen += frame.at(ckv::Point{x, y}).grapheme();
    CK_CHECK(screen.find("Start terminals") != std::string::npos);
    CK_CHECK(screen.find("Save") != std::string::npos);
}

CK_TEST(a_maximized_window_and_a_fifty_fifty_pair_come_back_proportioned_on_a_new_desktop) {
    // The proportional half of the policy, end to end: ckVision's own tile
    // query fills the share in (`capture_layout`), and the restore lays that
    // share back down on a desktop of a different size. A 50/50 split reattaches
    // as a 50/50 split, where replaying the stored cell rects would leave a
    // strip of bare desktop on a wider terminal and an overlap on a narrower.
    LayoutReports reports;
    ConfiguredFixture f(watching_layout(reports));
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    CK_CHECK(terminal_window_count(f.client) == 2U);

    // An exact, desktop-filling tiling — which is what makes the query answer
    // at all. It is measured, not flagged: a window dragged off the grid stops
    // being part of one immediately (ckVision U4-b).
    f.client.desktop().tile_horizontally();
    f.app.step(0);
    layout_tick(f);
    layout_tick(f);
    CK_CHECK(!reports.empty());
    if (reports.empty()) return;
    const std::vector<ckm::client::WindowPlacement> arranged = reports.back();
    CK_CHECK(arranged.size() == 2U);
    if (arranged.size() != 2U) return;
    for (const ckm::client::WindowPlacement& placed : arranged) {
        CK_CHECK(placed.tile.filled());
        CK_CHECK(placed.tile.width > 0.49 && placed.tile.width < 0.51);
        CK_CHECK(placed.tile.height > 0.99);
    }

    // The reader comes back on a terminal of another size entirely.
    f.terminal.resize(Size{80, 24});
    f.app.step(0);
    const ckv::Rect area = f.client.desktop().content_area();
    f.client.apply_layout(arranged);

    // Still a 50/50 split, and still an exact one: the two windows meet on a
    // single boundary and together fill the desktop. That is why the fraction
    // is applied as two EDGES rather than as an origin and a width — rounding
    // each width on its own gives two halves that overlap by a column.
    // Which band is which comes from the share itself rather than from the
    // order the report happens to be in — that order is the z-stack, and the
    // stack says nothing about who is on the left.
    const bool zero_is_left = arranged[0].tile.x < arranged[1].tile.x;
    ckv::widgets::Window* const left = showing(f.client, arranged[zero_is_left ? 0 : 1].terminal);
    ckv::widgets::Window* const right = showing(f.client, arranged[zero_is_left ? 1 : 0].terminal);
    CK_CHECK(left != nullptr && right != nullptr);
    if (left == nullptr || right == nullptr) return;
    CK_CHECK(left->bounds().x == area.x);
    CK_CHECK(left->bounds().y == area.y);
    CK_CHECK(left->bounds().height == area.height);
    CK_CHECK(right->bounds().right() == area.right());
    CK_CHECK(right->bounds().height == area.height);
    CK_CHECK(left->bounds().right() == right->bounds().x);
    CK_CHECK(left->bounds().width + right->bounds().width == area.width);

    // And the maximized case, which is read first and answers on its own: the
    // window fills the desktop as it is NOW rather than the rect it filled on
    // the terminal it was maximized on.
    Fixture maximized;
    maximized.app.step(0);
    ckv::widgets::Window* const window = maximized.client.desktop().windows()[0];
    const ckv::Rect filled_then = maximized.client.desktop().content_area();
    maximized.terminal.resize(Size{60, 20});
    maximized.app.step(0);
    const ckv::Rect filled_now = maximized.client.desktop().content_area();
    CK_CHECK(filled_now != filled_then);
    maximized.client.apply_layout({stored(window, filled_then, 0, /*zoomed=*/true)});
    CK_CHECK(window->zoomed());
    CK_CHECK(window->bounds() == filled_now);
}

// --- WP-13: the exit banner --------------------------------------------------

namespace {

// A client whose terminals hold after their child ends, running a program that
// ends at once. `/usr/bin/false` is chosen for the status: `[exit 1]` and
// `[exit 0]` are different news to a reader, so a test that used `true` could
// not tell a badge that reports the status from one that prints a zero.
ClientOptions exiting_options() {
    ClientOptions options = test_options();
    options.settings.shell = "/usr/bin/false";
    return options;
}

// Waits for the child to be reaped, bounded. A real process is involved, so
// the only honest way to wait for one is to wait — with the bound failing the
// test rather than hanging it.
bool pump_until_exited(ConfiguredFixture& f, int milliseconds = 4000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        f.clock.advance(200'000'000);
        f.app.step(f.clock.now_nanos());
        for (ckv::widgets::Window* window : f.client.desktop().windows()) {
            auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(window->content());
            if (view == nullptr) continue;
            using State = ckv::core::TerminalSubsessionState;
            const State state = view->session().state();
            if (state == State::Exited || state == State::Failed) return true;
        }
        ::usleep(2000);
    }
    return false;
}

// Exact rather than substring, deliberately — see the footer test below.
bool has_exact_label(const std::vector<std::string>& labels, std::string_view wanted) {
    for (const std::string& label : labels)
        if (label == wanted) return true;
    return false;
}

ckv::widgets::Window* first_terminal_window(ClientApp& client) {
    for (ckv::widgets::Window* window : client.desktop().windows())
        if (dynamic_cast<ckv::widgets::TerminalView*>(window->content()) != nullptr) return window;
    return nullptr;
}

}  // namespace

CK_TEST(a_window_whose_program_ended_wears_the_status_it_ended_with) {
    // The interface spec — "dim border, title badge `[exit 1]`". The number is the
    // point: a reader looking at a build loop needs to know whether it passed.
    ConfiguredFixture f{[] {
        ClientOptions options = exiting_options();
        // The host supplies the number, because the seam carries `state()` and
        // no status. Without this the badge can only say that something ended.
        options.exit_status = [](const ckv::term::TerminalSubsession&) { return 1; };
        return options;
    }()};
    CK_CHECK(pump_until_exited(f));
    ckv::widgets::Window* const window = first_terminal_window(f.client);
    CK_CHECK(window != nullptr);
    if (window != nullptr) CK_CHECK(window->title().find("[exit 1]") != std::string::npos);
}

CK_TEST(a_status_nobody_can_supply_is_said_as_exited_rather_than_as_zero) {
    // No `exit_status` callback — a client with no server, or one whose mirror
    // never learned the number. `[exit 0]` would be a lie: it says the program
    // succeeded, which is exactly the claim that cannot be made here.
    ConfiguredFixture f{exiting_options()};
    CK_CHECK(pump_until_exited(f));
    ckv::widgets::Window* const window = first_terminal_window(f.client);
    CK_CHECK(window != nullptr);
    if (window != nullptr) {
        CK_CHECK(window->title().find("[exited]") != std::string::npos);
        CK_CHECK(window->title().find("[exit 0]") == std::string::npos);
    }
}

CK_TEST(a_held_window_offers_restart_only_where_something_can_honour_it) {
    // The footer and the key binding ask the same question, so a reader is
    // never offered a key that does nothing. A client with no server has
    // nobody to ask for a respawn, so the hint is absent there and present
    // where a `request_respawn` exists.
    {
        ConfiguredFixture without{exiting_options()};
        CK_CHECK(pump_until_exited(without));
        // Exact, not contains: the ordinary terminal footer carries
        // "^B x close term", which a substring test for "x close" matches —
        // so a contains-check here would pass while the held footer was never
        // shown at all. It did, until this was tightened.
        const std::vector<std::string> labels = without.client.footer_labels();
        CK_CHECK(has_exact_label(labels, "x close"));
        CK_CHECK(!has_exact_label(labels, "Enter restart"));
    }
    {
        ConfiguredFixture with{[] {
            ClientOptions options = exiting_options();
            options.request_respawn = [](const ckv::term::TerminalSubsession&) {};
            return options;
        }()};
        CK_CHECK(pump_until_exited(with));
        const std::vector<std::string> labels = with.client.footer_labels();
        CK_CHECK(has_exact_label(labels, "Enter restart"));
        CK_CHECK(has_exact_label(labels, "x close"));
    }
}

CK_TEST(enter_restarts_a_held_window_and_x_closes_it) {
    // The two keys, pressed the way a reader presses them — through the
    // application's own event path, not by calling a handler (ckVision's rule).
    // They reach ckmux at all only because the view offers keys to its host
    // once the child is gone; while it is alive every key belongs to the child.
    int restarts = 0;
    ConfiguredFixture f{[&restarts] {
        ClientOptions options = exiting_options();
        options.request_respawn = [&restarts](const ckv::term::TerminalSubsession&) { ++restarts; };
        return options;
    }()};
    CK_CHECK(pump_until_exited(f));
    CK_CHECK(terminal_window_count(f.client) == 1U);

    CK_CHECK(f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}}));
    CK_CHECK(restarts == 1);
    // Restarting does not close the window — that is the whole of "in the same
    // window", and a respawn that took the window with it would be a new
    // terminal wearing the old one's place.
    CK_CHECK(terminal_window_count(f.client) == 1U);

    CK_CHECK(f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "x"}}));
    f.app.step(f.clock.now_nanos());
    CK_CHECK(terminal_window_count(f.client) == 0U);
    // And closing did not also ask for a restart on the way out.
    CK_CHECK(restarts == 1);
}

// --- WP-19: bell and activity monitors --------------------------------------

namespace {

// A client whose terminals report whatever the test says they are doing. The
// marks are the SERVER's in a real client, so a test supplies them the same
// way the server would — through the seam, not by reaching into a window.
struct MarkedFixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    std::map<const ckv::term::TerminalSubsession*, ckm::client::TerminalMarks> marks;
    int rings = 0;
    ClientApp client;

    explicit MarkedFixture(bool audible = false)
        : client{app, [this, audible] {
                     ClientOptions options = test_options();
                     options.settings.audible_bell = audible;
                     options.terminal_marks =
                         [this](const ckv::term::TerminalSubsession& asked_about) {
                             const auto found = marks.find(&asked_about);
                             return found == marks.end() ? ckm::client::TerminalMarks{}
                                                         : found->second;
                         };
                     options.ring_host_bell = [this] { ++rings; };
                     return options;
                 }()} {}

    void poll() {
        clock.advance(200'000'000);
        app.step(clock.now_nanos());
    }
};

const ckv::term::TerminalSubsession* subsession_of(ckv::widgets::Window* window) {
    auto* const view = window == nullptr
                           ? nullptr
                           : dynamic_cast<ckv::widgets::TerminalView*>(window->content());
    return view == nullptr ? nullptr
                           : dynamic_cast<const ckv::term::TerminalSubsession*>(&view->session());
}

}  // namespace

CK_TEST(a_terminal_the_reader_is_not_looking_at_says_so_in_the_footer) {
    // The interface spec's status area. The counts are of WINDOWS wanting attention,
    // which is the actionable number — "eleven bells" is not.
    MarkedFixture f;
    f.client.open_terminal("second");
    f.poll();
    std::vector<ckv::widgets::Window*> windows;
    for (ckv::widgets::Window* w : f.client.desktop().windows())
        if (subsession_of(w) != nullptr) windows.push_back(w);
    CK_CHECK(windows.size() == 2U);
    if (windows.size() < 2) return;

    // Nothing marked: no flags at all, rather than a zero.
    f.poll();
    CK_CHECK(!any_label_contains(f.client.footer_labels(), "•"));
    CK_CHECK(!any_label_contains(f.client.footer_labels(), "!"));

    // The window the reader is NOT in rings.
    ckv::widgets::Window* const elsewhere =
        windows.front() == f.client.desktop().active_window() ? windows.back() : windows.front();
    f.marks[subsession_of(elsewhere)] = ckm::client::TerminalMarks{true, true, 1, 1};
    f.poll();
    CK_CHECK(any_label_contains(f.client.footer_labels(), "• 1"));
    CK_CHECK(any_label_contains(f.client.footer_labels(), "! 1"));
}

CK_TEST(the_window_a_reader_is_in_never_raises_a_flag_about_itself) {
    // A terminal a reader is looking at is not telling them anything they
    // cannot already see. This is also what makes the flags go away as they
    // visit the windows, rather than needing to be dismissed.
    MarkedFixture f;
    f.poll();
    ckv::widgets::Window* const only = f.client.desktop().active_window();
    CK_CHECK(subsession_of(only) != nullptr);
    f.marks[subsession_of(only)] = ckm::client::TerminalMarks{true, true, 1, 1};
    f.poll();
    CK_CHECK(!any_label_contains(f.client.footer_labels(), "• 1"));
    CK_CHECK(!any_label_contains(f.client.footer_labels(), "! 1"));
}

CK_TEST(the_host_is_rung_once_per_bell_and_only_when_asked) {
    // The rising edge, not the condition: this poll runs several times a
    // second, and a reader whose terminal rang on every tick would turn the
    // setting off and never learn what it was for.
    MarkedFixture f{/*audible=*/true};
    f.client.open_terminal("second");
    f.poll();
    ckv::widgets::Window* elsewhere = nullptr;
    for (ckv::widgets::Window* w : f.client.desktop().windows())
        if (subsession_of(w) != nullptr && w != f.client.desktop().active_window()) elsewhere = w;
    CK_CHECK(elsewhere != nullptr);
    if (elsewhere == nullptr) return;

    f.marks[subsession_of(elsewhere)] = ckm::client::TerminalMarks{true, false, 1, 0};
    f.poll();
    CK_CHECK(f.rings == 1);
    f.poll();
    f.poll();
    CK_CHECK(f.rings == 1);  // still one: the condition held, no new edge

    // Cleared and rung again is a second bell.
    f.marks[subsession_of(elsewhere)] = ckm::client::TerminalMarks{};
    f.poll();
    f.marks[subsession_of(elsewhere)] = ckm::client::TerminalMarks{true, false, 1, 0};
    f.poll();
    CK_CHECK(f.rings == 2);
}

CK_TEST(visiting_a_terminal_puts_its_mark_down_for_good) {
    // The mark is a statement about THIS READER — "something happened where
    // you were not looking" — so visiting the window answers it. The server's
    // flag cannot do that job: it is sticky, set when the child rings and
    // cleared only when the terminal is respawned, so it says "this terminal
    // has rung at some point", not "since you last looked".
    //
    // Without a client-side memory the footer therefore shows a bell for the
    // rest of the session: hidden while the reader is in that window, back the
    // moment they step out of it. That is what shipped in the first cut of
    // WP-19, and this is the case that says so.
    MarkedFixture f;
    f.client.open_terminal("second");
    f.poll();
    ckv::widgets::Window* first = nullptr;
    ckv::widgets::Window* second = nullptr;
    for (ckv::widgets::Window* w : f.client.desktop().windows()) {
        if (subsession_of(w) == nullptr) continue;
        (w == f.client.desktop().active_window() ? first : second) = w;
    }
    CK_CHECK(first != nullptr && second != nullptr);
    if (first == nullptr || second == nullptr) return;

    // It rings while the reader is elsewhere, and is flagged.
    f.marks[subsession_of(second)] = ckm::client::TerminalMarks{true, false, 1, 0};
    f.poll();
    CK_CHECK(any_label_contains(f.client.footer_labels(), "• 1"));

    // The reader goes and looks at it. The server's flag is unchanged — it
    // never clears — so everything below is about what the CLIENT remembers.
    f.client.desktop().activate(second);
    f.poll();
    CK_CHECK(!any_label_contains(f.client.footer_labels(), "• 1"));

    // And back to the first window. The bell has been seen; it must not
    // return, or the footer becomes a permanent ornament a reader learns to
    // ignore — which is the same as not having it.
    f.client.desktop().activate(first);
    f.poll();
    CK_CHECK(!any_label_contains(f.client.footer_labels(), "• 1"));
}

CK_TEST(activity_arriving_under_a_held_bell_does_not_ring_again) {
    // Found by mutation: removing the rising-edge clause left every test
    // green, because none of them changed one mark while the other stood.
    // A terminal that rang and then kept printing is the ordinary case — a
    // build that beeped and carried on — and it must ring once, not once per
    // line of output.
    MarkedFixture f{/*audible=*/true};
    f.client.open_terminal("second");
    f.poll();
    ckv::widgets::Window* elsewhere = nullptr;
    for (ckv::widgets::Window* w : f.client.desktop().windows())
        if (subsession_of(w) != nullptr && w != f.client.desktop().active_window()) elsewhere = w;
    CK_CHECK(elsewhere != nullptr);
    if (elsewhere == nullptr) return;

    f.marks[subsession_of(elsewhere)] = ckm::client::TerminalMarks{true, false, 1, 0};
    f.poll();
    CK_CHECK(f.rings == 1);
    // The bell still stands; the terminal is now also producing output.
    f.marks[subsession_of(elsewhere)] = ckm::client::TerminalMarks{true, true, 1, 1};
    f.poll();
    CK_CHECK(f.rings == 1);
}

CK_TEST(the_window_a_reader_is_in_neither_rings_nor_lights) {
    // The footer has its own "not the active window" guard, so a test that
    // only reads the footer leaves the poll's guard uncovered — which is what
    // a mutation showed: inverting it changed nothing any test could see.
    // The ring is the observable that goes through the poll.
    MarkedFixture f{/*audible=*/true};
    f.poll();
    ckv::widgets::Window* const only = f.client.desktop().active_window();
    CK_CHECK(subsession_of(only) != nullptr);
    f.marks[subsession_of(only)] = ckm::client::TerminalMarks{true, true, 1, 1};
    f.poll();
    f.poll();
    // A reader looking straight at the terminal that rang does not need their
    // own terminal to ring about it.
    CK_CHECK(f.rings == 0);
}

CK_TEST(a_reader_who_did_not_ask_for_noise_does_not_get_any) {
    // The visual half is not configurable and the audible half is — so with
    // the setting off the flag still appears and the host stays silent.
    MarkedFixture f{/*audible=*/false};
    f.client.open_terminal("second");
    f.poll();
    ckv::widgets::Window* elsewhere = nullptr;
    for (ckv::widgets::Window* w : f.client.desktop().windows())
        if (subsession_of(w) != nullptr && w != f.client.desktop().active_window()) elsewhere = w;
    CK_CHECK(elsewhere != nullptr);
    if (elsewhere == nullptr) return;
    f.marks[subsession_of(elsewhere)] = ckm::client::TerminalMarks{true, false, 1, 0};
    f.poll();
    CK_CHECK(f.rings == 0);
    CK_CHECK(any_label_contains(f.client.footer_labels(), "• 1"));
}

// --- WP-19's rendering: the bell glyph ---------------------------------------

namespace {

// One title poll, exactly. `title_poll_nanos` is 100 ms, and the bell counts
// polls rather than seconds — so a test that advanced by some other amount
// would be counting something the code does not.
void tick(MarkedFixture& f) {
    f.clock.advance(100'000'000);
    f.app.step(f.clock.now_nanos());
}

ckv::widgets::Window* other_window(ClientApp& client) {
    for (ckv::widgets::Window* w : client.desktop().windows())
        if (subsession_of(w) != nullptr && w != client.desktop().active_window()) return w;
    return nullptr;
}

}  // namespace

CK_TEST(answering_records_the_count_reached_rather_than_merely_that_one_was_seen) {
    // Found by mutation: recording a fixed "seen" instead of the serial the
    // reader reached passed every case, because none let the count get past
    // two before a visit. A terminal that rings twice unwatched and is then
    // visited has had BOTH answered — a client remembering "one" would show a
    // mark for the second the instant the reader stepped away, for a bell they
    // had already seen.
    MarkedFixture f;
    f.client.open_terminal("second");
    tick(f);
    ckv::widgets::Window* const away = other_window(f.client);
    ckv::widgets::Window* const here = f.client.desktop().active_window();
    CK_CHECK(away != nullptr && here != nullptr);
    if (away == nullptr || here == nullptr) return;

    f.marks[subsession_of(away)] = ckm::client::TerminalMarks{true, false, 1, 0};
    tick(f);
    f.marks[subsession_of(away)] = ckm::client::TerminalMarks{true, false, 2, 0};
    tick(f);
    CK_CHECK(any_label_contains(f.client.footer_labels(), "• 1"));

    f.client.desktop().activate(away);
    tick(f);
    f.client.desktop().activate(here);
    tick(f);
    CK_CHECK(!any_label_contains(f.client.footer_labels(), "• 1"));
}

CK_TEST(a_terminal_that_rings_again_after_a_visit_says_so_a_second_time) {
    // The case no LEVEL can express, and the reason the wire counts. A flag
    // that reading clears cannot tell a second ring from the first it cleared;
    // a flag that reading does not clear can never go down. A bit gets one of
    // the two wrong whichever way it is defined. A count gets both.
    MarkedFixture f;
    f.client.open_terminal("second");
    tick(f);
    ckv::widgets::Window* const away = other_window(f.client);
    ckv::widgets::Window* const here = f.client.desktop().active_window();
    CK_CHECK(away != nullptr && here != nullptr);
    if (away == nullptr || here == nullptr) return;

    f.marks[subsession_of(away)] = ckm::client::TerminalMarks{true, false, 1, 0};
    tick(f);
    CK_CHECK(any_label_contains(f.client.footer_labels(), "• 1"));

    f.client.desktop().activate(away);
    tick(f);
    f.client.desktop().activate(here);
    tick(f);
    CK_CHECK(!any_label_contains(f.client.footer_labels(), "• 1"));

    // Rings a SECOND time. The level is unchanged — it was already up — so
    // everything here rests on the count having moved.
    f.marks[subsession_of(away)] = ckm::client::TerminalMarks{true, false, 2, 0};
    tick(f);
    CK_CHECK(any_label_contains(f.client.footer_labels(), "• 1"));
    CK_CHECK(away->title().find("⍾") != std::string::npos);
}

CK_TEST(the_bell_glyph_is_one_cell_and_so_is_the_space_that_replaces_it) {
    // The invariant the owner's whitespace clause protects, checked against
    // ckVision's own measurement rather than against how narrow a character
    // looks. U+1F514 🔔 — the obvious choice — is width 2, and would move
    // every unfocused title a column twice a second for as long as it rang.
    CK_CHECK(ckv::text::text_width("⍾") == 1);
    CK_CHECK(ckv::text::text_width(" ") == 1);
    CK_CHECK(ckv::text::text_width("\U0001F514") != 1);  // the one not to use
}

CK_TEST(a_bell_in_the_window_a_reader_is_in_is_said_once_and_then_let_go) {
    // Five seconds, steady, not blinking: the program rang while they were
    // looking at it, so this is an acknowledgement rather than a summons.
    MarkedFixture f;
    tick(f);
    ckv::widgets::Window* const here = f.client.desktop().active_window();
    CK_CHECK(subsession_of(here) != nullptr);
    f.marks[subsession_of(here)] = ckm::client::TerminalMarks{true, false, 1, 0};

    tick(f);
    CK_CHECK(here->title().find("⍾") != std::string::npos);
    // Steady while it stands: no off phase to catch it in.
    for (int i = 0; i < 8; ++i) {
        tick(f);
        CK_CHECK(here->title().find("⍾") != std::string::npos);
    }
    // And it stops. Fifty polls is five seconds; well past that it is gone,
    // rather than blinking on forever at a slow rate.
    for (int i = 0; i < 60; ++i) tick(f);
    CK_CHECK(here->title().find("⍾") == std::string::npos);
}

CK_TEST(a_bell_in_a_window_the_reader_is_not_in_blinks_until_they_answer_it) {
    MarkedFixture f;
    f.client.open_terminal("second");
    tick(f);
    ckv::widgets::Window* const away = other_window(f.client);
    CK_CHECK(away != nullptr);
    if (away == nullptr) return;
    f.marks[subsession_of(away)] = ckm::client::TerminalMarks{true, false, 1, 0};

    // Both phases occur within one cycle — the point of a blink is that it
    // has two states, and a test that only ever caught one would pass on a
    // glyph that never turned off.
    bool seen_on = false;
    bool seen_off = false;
    for (int i = 0; i < 20; ++i) {
        tick(f);
        if (away->title().find("⍾") != std::string::npos) seen_on = true;
        else seen_off = true;
    }
    CK_CHECK(seen_on);
    CK_CHECK(seen_off);
}

CK_TEST(a_blinking_bell_never_moves_the_title_it_sits_in_front_of) {
    // The whitespace clause, stated as the property it exists for: across a
    // full cycle the rendered width must not change, or the row re-flows twice
    // a second. This is the case that fails if anybody swaps the glyph for an
    // emoji, and it fails on WIDTH rather than on the character, so it keeps
    // working for whatever glyph a later reader prefers.
    MarkedFixture f;
    f.client.open_terminal("second");
    tick(f);
    ckv::widgets::Window* const away = other_window(f.client);
    CK_CHECK(away != nullptr);
    if (away == nullptr) return;
    f.marks[subsession_of(away)] = ckm::client::TerminalMarks{true, false, 1, 0};

    tick(f);
    const int width = ckv::text::text_width(away->title());
    for (int i = 0; i < 20; ++i) {
        tick(f);
        CK_CHECK(ckv::text::text_width(away->title()) == width);
    }
}

CK_TEST(visiting_the_window_puts_the_bell_glyph_down_with_the_mark) {
    // The glyph answers the same question the footer flag does, so it is
    // answered the same way: by looking. Without this the blink would outlive
    // the reason for it.
    MarkedFixture f;
    f.client.open_terminal("second");
    tick(f);
    ckv::widgets::Window* const away = other_window(f.client);
    CK_CHECK(away != nullptr);
    if (away == nullptr) return;
    f.marks[subsession_of(away)] = ckm::client::TerminalMarks{true, false, 1, 0};
    for (int i = 0; i < 4; ++i) tick(f);

    f.client.desktop().activate(away);
    for (int i = 0; i < 60; ++i) tick(f);   // past its own five seconds
    CK_CHECK(away->title().find("⍾") == std::string::npos);

    // And back out again, which is the half that matters and that the first
    // version of this case missed: while the reader stands IN the window the
    // blink branch never runs at all, so a mutation making a visited bell
    // blink forever passed. The bell must stay down once answered — that is
    // what "answered" means, and stepping away is when it would return.
    ckv::widgets::Window* const back = f.client.desktop().windows().front() == away
                                           ? f.client.desktop().windows().back()
                                           : f.client.desktop().windows().front();
    f.client.desktop().activate(back);
    for (int i = 0; i < 20; ++i) {
        tick(f);
        CK_CHECK(away->title().find("⍾") == std::string::npos);
    }
}
