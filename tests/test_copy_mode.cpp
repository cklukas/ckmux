// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Copy mode (the interface spec, WP-17). Two layers, deliberately: the
// text algebra — what a selection of a given shape yields, and where a search
// lands — is tested against a document with no widget anywhere near it, and
// the mode itself is driven through real key dispatch into a real
// `Application`, because "the reader can select and copy" is a claim about
// keys, not about a handler.
#include "client/copy_mode.hpp"

#if !defined(_WIN32)

#include <string>
#include <vector>

#include "client/client_app.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/term/terminal_emulator.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/ui/application.hpp"
#include "cvision/widgets/terminal_view.hpp"

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckm::client::compose_history;
using ckm::client::CopyLine;
using ckm::client::find_match;
using ckm::client::SelectionMode;
using ckm::client::selected_text;
using ckv::ManualClock;
using ckv::Point;
using ckv::Size;
using ckv::ui::Application;

namespace {

// A document written the way it reads, one string per line, padded to a fixed
// width the way a terminal grid pads one.
std::vector<CopyLine> document(const std::vector<std::string>& rows, int width) {
    std::vector<CopyLine> lines;
    for (const std::string& row : rows) {
        CopyLine line;
        for (const char c : row) line.push_back(std::string(1, c));
        while (static_cast<int>(line.size()) < width) line.push_back(" ");
        lines.push_back(std::move(line));
    }
    return lines;
}

// A command's id, looked up the way everything durable does: by key. Ids are
// assigned by the registry at runtime (ckVision D-013), so a test cannot hold
// one as a constant — it asks the registry the application actually declared
// into.
ckv::ui::CommandId id_of(Application& app, std::string_view key) {
    return app.commands().id_for(key).value_or(ckv::ui::kInvalidCommand);
}

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

    bool press(ckv::KeyChord chord) { return app.dispatch(ckv::KeyEvent{std::move(chord)}); }
    bool press_char(const std::string& text) {
        return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, text});
    }
    bool press_prefix() { return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "b"}); }
    void settle() { app.step(clock.now_nanos()); }
    // Enters copy mode the way a reader does, through the prefix.
    void enter_copy_mode() {
        press_prefix();
        press_char("[");
        settle();
    }
};

}  // namespace

// --- the text algebra ----------------------------------------------------

CK_TEST(the_history_is_the_scrollback_followed_by_the_screen) {
    ckv::term::TerminalCapabilityProfile profile = ckv::term::embedded_xterm_sixel_profile();
    profile.cells = Size{4, 2};
    ckv::term::TerminalSubsessionOptions options;
    options.max_scrollback_lines = 8;
    ckv::term::TerminalEmulator emulator(profile, options);
    emulator.feed_output("one\r\ntwo\r\nsix\r\nten");

    const std::vector<CopyLine> lines = compose_history(emulator.snapshot());
    CK_CHECK(lines.size() == 4U);  // two scrolled off, two on screen
    CK_CHECK(selected_text(lines, SelectionMode::Line, Point{0, 0}, Point{0, 3}) ==
             "one\ntwo\nsix\nten");
}

CK_TEST(a_character_selection_takes_the_cursor_cell_with_it) {
    // Marking one character must yield one character; an exclusive end is how
    // a selection comes out one short of what the reader highlighted.
    const std::vector<CopyLine> lines = document({"hello world"}, 16);
    CK_CHECK(selected_text(lines, SelectionMode::Character, Point{0, 0}, Point{0, 0}) == "h");
    CK_CHECK(selected_text(lines, SelectionMode::Character, Point{0, 0}, Point{4, 0}) == "hello");
    // Backwards is the same selection: which corner the reader started from is
    // not part of what they selected.
    CK_CHECK(selected_text(lines, SelectionMode::Character, Point{4, 0}, Point{0, 0}) == "hello");
}

