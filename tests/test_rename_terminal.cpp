// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Naming a terminal: the reader's own caption, and what happens to the
// program's underneath it (the interface spec "Window captions follow the program").
//
// The point every one of these turns on is that a custom title is an
// OVERRIDE, not one more writer of the same string. The child goes on
// renaming itself while a reader's name is pinned over it, so handing the
// name back has a CURRENT title to hand it back to rather than whatever the
// caption happened to say at the moment it was pinned. That is the whole
// difference, and it is invisible in any test that renames a terminal whose
// program never speaks again.
//
// Driven headlessly the way main.cpp drives the client: real Application,
// real menus, real mouse reports, real OSC bytes fed at the seam the PTY
// writes into. The child is /bin/cat, which stays alive and says nothing.
#if !defined(_WIN32)

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "client/client_app.hpp"
#include "common/proto.hpp"
#include "cvision/core/text.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/label.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/terminal_view.hpp"
#include "cvision/widgets/window_switcher_bar.hpp"

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;

namespace {

ClientOptions test_options() {
    ClientOptions options;
    options.settings.shell = "/bin/cat";
    return options;
}

struct Fixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, test_options()};
};

struct ConfiguredFixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client;

    explicit ConfiguredFixture(ClientOptions options) : client{app, std::move(options)} {}
};

ckv::ui::CommandId id_of(Application& app, std::string_view key) {
    return app.commands().id_for(key).value_or(ckv::ui::kInvalidCommand);
}

ckv::widgets::TerminalView* view_of(ckv::widgets::Window* window) {
    return window == nullptr ? nullptr
                             : dynamic_cast<ckv::widgets::TerminalView*>(window->content());
}

// One turn of the title poll. The caption is polled rather than pushed — the
// emulator records a title change and raises no event — so a test that only
// stepped would be asking before anything had read.
template <typename FixtureType>
void poll_titles(FixtureType& f) {
    f.clock.advance(200'000'000);
    f.app.step(f.clock.now_nanos());
}

// What a program in this terminal claims to be, said the way a program says
// it: OSC 2, fed at the seam the PTY writes into.
template <typename FixtureType>
void child_claims(FixtureType& f, ckv::widgets::Window* window, const std::string& title) {
    ckv::widgets::TerminalView* const view = view_of(window);
    if (view == nullptr) return;
    view->session().feed_output("\x1b]2;" + title + "\a");
    poll_titles(f);
}

// Types `text` into whatever holds the keyboard, one character at a time, the
// way a reader does.
template <typename FixtureType>
void type(FixtureType& f, std::string_view text) {
    for (const char letter : text)
        f.app.dispatch(
            ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, std::string(1, letter)}});
}

template <typename FixtureType>
void press_enter(FixtureType& f) {
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    f.app.step(0);
    f.app.step(0);
}

// Clears the name field and types a new one. Select-all then type would be the
// reader's other route; this one needs no assumption about which shortcuts the
// input line binds.
template <typename FixtureType>
void retype_name(FixtureType& f, std::string_view name) {
    for (int i = 0; i < 80; ++i)
        f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Backspace, ckv::Modifier::None, ""}});
    for (int i = 0; i < 80; ++i)
        f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Delete, ckv::Modifier::None, ""}});
    type(f, name);
}

// Every Label in a view tree, in the order the tree holds them. A dialog's
// Note field materializes as one Label of one row (ckVision's
// `materialize_dialog`), so this is how a test reads the sentences a dialog
// shows without scraping painted cells.
void collect_labels(ckv::ui::View* view, std::vector<std::string>& into) {
    if (view == nullptr) return;
    if (auto* const label = dynamic_cast<ckv::widgets::Label*>(view)) into.push_back(label->text());
    for (const auto& child : view->children()) collect_labels(child.get(), into);
}

std::vector<std::string> labels_in(ckv::widgets::Window* window) {
    std::vector<std::string> found;
    collect_labels(window, found);
    return found;
}

ckv::widgets::DropdownMenu* dropped_menu(ClientApp& client) {
    for (ckv::ui::View* popup : client.desktop().popups())
        if (auto* const menu = dynamic_cast<ckv::widgets::DropdownMenu*>(popup)) return menu;
    return nullptr;
}

