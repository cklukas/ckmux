// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Every key and every menu entry ckmux offers must do one of two things: work,
// or say plainly that it does not. There is no third state, and this suite
// exists because there was one.
//
// A ckVision command whose handler nobody installed is still *available* —
// CommandRegistry::is_available() asks the enablement predicate and the focus
// context, neither of which knows whether anyone is listening. So a menu item
// bound to such a command draws in the ordinary colour, accepts the click, and
// does nothing. Terminal ▸ Window List… shipped that way through all of M1: it
// referenced ckVision's window-list command, whose default handler Desktop
// does not install, and the roadmap recorded "window list" as landed.
//
// The rule below is therefore stated over the keymap table, which is the
// single source of truth for every surface that mentions a key: a binding is
// live (someone handles it) or deliberately dark (a predicate returns false,
// so it is greyed and unclickable everywhere). Silence is the bug.
#if !defined(_WIN32)

#include <cstdio>
#include <set>
#include <string>
#include <string_view>

#include "client/client_app.hpp"
#include "client/commands.hpp"
#include "client/stats_format.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/term/virtual_display.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/menu.hpp"

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckm::client::KeyBinding;
using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;

namespace {

ClientOptions test_options() {
    ClientOptions options;
    options.settings.shell = "/bin/cat";  // stays alive, says nothing (test_client_smoke)
    return options;
}

struct Fixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, test_options()};
};

// The commands ckmux deliberately shows greyed, and why. Anything else that
// turns out to be unhandled is a dead control, not a documented gap
// (the roadmap "Deliberately not yet true").
bool is_deliberately_dark(std::string_view key) {
    return key == ckm::client::commands::kDetach ||       // M2: no server yet
           key == ckm::client::commands::kSessions ||     // M2: no server yet
           key == ckm::client::commands::kFocusByNumber;  // digits carry a value
}

}  // namespace

CK_TEST(every_row_of_the_table_resolves_to_a_command_that_exists) {
    // The new failure mode the string-keyed command model brings with it: a
    // row naming "ckmux.terminal.nwe" resolves to nothing, and an id of
    // nothing is unhandled AND unavailable — which the dead-entry test below
    // reads as "greyed on purpose" and lets through. A typo in a key would
    // therefore ship as a menu item that quietly does nothing, which is the
    // exact defect that file exists to prevent. So the resolution itself is
    // checked first, by name.
    Fixture fixture;
    std::string unresolved;
    for (const KeyBinding& binding : fixture.client.keymap().bindings()) {
        if (binding.command != ckv::ui::kInvalidCommand) continue;
        if (!unresolved.empty()) unresolved += ", ";
        unresolved += std::string(binding.key);
    }
    if (!unresolved.empty())
        std::fprintf(stderr, "keys nothing declared: %s\n", unresolved.c_str());
    CK_CHECK(unresolved.empty());
}

CK_TEST(every_binding_either_works_or_is_visibly_disabled) {
    Fixture fixture;
    std::string dead;
    for (const KeyBinding& binding : fixture.client.keymap().bindings()) {
        const bool handled = fixture.app.commands().has_handler(binding.command);
        const bool available = fixture.app.command_available(binding.command);
        if (handled) continue;     // live
        if (!available) continue;  // greyed, and says so
        if (!dead.empty()) dead += ", ";
        dead += binding.title;
    }
    // Named on stderr, so a failure hands over the fix list rather than a bool.
    if (!dead.empty()) std::fprintf(stderr, "dead menu entries: %s\n", dead.c_str());
    CK_CHECK(dead.empty());
}

CK_TEST(deliberately_dark_commands_are_actually_dark) {
    Fixture fixture;
    for (const KeyBinding& binding : fixture.client.keymap().bindings()) {
        if (!is_deliberately_dark(binding.key)) continue;
        // Not merely unhandled: a predicate must refuse it, which is what
        // greys the menu item and stops the key doing nothing in silence.
        CK_CHECK(!fixture.app.command_available(binding.command));
    }
}