CK_TEST(a_multi_line_selection_keeps_whole_middle_lines_and_drops_padding) {
    const std::vector<CopyLine> lines = document({"first line", "middle", "last line"}, 16);
    CK_CHECK(selected_text(lines, SelectionMode::Character, Point{6, 0}, Point{3, 2}) ==
             "line\nmiddle\nlast");
    // The blanks a grid pads a row with were never typed, so they are not
    // copied — a clipboard full of trailing spaces is one nobody can paste.
    CK_CHECK(selected_text(lines, SelectionMode::Line, Point{0, 1}, Point{0, 1}) == "middle");
}

CK_TEST(a_rectangular_selection_takes_a_column_out_of_a_table) {
    // The shape line-wise copying ruins: three rows of a table, and the
    // reader wants the second column and nothing else.
    const std::vector<CopyLine> lines = document({"aa BB cc", "dd EE ff", "gg HH ii"}, 8);
    CK_CHECK(selected_text(lines, SelectionMode::Rectangular, Point{3, 0}, Point{4, 2}) ==
             "BB\nEE\nHH");
    // Drawn from the opposite corner it is the same block.
    CK_CHECK(selected_text(lines, SelectionMode::Rectangular, Point{4, 2}, Point{3, 0}) ==
             "BB\nEE\nHH");
}

CK_TEST(nothing_marked_yields_nothing_rather_than_everything) {
    const std::vector<CopyLine> lines = document({"one", "two"}, 8);
    CK_CHECK(selected_text(lines, SelectionMode::None, Point{0, 0}, Point{2, 1}).empty());
}

CK_TEST(search_walks_forwards_and_backwards_and_wraps) {
    const std::vector<CopyLine> lines = document({"alpha", "beta", "gamma", "beta again"}, 12);
    const std::optional<Point> first = find_match(lines, "beta", Point{0, 0}, true);
    CK_CHECK(first.has_value());
    if (first) CK_CHECK(first->y == 1);
    const std::optional<Point> second = find_match(lines, "beta", *first, true);
    CK_CHECK(second.has_value());
    if (second) CK_CHECK(second->y == 3);
    // Past the last match it comes round again rather than stopping at the
    // bottom: a history is a loop to a reader searching it.
    const std::optional<Point> wrapped = find_match(lines, "beta", *second, true);
    CK_CHECK(wrapped.has_value());
    if (wrapped) CK_CHECK(wrapped->y == 1);
    const std::optional<Point> backwards = find_match(lines, "alpha", Point{0, 2}, false);
    CK_CHECK(backwards.has_value());
    if (backwards) CK_CHECK(backwards->y == 0);
    CK_CHECK(!find_match(lines, "nothing here", Point{0, 0}, true).has_value());
    CK_CHECK(!find_match(lines, "", Point{0, 0}, true).has_value());
}

CK_TEST(a_repeated_search_moves_along_the_line_it_is_on) {
    // `from` is exclusive, and that has to hold within a line as much as
    // between lines: two matches on one line cost a reader pressing `n` a whole
    // lap of the history before the second one came round.
    const std::vector<CopyLine> lines = document({"one two one"}, 16);
    const std::optional<Point> first = find_match(lines, "one", Point{0, 0}, true);
    CK_CHECK(first.has_value());
    if (first) {
        CK_CHECK(first->y == 0);
        CK_CHECK(first->x == 8);
    }
    // And round again to the one it started past, rather than "not found".
    const std::optional<Point> second = find_match(lines, "one", Point{8, 0}, true);
    CK_CHECK(second.has_value());
    if (second) CK_CHECK(second->x == 0);
    const std::optional<Point> backwards = find_match(lines, "one", Point{8, 0}, false);
    CK_CHECK(backwards.has_value());
    if (backwards) CK_CHECK(backwards->x == 0);
}

CK_TEST(a_search_lands_on_a_column_rather_than_on_a_byte) {
    // The cursor is indexed by column, and a byte offset is the same number
    // only while every character is one byte wide. With a box-drawing character
    // in front of it the cursor landed inside that character instead — and a
    // selection started there copied half of one.
    std::vector<CopyLine> lines;
    CopyLine line;
    line.push_back("│");  // three bytes, one column
    line.push_back("é");  // two bytes, one column
    for (const char c : std::string("found")) line.push_back(std::string(1, c));
    lines.push_back(std::move(line));

    const std::optional<Point> hit = find_match(lines, "found", Point{0, 0}, true);
    CK_CHECK(hit.has_value());
    if (hit) {
        CK_CHECK(hit->y == 0);
        CK_CHECK(hit->x == 2);
    }
}