std::optional<ckv::Point> switcher_cell(ClientApp& client, std::size_t index) {
    const ckv::Rect where = client.window_switcher().absolute_bounds();
    for (const ckv::widgets::WindowSwitcherBar::DrawnEntry& drawn :
         client.window_switcher().drawn_entries())
        if (drawn.index == index) return ckv::Point{where.x + drawn.x + drawn.width / 2, where.y};
    return std::nullopt;
}

template <typename FixtureType>
bool switcher_right_press(FixtureType& f, std::size_t index) {
    const std::optional<ckv::Point> cell = switcher_cell(f.client, index);
    if (!cell) return false;
    (void)f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Right, *cell,
                                         std::nullopt, ckv::Modifier::None});
    f.app.step(0);
    return true;
}

}  // namespace

// --- The override ---------------------------------------------------------

CK_TEST(a_name_the_reader_gives_a_terminal_outranks_what_its_program_asks_for) {
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    CK_CHECK(window->title() == "Terminal 1");

    // The ordinary case first, so this test would notice if captions had
    // simply stopped following programs at all.
    child_claims(f, window, "vim: architecture.md");
    CK_CHECK(window->title() == "vim: architecture.md");

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    CK_CHECK(f.app.is_modal());
    retype_name(f, "notes");
    press_enter(f);
    CK_CHECK(!f.app.is_modal());
    CK_CHECK(window->title() == "notes");

    // And now the thing the whole feature is: the program goes on renaming
    // itself and the reader's name stays. Polled several times over, because
    // the caption is re-derived on a timer and a rule that only held for one
    // tick would look identical at the first assertion.
    child_claims(f, window, "vim: 05-protocol.md");
    CK_CHECK(window->title() == "notes");
    child_claims(f, window, "make -j8");
    poll_titles(f);
    poll_titles(f);
    CK_CHECK(window->title() == "notes");
}

CK_TEST(use_default_title_hands_the_caption_back_to_what_the_program_says_now) {
    // The half a naive implementation gets wrong. If the override merely
    // overwrote the caption, "use the default title" would have to restore the
    // title from the moment the reader pinned it — a name the program stopped
    // using long ago. What is restored is the CURRENT one, because the child's
    // own title was being recorded underneath the whole time.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];

    child_claims(f, window, "vim: architecture.md");
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    retype_name(f, "notes");
    press_enter(f);
    CK_CHECK(window->title() == "notes");

    // The program moves on, twice, while the reader's name is up.
    child_claims(f, window, "vim: 05-protocol.md");
    child_claims(f, window, "make -j8");
    CK_CHECK(window->title() == "notes");

    // Use Default Title, reached by its own mnemonic.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    CK_CHECK(f.app.is_modal());
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Alt, "d"}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(!f.app.is_modal());
    CK_CHECK(window->title() == "make -j8");

    // Following the program again, which is the other half of "default".
    child_claims(f, window, "less README.md");
    CK_CHECK(window->title() == "less README.md");
}

CK_TEST(a_pinned_terminal_whose_program_hands_its_own_title_back_keeps_the_readers_name) {
    // An empty OSC 2 is how a program says it is done naming the window. That
    // reaches the layer UNDER the override, so the caption a reader chose is
    // untouched — and is what they still see when the program has gone quiet.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];

    child_claims(f, window, "vim");
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    retype_name(f, "editor");
    press_enter(f);

    child_claims(f, window, "");
    CK_CHECK(window->title() == "editor");

    // And underneath, the default is back to ckmux's own name — which is what
    // Use Default Title now restores.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Alt, "d"}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(window->title() == "Terminal 1");
}

CK_TEST(an_empty_name_asks_for_the_default_title_rather_than_being_refused) {
    // A reader who clears the field has said what they want as plainly as one
    // who pressed the button for it. Refusing it — which is what the SESSION
    // rename does, because a session with no name is a row in the picker with
    // nothing to point at — would leave the caption pinned to a name they had
    // just deleted.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    child_claims(f, window, "htop");

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    retype_name(f, "watching");
    press_enter(f);
    CK_CHECK(window->title() == "watching");

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    retype_name(f, "");
    press_enter(f);
    CK_CHECK(window->title() == "htop");
}