CK_TEST(each_of_the_three_tilings_is_reachable_by_its_own_chord_and_arranges_the_windows) {
    // WP-31's done-when, stated the way this file states everything: not
    // "a handler exists" but "the chord a reader is shown reaches a command
    // something answers, and the windows move". The three commands are
    // ckVision's; their handlers are Desktop's, installed on attach — which is
    // exactly the assumption Window List shipped wrong for a whole milestone,
    // so it is checked rather than trusted.
    Fixture fixture;
    fixture.client.new_terminal();
    fixture.client.new_terminal();  // three windows: a grid of 2 columns, 2 rows
    fixture.app.step(fixture.clock.now_nanos());

    std::set<std::string> chords;
    for (const std::string_view key : {ckv::ui::std_command_keys::kTileHorizontally,
                                       ckv::ui::std_command_keys::kTileVertically,
                                       ckv::ui::std_command_keys::kTileGrid}) {
        const ckm::client::KeyBinding* row = nullptr;
        for (const KeyBinding& binding : fixture.client.keymap().bindings())
            if (binding.key == key) row = &binding;
        CK_CHECK(row != nullptr);
        if (row == nullptr) continue;
        // Its own chord: three menu entries sharing one key would leave two of
        // them reachable only by mouse, which is not what the menu says.
        CK_CHECK(!row->chord.empty());
        CK_CHECK(chords.insert(row->chord).second);
        CK_CHECK(fixture.app.commands().has_handler(row->command));
        CK_CHECK(fixture.app.command_available(row->command));
        CK_CHECK(fixture.app.execute_command(row->command));
        fixture.app.step(fixture.clock.now_nanos());
    }

    // And each arrangement is the one its name promises, measured on the
    // windows themselves rather than on the fact that a handler ran.
    const ckv::Rect area = fixture.client.desktop().content_area();
    const auto windows = fixture.client.desktop().windows();
    CK_CHECK(windows.size() == 3U);

    CK_CHECK(fixture.app.execute_command(
        fixture.app.commands().standard().tile_horizontally));
    fixture.app.step(fixture.clock.now_nanos());
    // Full-HEIGHT bands side by side: the axis names what the windows are laid
    // out ALONG, so tiling horizontally puts them in a row across the desktop.
    // This is also the arrangement the unqualified `Tile` this package retired
    // has always produced.
    for (ckv::widgets::Window* window : windows) CK_CHECK(window->bounds().height == area.height);

    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().tile_vertically));
    fixture.app.step(fixture.clock.now_nanos());
    // Full-WIDTH bands stacked down the desktop — the transpose.
    for (ckv::widgets::Window* window : windows) CK_CHECK(window->bounds().width == area.width);

    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().tile_grid));
    fixture.app.step(fixture.clock.now_nanos());
    // Near-square: three windows are two above and one full-width below, so
    // neither dimension is any longer the whole content area for all of them.
    int full_width = 0;
    for (ckv::widgets::Window* window : windows) {
        CK_CHECK(window->bounds().height < area.height);
        if (window->bounds().width == area.width) ++full_width;
    }
    CK_CHECK(full_width == 1);
}

CK_TEST(window_list_opens_a_window_list) {
    Fixture fixture;
    fixture.client.new_terminal();  // two terminals, so the list has content
    const ckv::ui::CommandId window_list = fixture.app.commands().standard().window_list;
    CK_CHECK(fixture.app.commands().has_handler(window_list));
    CK_CHECK(fixture.app.command_available(window_list));
    const std::size_t before = fixture.client.desktop().windows().size();
    CK_CHECK(fixture.app.execute_command(window_list));
    fixture.app.step(fixture.clock.now_nanos());
    CK_CHECK(fixture.client.desktop().windows().size() == before + 1);
}