CK_TEST(a_search_from_outside_the_document_answers_rather_than_reads_past_it) {
    // `find_match` is a public function over a document its caller composed, so
    // a stale or invented cursor has to be an answer: indexing the line vector
    // with it was a crash bought by a point nobody had clamped.
    const std::vector<CopyLine> lines = document({"alpha", "beta"}, 8);
    CK_CHECK(find_match(lines, "beta", Point{0, 99}, true).has_value());
    CK_CHECK(find_match(lines, "alpha", Point{0, -7}, false).has_value());
    CK_CHECK(find_match(lines, "beta", Point{500, 0}, true).has_value());
    CK_CHECK(!find_match(lines, "gamma", Point{0, -99}, true).has_value());
}

CK_TEST(a_terminal_nothing_has_sized_yet_composes_one_blank_line) {
    // No width is no document. Slicing the buffers at a made-up width of one
    // would open copy mode on a column of single letters as tall as the
    // history.
    const ckv::core::TerminalSnapshot never_sized;
    const std::vector<CopyLine> lines = compose_history(never_sized);
    CK_CHECK(lines.size() == 1U);
}

CK_TEST(a_lower_case_search_does_not_care_about_case_and_a_capital_does) {
    const std::vector<CopyLine> lines = document({"Error: nothing", "error: something"}, 20);
    // Smartcase: all lower case means "I do not care", so the first hit is
    // the capitalised one on the line above.
    const std::optional<Point> insensitive = find_match(lines, "error", Point{0, 1}, true);
    CK_CHECK(insensitive.has_value());
    if (insensitive) CK_CHECK(insensitive->y == 0);
    // A capital in the query means the reader typed one on purpose.
    const std::optional<Point> sensitive = find_match(lines, "Error", Point{0, 0}, true);
    CK_CHECK(sensitive.has_value());
    if (sensitive) CK_CHECK(sensitive->y == 0);
}

// --- the mode, driven through real keys ----------------------------------

CK_TEST(the_prefix_key_opens_copy_mode_over_the_focused_terminal) {
    Fixture f;
    f.settle();
    CK_CHECK(f.client.copy_mode() == nullptr);
    f.enter_copy_mode();
    CK_CHECK(f.client.copy_mode() != nullptr);
    // The caption says which mode the window is in and how far back the
    // reader has scrolled — the badge of the interface spec.
    bool badged = false;
    for (ckv::widgets::Window* window : f.client.desktop().windows())
        if (window->title().find("COPY") != std::string::npos) badged = true;
    CK_CHECK(badged);
    // ...and the footer stops advertising prefix keys that are unreachable
    // from here, and says what copy mode's own keys do.
    bool says_copy = false;
    for (const std::string& label : f.client.footer_labels())
        if (label.find("y copy") != std::string::npos) says_copy = true;
    CK_CHECK(says_copy);
}

CK_TEST(q_leaves_copy_mode_and_gives_the_caption_back) {
    Fixture f;
    f.settle();
    f.enter_copy_mode();
    CK_CHECK(f.client.copy_mode() != nullptr);
    f.press_char("q");
    f.settle();
    CK_CHECK(f.client.copy_mode() == nullptr);
    for (ckv::widgets::Window* window : f.client.desktop().windows())
        CK_CHECK(window->title().find("COPY") == std::string::npos);
    // The keyboard goes back to the program, which is the state a reader who
    // pressed `q` expects to be in.
    CK_CHECK(f.client.context() == ckm::client::Context::Terminal);
}

