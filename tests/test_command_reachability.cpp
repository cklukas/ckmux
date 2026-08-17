// Copyright (c) 2026 C. Klukas. All rights reserved.
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
#include <string>

#include "client/client_app.hpp"
#include "client/commands.hpp"
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
// (plans/10-roadmap.md "Deliberately not yet true").
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
            const ckv::Cell& cell = display.frame().at(ckv::Point{x, y});
            if (!cell.is_continuation()) row += cell.grapheme();
        }
        if (row.find("Terminal") != std::string::npos) ++rows_naming_a_terminal;
    }
    // One row per terminal, listed where a reader can read them.
    CK_CHECK(rows_naming_a_terminal >= static_cast<int>(terminals));
}

#endif  // !_WIN32
