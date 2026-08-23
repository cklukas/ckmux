// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Putting a terminal away, and getting a row of the screen back (WP-34,
// WP-35) — the two halves of one reader's report: "the window bar does not
// appear when the single terminal in a session is minimized", and "give me a
// way to push the window list down and hide the status bar".
//
// They meet in the same place, which is why they are tested together: the
// bottom chrome is one docked `ui::Column` of two rows, and both packages are
// about which of those rows is on screen and what the desktop does about it.
// The rule WP-34 adds is the one that keeps a reader from being stranded — a
// terminal that is hidden is reachable only from the bar, so the bar must be
// there whenever anything is hidden, even when there is only one terminal and
// nothing to switch between.
//
// Driven headlessly the way main.cpp drives the client: real Application,
// real menus, real mouse reports. The child is /bin/cat, which stays alive
// and says nothing.
#if !defined(_WIN32)

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "client/client_app.hpp"
#include "client/commands.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/status_line.hpp"
#include "cvision/widgets/terminal_view.hpp"
#include "cvision/widgets/window_switcher_bar.hpp"

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;
using Status = ckv::widgets::WindowSwitcherBar::Status;

namespace {

constexpr int kRows = 30;

ClientOptions test_options() {
    ClientOptions options;
    options.settings.shell = "/bin/cat";
    // Undamped, so a width read straight after a state change is the width
    // that state asks for (U4-m holds a box still for a second otherwise).
    // None of these tests is about damping, and one that had to advance a
    // clock to see a row would be testing the wrong thing.
    options.switcher_grow_nanos = 0;
    options.switcher_shrink_nanos = 0;
    return options;
}

struct Fixture {
    ckv::term::HeadlessTerminal terminal{Size{100, kRows}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, test_options()};

    bool press(ckv::KeyChord chord) { return app.dispatch(ckv::KeyEvent{std::move(chord)}); }
    bool press_prefix() { return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "b"}); }
    void settle() { app.step(clock.now_nanos()); }
};

ckv::ui::CommandId id_of(Application& app, std::string_view key) {
    return app.commands().id_for(key).value_or(ckv::ui::kInvalidCommand);
}

ckv::widgets::MenuBar* menu_bar(ClientApp& client) {
    return dynamic_cast<ckv::widgets::MenuBar*>(client.desktop().top_dock());
}

ckv::widgets::DropdownMenu* dropped_menu(ClientApp& client) {
    for (ckv::ui::View* popup : client.desktop().popups())
        if (auto* const menu = dynamic_cast<ckv::widgets::DropdownMenu*>(popup)) return menu;
    return nullptr;
}

// The ABSOLUTE cell in the middle of one switcher entry, converted through the
// bar's own placement rather than through an assumption about where it was
// docked — which is exactly what these tests move.
std::optional<ckv::Point> switcher_cell(ClientApp& client, std::size_t index) {
    const ckv::Rect where = client.window_switcher().absolute_bounds();
    for (const ckv::widgets::WindowSwitcherBar::DrawnEntry& drawn :
         client.window_switcher().drawn_entries())
        if (drawn.index == index)
            return ckv::Point{where.x + drawn.x + drawn.width / 2, where.y};
    return std::nullopt;
}

bool switcher_click(Fixture& f, std::size_t index) {
    const std::optional<ckv::Point> cell = switcher_cell(f.client, index);
    if (!cell) return false;
    (void)f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Left, *cell,
                                         std::nullopt, ckv::Modifier::None});
    (void)f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Up, ckv::MouseButton::Left, *cell,
                                         std::nullopt, ckv::Modifier::None});
    f.app.step(0);
    return true;
}

bool switcher_right_press(Fixture& f, std::size_t index) {
    const std::optional<ckv::Point> cell = switcher_cell(f.client, index);
    if (!cell) return false;
    (void)f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Right, *cell,
                                         std::nullopt, ckv::Modifier::None});
    f.app.step(0);
    return true;
}