CK_TEST(vi_keys_move_the_cursor_and_the_arrows_do_the_same_thing) {
    Fixture f;
    f.settle();
    f.enter_copy_mode();
    ckm::client::CopyModeView* const copy = f.client.copy_mode();
    CK_CHECK(copy != nullptr);
    if (copy == nullptr) return;
    const Point start = copy->cursor();
    f.press_char("k");
    CK_CHECK(copy->cursor().y == start.y - 1);
    f.press(ckv::KeyChord{ckv::Key::Down, ckv::Modifier::None, ""});
    CK_CHECK(copy->cursor().y == start.y);
    f.press_char("l");
    CK_CHECK(copy->cursor().x == 1);
    f.press(ckv::KeyChord{ckv::Key::Left, ckv::Modifier::None, ""});
    CK_CHECK(copy->cursor().x == 0);
    // `g` and `G` are the ends of the document.
    f.press_char("g");
    CK_CHECK(copy->cursor().y == 0);
    f.press_char("G");
    CK_CHECK(copy->cursor().y == copy->history_size() - 1);
}

CK_TEST(v_marks_a_selection_and_pressing_it_again_takes_it_back) {
    Fixture f;
    f.settle();
    f.enter_copy_mode();
    ckm::client::CopyModeView* const copy = f.client.copy_mode();
    CK_CHECK(copy != nullptr);
    if (copy == nullptr) return;
    CK_CHECK(copy->selection_mode() == SelectionMode::None);
    f.press_char("v");
    CK_CHECK(copy->selection_mode() == SelectionMode::Character);
    f.press_char("V");
    CK_CHECK(copy->selection_mode() == SelectionMode::Line);
    f.press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "v"});
    CK_CHECK(copy->selection_mode() == SelectionMode::Rectangular);
    // The same key again is how a reader takes back a selection they did not
    // mean, without leaving copy mode and losing their place.
    f.press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "v"});
    CK_CHECK(copy->selection_mode() == SelectionMode::None);
}

CK_TEST(a_search_prompt_swallows_the_keys_that_would_otherwise_be_commands) {
    // Typing a search for "quit" must not leave copy mode on the `q`.
    Fixture f;
    f.settle();
    f.enter_copy_mode();
    ckm::client::CopyModeView* const copy = f.client.copy_mode();
    CK_CHECK(copy != nullptr);
    if (copy == nullptr) return;
    f.press_char("/");
    CK_CHECK(copy->searching());
    f.press_char("q");
    f.press_char("u");
    f.settle();
    CK_CHECK(f.client.copy_mode() != nullptr);  // still here
    CK_CHECK(copy->searching());
    f.press(ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""});
    CK_CHECK(!copy->searching());
    CK_CHECK(copy->query() == "qu");
    // Esc during a prompt abandons the new query and keeps the old one, which
    // is what a reader who changed their mind meant.
    f.press_char("/");
    f.press_char("z");
    f.press(ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""});
    CK_CHECK(!copy->searching());
    CK_CHECK(copy->query() == "qu");
    CK_CHECK(f.client.copy_mode() != nullptr);
}

CK_TEST(yanking_fills_the_internal_clipboard_and_leaves_copy_mode) {
    Fixture f;
    f.settle();
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(f.app.focused());
    CK_CHECK(view != nullptr);
    if (view == nullptr) return;
    view->session().feed_output("copy this line\r\n");
    f.settle();

    f.enter_copy_mode();
    ckm::client::CopyModeView* const copy = f.client.copy_mode();
    CK_CHECK(copy != nullptr);
    if (copy == nullptr) return;
    f.press_char("g");           // the first line of the history
    f.press_char("V");           // line-wise
    f.press_char("y");           // copy and leave
    f.settle();
    CK_CHECK(f.client.copy_mode() == nullptr);
    CK_CHECK(f.client.internal_clipboard() == "copy this line");
    // And the system clipboard got it too, through ckVision's writer.
    CK_CHECK(f.app.clipboard_text() == "copy this line");
}

CK_TEST(a_yank_with_nothing_marked_takes_the_line_the_cursor_is_on) {
    Fixture f;
    f.settle();
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(f.app.focused());
    CK_CHECK(view != nullptr);
    if (view == nullptr) return;
    view->session().feed_output("just this\r\n");
    f.settle();
    f.enter_copy_mode();
    f.press_char("g");
    f.press_char("y");
    f.settle();
    CK_CHECK(f.client.internal_clipboard() == "just this");
}