CK_TEST(the_window_list_shows_the_windows_and_not_an_empty_box) {
    // Reported from a running ckmux: the dialog opened and listed nothing.
    // "A window appeared" was all the test above checked, and a window with no
    // room in it appears perfectly well — it was five rows tall, a Close button
    // and not one entry. The cause was upstream (a list that reported no size
    // hints, so the dialog around it was sized as though it were empty), and
    // this is the ckmux-side statement of what a READER must see: the entries,
    // on the screen, inside the dialog.
    Fixture fixture;
    fixture.client.new_terminal();
    fixture.client.new_terminal();
    fixture.app.step(fixture.clock.now_nanos());
    const std::size_t terminals = fixture.client.desktop().windows().size();
    CK_CHECK(terminals >= 3U);

    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().window_list));
    fixture.app.step(fixture.clock.now_nanos());

    ckv::widgets::Window* dialog = nullptr;
    for (ckv::widgets::Window* window : fixture.client.desktop().windows())
        if (window->title().rfind("Terminal", 0) != 0) dialog = window;
    CK_CHECK(dialog != nullptr);
    if (dialog == nullptr) return;

    // Decoded, because the question is what reached the screen. A frame title
    // says "Terminal 2" too, so the count is taken INSIDE the dialog's own
    // rectangle: that is where the entries are, and nothing else is.
    ckv::term::VirtualDisplay display(Size{100, 30});
    (void)display.write(fixture.terminal.written_bytes());
    int rows_naming_a_terminal = 0;
    const ckv::Rect area = dialog->bounds();
    for (int y = area.y + 1; y < area.y + area.height - 1 && y < 30; ++y) {
        std::string row;
        for (int x = area.x + 1; x < area.x + area.width - 1 && x < 100; ++x) {
            const ckv::Cell cell = display.frame().at(ckv::Point{x, y});
            if (!cell.is_continuation()) row += cell.grapheme();
        }
        if (row.find("Terminal") != std::string::npos) ++rows_naming_a_terminal;
    }
    // One row per terminal, listed where a reader can read them.
    CK_CHECK(rows_naming_a_terminal >= static_cast<int>(terminals));
}

CK_TEST(the_print_output_window_shows_the_jobs_and_not_an_empty_box) {
    // The printer's version of the window-list defect above, and it shipped:
    // PRINT-1..6 landed 504 lines of dialog with all nine of its
    // `ClientOptions` seams assigned nowhere in src/, so a reader saw
    // `[ PRINT . 2 . 0 B ]` on the frame button and "Nothing has been captured
    // from this terminal." in the window, on one screen, at one moment. The
    // button was right because its path falls back to the emulator's own
    // scalars; the list had no fallback and reported nothing.
    //
    // `tools/check_seams.py` now guards the wiring. This guards the other
    // half: that when jobs ARE reported, they reach the screen. Two jobs with
    // DIFFERENT sizes deliberately -- a build rendering one hardcoded row
    // cannot pass, and neither can one that renders the same row twice.
    ClientOptions options = test_options();
    options.printer_jobs =
        [](const ckv::term::TerminalSubsession&) -> std::vector<ckm::proto::PrintJobInfo> {
        ckm::proto::PrintJobInfo first;
        first.job = 1;
        first.kind = ckm::proto::PrintJobKind::Controller;
        first.bytes = 5;   // "FIRST"
        first.lines = 1;
        ckm::proto::PrintJobInfo second;
        second.job = 2;
        second.kind = ckm::proto::PrintJobKind::Controller;
        second.bytes = 18;  // "CKMUX-SPOOL-MARKER"
        second.lines = 1;
        return {first, second};
    };

    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, std::move(options)};
    app.step(clock.now_nanos());

    // Resolved through the keymap, which is the single source of truth for
    // every surface that names a command (see the table test above). There is
    // no name-to-id lookup on the registry, and hard-coding an id would tie
    // this to a registration order nobody promised.
    ckv::ui::CommandId print_output = ckv::ui::kInvalidCommand;
    for (const KeyBinding& binding : client.keymap().bindings())
        if (binding.key == ckm::client::commands::kPrintOutput) print_output = binding.command;
    CK_CHECK(print_output != ckv::ui::kInvalidCommand);
    CK_CHECK(app.commands().has_handler(print_output));
    CK_CHECK(app.execute_command(print_output));
    app.step(clock.now_nanos());

    ckv::widgets::Window* dialog = nullptr;
    for (ckv::widgets::Window* window : client.desktop().windows())
        if (window->title().rfind("Terminal", 0) != 0) dialog = window;
    CK_CHECK(dialog != nullptr);
    if (dialog == nullptr) return;

    // Decoded, because the question is what reached the screen -- and read
    // INSIDE the dialog's own rectangle, since that is where the rows are.
    ckv::term::VirtualDisplay display(Size{100, 30});
    (void)display.write(terminal.written_bytes());
    const ckv::Rect area = dialog->bounds();
    std::string inside;
    for (int y = area.y + 1; y < area.y + area.height - 1 && y < 30; ++y) {
        for (int x = area.x + 1; x < area.x + area.width - 1 && x < 100; ++x) {
            const ckv::Cell cell = display.frame().at(ckv::Point{x, y});
            if (!cell.is_continuation()) inside += cell.grapheme();
        }
        inside += '\n';
    }

    // The sentence that was the whole defect: it must not appear while two
    // jobs are held. Paired with the positives below, because on its own an
    // absence assertion passes as soon as the wording changes.
    CK_CHECK(inside.find("Nothing has been captured") == std::string::npos);

    // Both jobs, told apart by their sizes rather than merely counted. The
    // byte counts come from the one shared formatter (the interface spec), so this
    // cannot drift from what the button and the job list show elsewhere.
    CK_CHECK(inside.find(ckm::client::format_bytes(5)) != std::string::npos);
    CK_CHECK(inside.find(ckm::client::format_bytes(18)) != std::string::npos);

    // And in the reader's words, not the standard's: `CSI 5 i` is called
    // "Controller" in ECMA-48 and means nothing to somebody looking at a list
    // of their own captures.
    std::size_t rows_naming_a_capture = 0;
    for (std::size_t at = inside.find("Printed by the program"); at != std::string::npos;
         at = inside.find("Printed by the program", at + 1))
        ++rows_naming_a_capture;
    CK_CHECK(rows_naming_a_capture >= 2);
}