// A press on the bar's own ▼, which is at its first column when the host has
// asked for one (PagedStrip::Chrome::collapse_x).
bool press_collapse_toggle(Fixture& f) {
    const ckv::widgets::WindowSwitcherBar& bar = f.client.window_switcher();
    if (!bar.visible() || bar.chrome().collapse_x < 0) return false;
    const ckv::Rect where = bar.absolute_bounds();
    (void)f.app.dispatch(ckv::MouseEvent{
        ckv::MouseAction::Down, ckv::MouseButton::Left,
        ckv::Point{where.x + bar.chrome().collapse_x, where.y}, std::nullopt, ckv::Modifier::None});
    f.app.step(0);
    return true;
}

// A second terminal, so that a test that needs the bar on screen for reasons
// of its own does not have to minimize something to get it there.
void open_second_terminal(Fixture& f) {
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
}

std::size_t index_of_command(const ckv::widgets::MenuBarItem& menu, ckv::ui::CommandId command) {
    for (std::size_t i = 0; i < menu.items.size(); ++i)
        if (menu.items[i].command() == command) return i;
    return menu.items.size();
}

// The menu with this title, or nullptr. By TITLE, never by position: this
// case used to read `menus()[2]` and went red the day another package
// inserted a View menu ahead of Window — the rules it asserts were untouched
// and the test was simply looking in the wrong menu. A position in a
// structure other sessions can extend is a test with a scheduled failure
// date, and the items below are already found by command id for the same
// reason.
const ckv::widgets::MenuBarItem* menu_titled(const ckv::widgets::MenuBar& bar,
                                             std::string_view title) {
    for (const ckv::widgets::MenuBarItem& menu : bar.menus())
        if (menu.label == title) return &menu;
    return nullptr;
}

}  // namespace

// --- WP-34: the bar appears whenever something is hidden -------------------

CK_TEST(minimizing_the_only_terminal_brings_the_bar_on_screen) {
    Fixture f;
    f.settle();
    ckv::widgets::Window* const only = f.client.desktop().windows()[0];
    // One terminal and nothing hidden: no bar, which is the rule WP-32 set
    // and which this package does not undo.
    CK_CHECK(!f.client.window_switcher().visible());

    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kMinimize)));
    f.settle();

    // The reader's report, in one assertion: their program is off the screen
    // and the row that names it is on.
    CK_CHECK(only->minimized());
    CK_CHECK(!only->visible());
    CK_CHECK(f.client.window_switcher().visible());
    CK_CHECK(f.client.window_switcher().entries().size() == 1U);
    if (f.client.window_switcher().entries().empty()) return;
    CK_CHECK(f.client.window_switcher().entries()[0].window == only);
    CK_CHECK(f.client.window_switcher().entries()[0].status() == Status::Minimized);
    // And the desktop knows the row is there: a bar nothing reserved a line
    // for would be drawn over by the next window that filled the screen.
    CK_CHECK(f.client.window_switcher().absolute_bounds().y == kRows - 2);
}

CK_TEST(the_bar_that_appeared_is_the_way_back_to_the_terminal) {
    Fixture f;
    f.settle();
    ckv::widgets::Window* const only = f.client.desktop().windows()[0];
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kMinimize)));
    f.settle();
    CK_CHECK(f.client.window_switcher().visible());

    // One click on the row it left behind.
    CK_CHECK(switcher_click(f, 0));
    f.settle();
    CK_CHECK(!only->minimized());
    CK_CHECK(only->visible());
    CK_CHECK(f.client.desktop().active_window() == only);
    // And the keyboard came with it: a restored terminal the reader cannot
    // type into is a window they have to click a second time.
    CK_CHECK(f.app.focused() == dynamic_cast<ckv::widgets::TerminalView*>(only->content()));
    // The reason for the row is gone, so the row is gone: one terminal, none
    // hidden, and the line goes back to the reader's program.
    CK_CHECK(!f.client.window_switcher().visible());
}

CK_TEST(the_prefix_chord_puts_the_active_terminal_away) {
    Fixture f;
    f.settle();
    ckv::widgets::Window* const only = f.client.desktop().windows()[0];

    // `^B _`, the chord the default table gives it — the glyph on the
    // window's own frame control.
    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "_"}));
    f.settle();

    CK_CHECK(only->minimized());
    CK_CHECK(f.client.window_switcher().visible());
}