CK_TEST(the_rename_prompt_starts_from_the_caption_the_reader_can_see) {
    // Adjusting a name that is nearly right is the commonest rename there is,
    // and a field that started empty would make it retyping.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    child_claims(f, window, "build");

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    // Typed onto the end of what is already there, and accepted: the result
    // proves what the field held without reading the widget.
    type(f, " 2");
    press_enter(f);
    CK_CHECK(window->title() == "build 2");
}

CK_TEST(cancelling_the_rename_leaves_the_caption_exactly_as_it_was) {
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    child_claims(f, window, "vim");

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    retype_name(f, "something else");
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(!f.app.is_modal());
    CK_CHECK(window->title() == "vim");
    // Still following the program, because nothing was pinned.
    child_claims(f, window, "less");
    CK_CHECK(window->title() == "less");
}

// --- Where it is reached from ---------------------------------------------

CK_TEST(each_terminal_carries_its_own_name_and_only_its_own) {
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];
    child_claims(f, first, "one");
    child_claims(f, second, "two");

    // The command names the ACTIVE terminal, which is the second.
    CK_CHECK(f.client.desktop().active_window() == second);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    retype_name(f, "named");
    press_enter(f);
    CK_CHECK(second->title() == "named");
    CK_CHECK(first->title() == "one");

    // And the one that was not named goes on following its program.
    child_claims(f, first, "one, later");
    child_claims(f, second, "two, later");
    CK_CHECK(first->title() == "one, later");
    CK_CHECK(second->title() == "named");
}

CK_TEST(renaming_from_the_window_bar_names_the_row_clicked_not_the_one_in_front) {
    // The correctness point the whole context menu exists for: every entry is
    // bound to the window whose row it is, because command dispatch reads
    // active_window() and would name the terminal the reader did not point at.
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];
    CK_CHECK(f.client.desktop().active_window() == second);

    CK_CHECK(switcher_right_press(f, 0));
    ckv::widgets::DropdownMenu* const menu = dropped_menu(f.client);
    CK_CHECK(menu != nullptr);
    if (menu == nullptr) return;
    // Nine since WP-34 put Minimize at the top of this menu.
    CK_CHECK(menu->items().size() == 9U);
    if (menu->items().size() != 9U) return;
    CK_CHECK(menu->items()[4].label() == "Re&name…");
    menu->items()[4].action()();
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(f.app.is_modal());
    retype_name(f, "background");
    press_enter(f);

    CK_CHECK(first->title() == "background");
    CK_CHECK(second->title() == "Terminal 2");
    // And the reader was not taken to the window they were only pointing at.
    CK_CHECK(f.client.desktop().active_window() == second);
}

CK_TEST(the_terminal_menu_offers_the_rename_and_says_which_key_reaches_it) {
    Fixture f;
    f.app.step(0);
    auto* const bar = dynamic_cast<ckv::widgets::MenuBar*>(f.client.desktop().top_dock());
    CK_CHECK(bar != nullptr);
    if (bar == nullptr) return;

    const ckv::ui::CommandId rename = id_of(f.app, ckm::client::commands::kRenameTerminal);
    CK_CHECK(rename != ckv::ui::kInvalidCommand);
    bool found = false;
    for (const ckv::widgets::MenuBarItem& menu : bar->menus()) {
        if (menu.label != "&Terminal") continue;
        for (const ckv::widgets::MenuItem& entry : menu.items) {
            if (entry.command() != rename) continue;
            found = true;
            // Rule 2 of the interface spec: every menu entry shows the key that reaches
            // it from a terminal, and this one has one.
            CK_CHECK(entry.presentation().chord == "^B ,");
        }
    }
    CK_CHECK(found);
}

CK_TEST(with_no_terminal_to_name_the_rename_is_plainly_unavailable) {
    // An empty desktop is a state a reader can reach — close the last window —
    // and a menu item that opened a dialog onto nothing would be worse than
    // one that is visibly not available.
    Fixture f;
    f.app.step(0);
    const ckv::ui::CommandId rename = id_of(f.app, ckm::client::commands::kRenameTerminal);
    CK_CHECK(f.app.commands().is_enabled(rename));

    // Closed the way the reader closes it: the window's own close box, then
    // the confirmation the client puts in front of a live child. With no
    // server, accepting that ends the terminal here and the window goes.
    f.client.desktop().windows()[0]->close();
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(f.app.is_modal());
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
    for (int turn = 0; turn < 6; ++turn) f.app.step(0);
    CK_CHECK(f.client.desktop().windows().empty());
    CK_CHECK(!f.app.commands().is_enabled(rename));
}