CK_TEST(a_copy_reaches_every_target_the_configuration_named_in_order) {
    // `[terminal] clipboard` is an ordered list, so the order is the setting.
    // The helper is injected: a client that forked would be a client no test
    // could drive, and would depend on which helpers the machine has.
    std::vector<std::pair<std::string, std::string>> ran;
    ClientOptions options = test_options();
    options.settings.clipboard = {
        ckm::ClipboardTarget{ckm::ClipboardTarget::Kind::Exec, "cat > /dev/null"},
        ckm::ClipboardTarget{ckm::ClipboardTarget::Kind::Osc52, {}},
        ckm::ClipboardTarget{ckm::ClipboardTarget::Kind::Pbcopy, {}}};
    options.clipboard_writer = [&ran](const std::string& command, std::string_view text) {
        ran.emplace_back(command, std::string(text));
        return true;
    };

    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, std::move(options)};
    app.step(clock.now_nanos());
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(app.focused());
    CK_CHECK(view != nullptr);
    if (view == nullptr) return;
    view->session().feed_output("target text\r\n");
    app.step(clock.now_nanos());

    app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "b"}});
    app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "["}});
    app.step(clock.now_nanos());
    app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "g"}});
    app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "V"}});
    app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "y"}});
    app.step(clock.now_nanos());

    CK_CHECK(ran.size() == 2U);
    if (ran.size() == 2U) {
        CK_CHECK(ran[0].first == "cat > /dev/null");
        CK_CHECK(ran[1].first == "pbcopy");
        CK_CHECK(ran[0].second == "target text");
    }
    CK_CHECK(app.clipboard_text() == "target text");
    // The internal one is filled whatever the targets did: `^B ]` has to work
    // over a connection where nothing outside this process can be reached.
    CK_CHECK(client.internal_clipboard() == "target text");
}

CK_TEST(a_paste_is_bracketed_only_for_a_program_that_asked_for_it) {
    // The rule, at the one place it is decided. Delivery is `send_input` on a
    // real subsession, which goes down a pty — observable only by waiting for
    // a child process to echo it back, which is not something a unit test
    // should be timing.
    CK_CHECK(ckm::client::paste_bytes("hello", false) == "hello");
    CK_CHECK(ckm::client::paste_bytes("hello", true) == "\x1b[200~hello\x1b[201~");
    // Nothing copied is nothing sent — not a pair of empty markers, which a
    // program in bracketed-paste mode would still have to read and handle.
    CK_CHECK(ckm::client::paste_bytes("", true).empty());
}

CK_TEST(paste_sends_what_copy_mode_yanked) {
    Fixture f;
    f.settle();
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(f.app.focused());
    CK_CHECK(view != nullptr);
    if (view == nullptr) return;
    view->session().feed_output("pasteable\r\n");
    f.settle();
    f.enter_copy_mode();
    f.press_char("g");
    f.press_char("V");
    f.press_char("y");
    f.settle();
    CK_CHECK(f.client.internal_clipboard() == "pasteable");

    // Through the command, which is the route the menu takes and the one
    // `^B ]` resolves to. It reaches a live handler and runs.
    CK_CHECK(f.app.commands().has_handler(id_of(f.app, ckm::client::commands::kPaste)));
    CK_CHECK(f.app.execute_command(id_of(f.app, ckm::client::commands::kPaste)));
    // ...and pasting does not consume what was copied: a reader pastes the
    // same thing into three windows.
    CK_CHECK(f.client.internal_clipboard() == "pasteable");
}

CK_TEST(paste_is_unavailable_until_something_has_been_copied) {
    // An empty clipboard is a menu item that is plainly not yet available,
    // not a dialog explaining why nothing happened.
    Fixture f;
    f.settle();
    CK_CHECK(!f.app.command_available(id_of(f.app, ckm::client::commands::kPaste)));
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(f.app.focused());
    CK_CHECK(view != nullptr);
    if (view == nullptr) return;
    view->session().feed_output("something\r\n");
    f.settle();
    f.enter_copy_mode();
    f.press_char("g");
    f.press_char("y");
    f.settle();
    CK_CHECK(f.app.command_available(id_of(f.app, ckm::client::commands::kPaste)));
}

#endif  // !_WIN32