CK_TEST(the_window_menu_offers_minimize_beside_zoom) {
    Fixture f;
    f.settle();
    ckv::widgets::MenuBar* const bar = menu_bar(f.client);
    CK_CHECK(bar != nullptr);
    if (bar == nullptr) return;
    const ckv::widgets::MenuBarItem* const window_menu = menu_titled(*bar, "&Window");
    CK_CHECK(window_menu != nullptr);
    if (window_menu == nullptr) return;

    const std::size_t zoom =
        index_of_command(*window_menu, id_of(f.app, ckv::ui::std_command_keys::kZoom));
    const std::size_t minimize =
        index_of_command(*window_menu, id_of(f.app, ckv::ui::std_command_keys::kMinimize));
    CK_CHECK(zoom < window_menu->items.size());
    CK_CHECK(minimize < window_menu->items.size());
    // Beside, not merely present: the two verbs are the two controls the
    // window draws together on its own frame, and a menu that separated them
    // would be describing a different window.
    CK_CHECK(minimize == zoom + 1U);
}

CK_TEST(the_rows_own_menu_puts_away_the_window_the_reader_pointed_at) {
    Fixture f;
    f.settle();
    open_second_terminal(f);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];
    CK_CHECK(f.client.desktop().active_window() == second);

    CK_CHECK(switcher_right_press(f, 0));
    ckv::widgets::DropdownMenu* const menu = dropped_menu(f.client);
    CK_CHECK(menu != nullptr);
    if (menu == nullptr) return;
    CK_CHECK(menu->items().size() == 9U);
    if (menu->items().size() != 9U) return;
    CK_CHECK(menu->items()[0].label() == "Mi&nimize");

    menu->items()[0].action()();
    f.settle();
    // The BACKGROUND window went away, which is the window whose row was
    // clicked — command dispatch would have hidden the one in front.
    CK_CHECK(first->minimized());
    CK_CHECK(!second->minimized());
    CK_CHECK(f.client.desktop().active_window() == second);
    CK_CHECK(f.client.window_switcher().entries()[0].status() == Status::Minimized);
}

CK_TEST(the_rows_own_menu_says_show_for_a_window_that_is_already_away) {
    Fixture f;
    f.settle();
    open_second_terminal(f);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    first->set_minimized(true);
    f.settle();

    CK_CHECK(switcher_right_press(f, 0));
    ckv::widgets::DropdownMenu* const menu = dropped_menu(f.client);
    CK_CHECK(menu != nullptr);
    if (menu == nullptr || menu->items().empty()) return;
    // "&Show" rather than a second "&Restore": the row below it already
    // reads Restore whenever that window is maximized, and two items of the
    // same name in one menu is a reader guessing.
    CK_CHECK(menu->items()[0].label() == "&Show");

    menu->items()[0].action()();
    f.settle();
    CK_CHECK(!first->minimized());
    CK_CHECK(f.client.desktop().active_window() == first);
}

CK_TEST(a_second_terminal_leaves_the_bar_where_it_already_was) {
    // The clause is an OR, not a replacement: the count rule still puts the
    // bar on screen on its own, and a minimized window leaving does not take
    // the bar with it while more than one terminal is open.
    Fixture f;
    f.settle();
    open_second_terminal(f);
    CK_CHECK(f.client.window_switcher().visible());

    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    first->set_minimized(true);
    f.settle();
    CK_CHECK(f.client.window_switcher().visible());

    first->set_minimized(false);
    f.settle();
    CK_CHECK(f.client.window_switcher().visible());
}

// --- What leaves the screen with the window --------------------------------