// --- The session half -----------------------------------------------------

CK_TEST(an_attached_client_asks_the_server_to_name_the_terminal) {
    // A custom title is session state (the session model): it has to survive a detach
    // and reach the other client watching the same session, so an attached
    // client asks rather than deciding. The hook stands in for the server.
    ClientOptions options = test_options();
    int asked = 0;
    std::string asked_name = "unset";
    ckv::term::TerminalSubsession* asked_terminal = nullptr;
    options.rename_terminal = [&](ckv::term::TerminalSubsession& terminal,
                                  const std::string& name) {
        ++asked;
        asked_name = name;
        asked_terminal = &terminal;
    };
    ConfiguredFixture f(std::move(options));
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    retype_name(f, "deploy");
    press_enter(f);
    CK_CHECK(asked == 1);
    CK_CHECK(asked_name == "deploy");
    CK_CHECK(asked_terminal == &view_of(window)->session());
    // Shown at once as well as asked for. A caption that waited a round trip
    // would leave the reader wondering whether the command worked.
    CK_CHECK(window->title() == "deploy");

    // Handing it back is the same seam with an empty name, not a second one.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    f.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Alt, "d"}});
    f.app.step(0);
    f.app.step(0);
    CK_CHECK(asked == 2);
    CK_CHECK(asked_name.empty());
}

CK_TEST(the_name_the_session_states_is_the_one_shown_even_without_a_rename_here) {
    // The reattach case, and the second-client case: this client never renamed
    // anything, and the name still has to appear over whatever the program in
    // that terminal is calling itself.
    ClientOptions options = test_options();
    std::string from_the_server;
    options.custom_title = [&](const ckv::term::TerminalSubsession&) { return from_the_server; };
    options.rename_terminal = [](ckv::term::TerminalSubsession&, const std::string&) {};
    ConfiguredFixture f(std::move(options));
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];

    child_claims(f, window, "vim");
    CK_CHECK(window->title() == "vim");

    // Somebody else named it — another client, or this reader before they
    // detached — and the server says so.
    from_the_server = "the important one";
    poll_titles(f);
    CK_CHECK(window->title() == "the important one");

    // The program goes on renaming itself and is still not shown.
    child_claims(f, window, "make");
    CK_CHECK(window->title() == "the important one");

    // And the server saying the name is gone hands the caption back to the
    // program's current title, not to a stale one.
    from_the_server.clear();
    poll_titles(f);
    CK_CHECK(window->title() == "make");
}

CK_TEST(a_caption_too_long_to_quote_is_elided_and_the_sentence_around_it_survives) {
    // A child's title is a child's string, and this dialog quotes it back —
    // the second place ckmux prints one somewhere other than a window frame,
    // where the caption is already length-bounded (the architecture spec security posture).
    //
    // What goes wrong is not geometry: a dialog is clamped to the desktop
    // whatever it holds, and a Note is one Label of one row. So a note that
    // quoted four kilobytes would be CLIPPED, and the reader would see the
    // opening quote, a wall of the child's text, and none of the sentence that
    // says what the button does. The quotation is elided so the sentence
    // around it survives.
    Fixture f;
    f.app.step(0);
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    child_claims(f, window, std::string(2000, 'x'));

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    CK_CHECK(f.app.is_modal());

    // The dialog is the window on this desktop that is not a terminal's.
    ckv::widgets::Window* dialog = nullptr;
    for (ckv::widgets::Window* candidate : f.client.desktop().windows())
        if (view_of(candidate) == nullptr) dialog = candidate;
    CK_CHECK(dialog != nullptr);
    if (dialog == nullptr) return;
    // Clamped to the desktop either way — stated so this test says which of
    // the two possible failures it is about.
    CK_CHECK(dialog->bounds().width <= f.client.desktop().bounds().width);

    // Every line of the explanation fits the dialog, so none of it is clipped
    // away — including the line that says what pressing the button does, which
    // is the one an unbounded quotation would have pushed off the end.
    const std::vector<std::string> notes = labels_in(dialog);
    std::string quoting;
    bool says_what_the_button_does = false;
    for (const std::string& note : notes) {
        CK_CHECK(ckv::text::text_width(note) <= dialog->bounds().width);
        if (note.find("Use Default Title goes back to") != std::string::npos)
            says_what_the_button_does = true;
        if (note.find("xxx") != std::string::npos) quoting = note;
    }
    CK_CHECK(says_what_the_button_does);
    // The child's own text is quoted, and only a bounded amount of it.
    CK_CHECK(!quoting.empty());
    CK_CHECK(quoting.find(std::string(200, 'x')) == std::string::npos);
}