CK_TEST(terminal_report_shows_the_hosts_evidence_with_the_decoded_count) {
    // Help ▸ Terminal Report (the ckVision integration spec L-56): ckVision's dialog over the
    // OUTER host's capabilities, with ckmux's claimed handler adding the
    // decoded-SGR-reports line only a host can count. The check is the
    // reader's: the words, on the screen, inside the dialog's rectangle.
    ClientOptions options = test_options();
    options.mouse_reports_probe = [] { return std::size_t{412}; };
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, std::move(options)};

    const ckv::ui::CommandId report = app.commands().standard().terminal_report;
    CK_CHECK(app.commands().has_handler(report));
    CK_CHECK(app.command_available(report));
    CK_CHECK(app.execute_command(report));
    app.step(clock.now_nanos());

    ckv::widgets::Window* dialog = nullptr;
    for (ckv::widgets::Window* window : client.desktop().windows())
        if (window->title() == "Terminal report") dialog = window;
    CK_CHECK(dialog != nullptr);
    if (dialog == nullptr) return;

    ckv::term::VirtualDisplay display(Size{100, 30});
    (void)display.write(terminal.written_bytes());
    bool decoded_line = false;
    bool capability_row = false;
    const ckv::Rect area = dialog->bounds();
    for (int y = area.y + 1; y < area.y + area.height - 1 && y < 30; ++y) {
        std::string row;
        for (int x = area.x + 1; x < area.x + area.width - 1 && x < 100; ++x) {
            const ckv::Cell cell = display.frame().at(ckv::Point{x, y});
            if (!cell.is_continuation()) row += cell.grapheme();
        }
        if (row.find("Mouse reports decoded") != std::string::npos &&
            row.find("412") != std::string::npos)
            decoded_line = true;
        // The first capability entries sit at the top of the table, so they
        // are on screen however small the desktop clamps the dialog.
        if (row.find("Cell grid") != std::string::npos) capability_row = true;
    }
    CK_CHECK(decoded_line);
    CK_CHECK(capability_row);
}

#endif  // !_WIN32