CK_TEST(minimizing_the_only_terminal_takes_the_cursor_and_the_keyboard_with_it) {
    // A reader's report: "when I minimize a window the cursor does not
    // disappear, it appears as a bright cell on the background". The window
    // is hidden, but nothing had taken the keyboard away from the view inside
    // it — with every terminal minimized there is nothing to hand it to — and
    // a focused terminal publishes its cursor in ABSOLUTE cells, so the host
    // was still being told to park the cursor in a cell that now shows the
    // desktop.
    Fixture f;
    f.settle();
    ckv::widgets::Window* const only = f.client.desktop().windows()[0];
    CK_CHECK(f.app.current_cursor().visible);
    const ckv::Point where = f.app.current_cursor().position;
    CK_CHECK(f.terminal.display().cursor().visible);

    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kMinimize)));
    f.settle();
    CK_CHECK(only->minimized());
    CK_CHECK(!f.app.current_cursor().visible);
    // On the wire, not merely in the compositor: the host was TOLD to hide
    // the cursor. A frame that only stopped addressing it would leave the
    // bright cell exactly where the reader reported it, because a terminal
    // keeps drawing a cursor it was never asked to hide.
    CK_CHECK(!f.terminal.display().cursor().visible);
    // The other half, and the reason the cursor was there at all: the
    // keyboard does not stay in a terminal that is off the screen. Nothing
    // is focused, which is the state a client started without a session is
    // already in — `^B` reaches the command table from there.
    CK_CHECK(f.app.focused() == nullptr);

    // Back from the bar, and the cursor comes back to the cell it left.
    CK_CHECK(switcher_click(f, 0));
    f.settle();
    CK_CHECK(f.app.current_cursor().visible);
    CK_CHECK(f.app.current_cursor().position == where);
    CK_CHECK(f.terminal.display().cursor().visible);
    CK_CHECK(f.terminal.display().cursor().position == where);
}

CK_TEST(a_window_put_away_from_its_own_frame_hands_the_keyboard_on) {
    // The `↓` on the window's own top border is `Window::set_minimized`
    // itself: it never passes through ckmux, which is why the bar's minimize
    // action re-homing the keyboard was not enough. The library route is what
    // this test takes, and the rule is the same whichever of the four routes
    // a reader used — the keys belong in whatever is still on the screen.
    Fixture f;
    f.settle();
    open_second_terminal(f);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];
    CK_CHECK(f.app.focused() ==
             dynamic_cast<ckv::widgets::TerminalView*>(second->content()));

    second->set_minimized(true);
    f.settle();

    CK_CHECK(f.client.desktop().active_window() == first);
    CK_CHECK(f.app.focused() == dynamic_cast<ckv::widgets::TerminalView*>(first->content()));
    // And the cursor is in the terminal the reader can see, rather than in
    // the rectangle the hidden one used to occupy.
    CK_CHECK(f.app.current_cursor().visible);
    CK_CHECK(first->absolute_bounds().contains(f.app.current_cursor().position));
}

// --- WP-35: collapsing the chrome ------------------------------------------

CK_TEST(collapsing_hides_the_footer_and_puts_the_bar_on_the_last_row) {
    Fixture f;
    f.settle();
    open_second_terminal(f);
    CK_CHECK(f.client.window_switcher().visible());
    CK_CHECK(f.client.footer().visible());
    CK_CHECK(f.client.window_switcher().absolute_bounds().y == kRows - 2);
    CK_CHECK(!f.client.chrome_collapsed());

    CK_CHECK(press_collapse_toggle(f));
    f.settle();

    CK_CHECK(f.client.chrome_collapsed());
    CK_CHECK(f.client.window_switcher().collapsed());
    CK_CHECK(!f.client.footer().visible());
    // The row the footer was on, which is what the reader asked for.
    CK_CHECK(f.client.window_switcher().visible());
    CK_CHECK(f.client.window_switcher().absolute_bounds().y == kRows - 1);

    // And back, both together.
    CK_CHECK(press_collapse_toggle(f));
    f.settle();
    CK_CHECK(!f.client.chrome_collapsed());
    CK_CHECK(f.client.footer().visible());
    CK_CHECK(f.client.window_switcher().absolute_bounds().y == kRows - 2);
}