// --- The window bar -------------------------------------------------------

CK_TEST(the_window_bar_does_not_re_size_a_button_for_every_caption_a_program_writes) {
    // The reader-facing half of ckVision's U4-m, wired here: a shell rewrites
    // its caption at every prompt, and an undamped bar re-sizes that button
    // and slides every button after it each time.
    //
    // A fast title poll so that sixty rewrites take one second of the reader's
    // time rather than twelve — the damping is measured on the same clock, and
    // a test that spent twelve virtual seconds would be allowing twelve steps
    // and proving very little.
    ClientOptions options = test_options();
    options.title_poll_nanos = 10'000'000;
    ConfiguredFixture f(std::move(options));
    f.app.step(0);
    // Two terminals, because the bar is on screen exactly while there is more
    // than one.
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    ckv::widgets::Window* const first = f.client.desktop().windows()[0];
    CK_CHECK(f.client.window_switcher().visible());

    const auto button_width = [&]() -> int {
        for (const ckv::widgets::WindowSwitcherBar::DrawnEntry& drawn :
             f.client.window_switcher().drawn_entries())
            if (drawn.index == 0) return drawn.width;
        return -1;
    };

    // Sixty rewrites of visibly different lengths, twenty milliseconds apart:
    // 1.2 seconds, which the default grow delay of one second divides into at
    // most two steps. Undamped this is sixty.
    int previous = button_width();
    int steps = 0;
    ckv::widgets::TerminalView* const view = view_of(first);
    CK_CHECK(view != nullptr);
    if (view == nullptr) return;
    for (int frame = 1; frame <= 60; ++frame) {
        view->session().feed_output("\x1b]2;target " + std::to_string(frame) + " of 300" +
                                    std::string(static_cast<std::size_t>(frame % 17), '.') + "\a");
        f.clock.advance(20'000'000);
        f.app.step(f.clock.now_nanos());
        const int now = button_width();
        if (now != previous) ++steps;
        previous = now;
    }
    CK_CHECK(steps <= 2);
    // The positive partner: the LABEL is current the whole time, and it is
    // only the box that was held. A bar that had stopped following captions
    // altogether would satisfy the count above and fail here.
    CK_CHECK(f.client.window_switcher().entries()[0].label == first->title());
    CK_CHECK(first->title() != "Terminal 1");
}

CK_TEST(a_name_the_reader_gives_re_sizes_its_button_without_waiting) {
    // Damping absorbs what a PROGRAM does to a caption. A reader who has just
    // renamed the window is not flicker, and a rename that visibly took effect
    // half a minute later reads as a command that did not work.
    Fixture f;
    f.app.step(0);
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal)));
    f.app.step(0);
    ckv::widgets::Window* const second = f.client.desktop().windows()[1];
    child_claims(f, second, "a caption of a fairly generous length");

    const auto button_width = [&](std::size_t index) -> int {
        for (const ckv::widgets::WindowSwitcherBar::DrawnEntry& drawn :
             f.client.window_switcher().drawn_entries())
            if (drawn.index == index) return drawn.width;
        return -1;
    };
    const int wide = button_width(1);

    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kRenameTerminal)));
    f.app.step(0);
    retype_name(f, "x");
    press_enter(f);
    CK_CHECK(second->title() == "x");
    // Narrower at once, although the shrink delay is half a minute away.
    CK_CHECK(button_width(1) < wide);
}

#endif