CK_TEST(a_maximized_terminal_grows_into_the_freed_row_and_gives_it_back) {
    Fixture f;
    f.settle();
    open_second_terminal(f);
    ckv::widgets::Window* const front = f.client.desktop().active_window();
    CK_CHECK(front != nullptr);
    if (front == nullptr) return;
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kZoom)));
    f.settle();
    CK_CHECK(front->zoomed());
    const int maximized_height = front->bounds().height;
    CK_CHECK(front->bounds() == f.client.desktop().content_area());

    f.client.set_chrome_collapsed(true);
    f.settle();
    // A row of chrome that went is a row of terminal that arrived. This is
    // the assertion the whole package is for: `content_area()` answers
    // correctly on its own, but nothing re-fills a zoomed window unless the
    // desktop is told the dock's height changed.
    CK_CHECK(front->bounds().height == maximized_height + 1);
    CK_CHECK(front->bounds() == f.client.desktop().content_area());

    f.client.set_chrome_collapsed(false);
    f.settle();
    CK_CHECK(front->bounds().height == maximized_height);
    CK_CHECK(front->bounds() == f.client.desktop().content_area());
}

CK_TEST(the_menu_command_and_the_bars_toggle_are_one_state) {
    Fixture f;
    f.settle();
    open_second_terminal(f);

    // The command's route.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kToggleStatusBar)));
    f.settle();
    CK_CHECK(f.client.chrome_collapsed());
    // The bar's own glyph agrees, because there is one state and both routes
    // set it — a ▼ that still said "expanded" over a hidden footer would be
    // the surface disagreeing with itself.
    CK_CHECK(f.client.window_switcher().collapsed());
    CK_CHECK(!f.client.footer().visible());

    // And the toggle's route puts it back.
    CK_CHECK(press_collapse_toggle(f));
    f.settle();
    CK_CHECK(!f.client.chrome_collapsed());
    CK_CHECK(!f.client.window_switcher().collapsed());
    CK_CHECK(f.client.footer().visible());
}

CK_TEST(the_status_bar_comes_back_from_the_command_when_no_bar_is_on_screen) {
    // Why the command exists beside the ▼. One terminal, nothing minimized:
    // the bar is not there, so the toggle is not there either, and a reader
    // who collapsed the chrome while it was would otherwise have no way back
    // to their footer.
    Fixture f;
    f.settle();
    CK_CHECK(!f.client.window_switcher().visible());

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kToggleStatusBar)));
    f.settle();
    CK_CHECK(f.client.chrome_collapsed());
    CK_CHECK(!f.client.footer().visible());

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kToggleStatusBar)));
    f.settle();
    CK_CHECK(!f.client.chrome_collapsed());
    CK_CHECK(f.client.footer().visible());
}

CK_TEST(the_chrome_state_survives_the_bar_coming_and_going) {
    // The two rules meet here: collapsing is the reader's decision about
    // ckmux's chrome, and the bar appearing or disappearing is a fact about
    // their windows. Neither may quietly undo the other.
    Fixture f;
    f.settle();
    open_second_terminal(f);
    f.client.set_chrome_collapsed(true);
    f.settle();
    CK_CHECK(f.client.window_switcher().absolute_bounds().y == kRows - 1);

    // Down to one terminal: the bar goes, and the footer stays away because
    // that is what the reader asked for. `/bin/cat` is alive in the window,
    // so closing it asks first — the reader's own Enter is part of the path.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kCloseTerminal)));
    f.settle();
    f.settle();
    if (f.app.is_modal())
        (void)f.app.dispatch(
            ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    f.settle();
    f.settle();
    CK_CHECK(!f.client.window_switcher().visible());
    CK_CHECK(f.client.chrome_collapsed());
    CK_CHECK(!f.client.footer().visible());

    // And when the bar comes back — here by minimizing that last terminal —
    // it comes back collapsed, on the last row, still reporting ▲.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckv::ui::std_command_keys::kMinimize)));
    f.settle();
    CK_CHECK(f.client.window_switcher().visible());
    CK_CHECK(f.client.window_switcher().collapsed());
    CK_CHECK(f.client.window_switcher().absolute_bounds().y == kRows - 1);
}

#endif  // !defined(_WIN32)
