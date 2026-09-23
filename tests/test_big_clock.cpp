// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The big clock: tmux's clock mode, as a ckmux command. `^B t` puts the time
// over the focused terminal in block digits, the View menu adds the date and
// both at once, and any key puts it away. Driven through real key dispatch
// and the real menu bar, because "a reader can show the time" is a claim about
// keys and menus, not about a handler.
#if !defined(_WIN32)

#include <string>
#include <vector>

#include "client/client_app.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/ui/application.hpp"
#include "cvision/widgets/big_clock.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/terminal_view.hpp"

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckm::client::Context;
using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;
using ckv::widgets::BigClockContent;

namespace {

ClientOptions test_options() {
    ClientOptions options;
    options.settings.shell = "/bin/cat";
    // 23 September 2026, 14:05:09, wherever the test runs.
    options.local_now = [] {
        return ckm::client::LocalMoment{ckv::widgets::DateValue{2026, 9, 23},
                                        ckv::widgets::TimeValue{14, 5, 9}};
    };
    return options;
}

struct Fixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client;

    explicit Fixture(ClientOptions options = test_options()) : client{app, std::move(options)} {}

    bool press(ckv::KeyChord chord) { return app.dispatch(ckv::KeyEvent{std::move(chord)}); }
    bool press_char(const std::string& text) {
        return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, text});
    }
    bool press_key(ckv::Key key) { return press(ckv::KeyChord{key, ckv::Modifier::None, {}}); }
    bool press_prefix() { return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "b"}); }
    // Runs posted work and a frame, and lets the title poll see the result.
    // Twice, because work posted by posted work — the prefix resolves on a
    // post, and the keyboard follows an activation on another — runs on the
    // next turn of the loop, as it does in the running client.
    void settle() {
        for (int turn = 0; turn < 2; ++turn) {
            clock.advance(100'000'000);
            app.step(clock.now_nanos());
        }
    }
    void show_time() {
        press_prefix();
        press_char("t");
        settle();
    }
    // The View menu reached from the keyboard, the way a ckmux reader reaches
    // any menu: the prefix and the menu key, then along the bar to View.
    bool open_view_menu() {
        press_prefix();
        press_char("m");
        settle();
        press_key(ckv::Key::Down);  // opens the first menu, Session
        settle();
        // Recognised by what it holds, not by its position on the bar.
        const ckv::ui::CommandId show_time = id_of(ckm::client::commands::kShowClock);
        for (int steps = 0; steps < 6; ++steps) {
            if (const ckv::widgets::DropdownMenu* const menu = dropped())
                for (const ckv::widgets::MenuItem& item : menu->items())
                    if (item.command() == show_time) return true;
            press_key(ckv::Key::Right);
            settle();
        }
        return false;
    }
    ckv::ui::CommandId id_of(std::string_view key) {
        return app.commands().id_for(key).value_or(ckv::ui::kInvalidCommand);
    }
    ckv::widgets::DropdownMenu* dropped() {
        for (ckv::ui::View* popup : client.desktop().popups())
            if (auto* const menu = dynamic_cast<ckv::widgets::DropdownMenu*>(popup)) return menu;
        return nullptr;
    }
    ckv::widgets::Window* terminal_window() {
        for (ckv::widgets::Window* window : client.desktop().windows())
            if (dynamic_cast<ckv::widgets::TerminalView*>(window->content()) != nullptr) return window;
        return nullptr;
    }
    // Where a view actually is on the screen, against where its window's
    // interior actually is — both in screen cells. Comparing the face's
    // bounds with content_rect() proved nothing: the face had been placed
    // WITH content_rect(), in the wrong coordinate space, and the two agreed
    // while the face sat at the desktop's corner.
    static bool covers_interior(const ckv::ui::View& view, const ckv::widgets::Window& window) {
        const ckv::Rect frame = window.absolute_bounds();
        const ckv::Rect local = window.content_rect();
        const ckv::Rect interior{frame.x + local.x, frame.y + local.y, local.width, local.height};
        return view.absolute_bounds() == interior;
    }
    bool footer_says(std::string_view text) {
        for (const std::string& label : client.footer_labels())
            if (label.find(text) != std::string::npos) return true;
        return false;
    }
};

}  // namespace

CK_TEST(prefix_t_puts_the_time_over_the_focused_terminal) {
    Fixture f;
    f.settle();
    CK_CHECK(f.client.big_clock() == nullptr);
    f.show_time();

    ckv::widgets::BigClockView* const face = f.client.big_clock();
    CK_CHECK(face != nullptr);
    if (face == nullptr) return;
    CK_CHECK(face->content() == BigClockContent::Time);
    CK_CHECK(face->lines() == std::vector<std::string>{"14:05:09"});
    // Over the terminal's own content, exactly — not the whole screen, not
    // the frame the reader grabs the window by, and not the desktop's corner.
    // The window is moved off the origin first, so a face placed in the
    // window's coordinates instead of the screen's cannot pass by accident.
    ckv::widgets::Window* const window = f.terminal_window();
    window->set_bounds(ckv::Rect{15, 6, 70, 20});
    f.settle();
    CK_CHECK(window->content_cover() == face);
    CK_CHECK(Fixture::covers_interior(*face, *window));
    CK_CHECK(face->absolute_bounds().x == 16 && face->absolute_bounds().y == 7);
    CK_CHECK(face->drawn_large());
    // The keyboard is the face's, and the footer says what it does rather
    // than advertising prefix chords the face would swallow.
    CK_CHECK(f.app.focused() == face);
    CK_CHECK(f.footer_says("any key"));
    CK_CHECK(!f.footer_says("new term"));
}

CK_TEST(any_key_puts_the_clock_away_and_gives_the_keyboard_back_to_the_program) {
    Fixture f;
    f.settle();
    f.show_time();
    CK_CHECK(f.client.big_clock() != nullptr);
    f.press_char("q");
    f.settle();
    CK_CHECK(f.client.big_clock() == nullptr);
    CK_CHECK(f.client.context() == Context::Terminal);
    CK_CHECK(!f.footer_says("any key"));

    // A special key does the same: the face has no keys of its own to keep.
    f.show_time();
    CK_CHECK(f.client.big_clock() != nullptr);
    f.press_key(ckv::Key::Escape);
    f.settle();
    CK_CHECK(f.client.big_clock() == nullptr);
    CK_CHECK(f.client.context() == Context::Terminal);
}

CK_TEST(the_view_menu_shows_the_date_and_the_date_with_the_time) {
    Fixture f;
    f.settle();
    CK_CHECK(f.open_view_menu());
    f.press_char("d");  // Show &Date
    f.settle();
    CK_CHECK(f.client.big_clock() != nullptr);
    if (f.client.big_clock() == nullptr) return;
    CK_CHECK(f.client.big_clock()->lines() == std::vector<std::string>{"2026-09-23"});

    f.press_char("x");  // put it away, then ask for both
    f.settle();
    CK_CHECK(f.client.big_clock() == nullptr);
    CK_CHECK(f.open_view_menu());
    f.press_char("a");  // Show Date &and Time
    f.settle();
    CK_CHECK(f.client.big_clock() != nullptr);
    if (f.client.big_clock() == nullptr) return;
    CK_CHECK((f.client.big_clock()->lines() == std::vector<std::string>{"2026-09-23", "14:05:09"}));
}

CK_TEST(the_clock_follows_its_window_when_the_window_changes_size) {
    // The environment changes under a face that stays up: the window it covers
    // is made smaller than the glyphs, then large again. The face goes with
    // it, falls back to plain text while too small, and never covers a
    // rectangle the window has left.
    Fixture f;
    f.settle();
    f.show_time();
    ckv::widgets::Window* const window = f.terminal_window();
    ckv::widgets::BigClockView* const face = f.client.big_clock();
    CK_CHECK(face != nullptr && window != nullptr);
    if (face == nullptr || window == nullptr) return;

    window->set_bounds(ckv::Rect{12, 4, 20, 6});
    f.settle();
    CK_CHECK(f.client.big_clock() == face);
    CK_CHECK(Fixture::covers_interior(*face, *window));
    CK_CHECK(!face->drawn_large());

    window->set_bounds(ckv::Rect{20, 3, 70, 16});  // larger, and moved
    f.settle();
    CK_CHECK(Fixture::covers_interior(*face, *window));
    CK_CHECK(face->drawn_large());
}

CK_TEST(the_clock_stays_up_while_the_reader_works_in_another_terminal) {
    // The reason to show a clock: put it up in one window, go on working in
    // another, and glance across. Leaving the window leaves the clock; coming
    // back — by keyboard or by mouse — lands on the clock, not on the terminal
    // hidden under it, and only then does a key put it away.
    Fixture f;
    f.settle();
    ckv::widgets::Window* const clock_window = f.terminal_window();
    clock_window->set_bounds(ckv::Rect{2, 2, 50, 16});
    f.show_time();
    ckv::widgets::BigClockView* const face = f.client.big_clock();
    CK_CHECK(face != nullptr);
    if (face == nullptr) return;

    f.press_prefix();
    f.press_char("c");  // a second terminal, which takes the keyboard
    f.settle();
    // Beside the clock rather than cascaded over it, so the click back below
    // lands on the clock and not on this window's corner.
    for (ckv::widgets::Window* window : f.client.desktop().windows())
        if (window != clock_window && dynamic_cast<ckv::widgets::TerminalView*>(window->content()) != nullptr)
            window->set_bounds(ckv::Rect{54, 2, 44, 16});
    f.settle();
    CK_CHECK(f.client.big_clock() == face);
    CK_CHECK(clock_window->content_cover() == face);
    CK_CHECK(f.client.context() == Context::Terminal);
    // The footer is the window being worked in, not the clock's.
    CK_CHECK(!f.footer_says("any key"));
    f.press_char("l");
    f.press_char("s");
    f.press_key(ckv::Key::Enter);
    f.settle();
    CK_CHECK(f.client.big_clock() == face);

    // Back by keyboard: the keys go to the clock, and nothing is dismissed on
    // the way.
    f.press_prefix();
    f.press_char("p");
    f.settle();
    CK_CHECK(f.client.big_clock() == face);
    CK_CHECK(f.app.focused() == face);
    CK_CHECK(f.footer_says("any key"));

    // Away again, and back by mouse: the click that returns only focuses.
    f.press_prefix();
    f.press_char("n");
    f.settle();
    CK_CHECK(f.app.focused() != face);
    const ckv::Rect at = face->absolute_bounds();
    const ckv::Point inside{at.x + at.width / 2, at.y + at.height / 2};
    f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Left, inside, std::nullopt,
                                   ckv::Modifier::None});
    f.app.dispatch(ckv::MouseEvent{ckv::MouseAction::Up, ckv::MouseButton::Left, inside, std::nullopt,
                                   ckv::Modifier::None});
    f.settle();
    CK_CHECK(f.client.big_clock() == face);
    CK_CHECK(f.app.focused() == face);

    // And now a key, in the clock's own window, puts it away.
    f.press_char("q");
    f.settle();
    CK_CHECK(f.client.big_clock() == nullptr);
    CK_CHECK(clock_window->content_cover() == nullptr);
    CK_CHECK(f.client.context() == Context::Terminal);
}

CK_TEST(asking_for_the_clock_in_another_terminal_moves_it_there) {
    Fixture f;
    f.settle();
    ckv::widgets::Window* const first = f.terminal_window();
    f.show_time();
    f.press_prefix();
    f.press_char("c");
    f.settle();
    f.show_time();
    ckv::widgets::BigClockView* const face = f.client.big_clock();
    CK_CHECK(face != nullptr);
    CK_CHECK(first->content_cover() == nullptr);
    int covered = 0;
    for (ckv::widgets::Window* window : f.client.desktop().windows())
        if (window->content_cover() != nullptr) ++covered;
    CK_CHECK(covered == 1);
    CK_CHECK(face != nullptr && f.app.focused() == face);
}

CK_TEST(a_minimized_window_keeps_its_clock_and_lets_go_of_the_keyboard) {
    Fixture f;
    f.settle();
    f.press_prefix();
    f.press_char("c");
    f.settle();
    f.show_time();
    ckv::widgets::BigClockView* const face = f.client.big_clock();
    CK_CHECK(face != nullptr);
    // From the clock itself: the prefix is a command, not a key AT the
    // clock, so it arms as it would in the terminal and the clock stays.
    f.press_prefix();
    f.press_char("_");
    f.settle();
    CK_CHECK(f.client.big_clock() == face);
    // Hidden with its window, and not holding the keyboard from there: a
    // reader typing next must reach a window they can see.
    CK_CHECK(f.app.focused() != face);
    CK_CHECK(f.app.focused() == nullptr || f.app.focused()->visible_in_tree());
}

CK_TEST(asking_again_while_the_clock_is_up_changes_what_it_shows) {
    // One face per reader, not a stack of them: the second request answers a
    // different question on the same face.
    Fixture f;
    f.settle();
    f.show_time();
    ckv::widgets::BigClockView* const face = f.client.big_clock();
    CK_CHECK(face != nullptr);
    CK_CHECK(f.app.execute_command(f.id_of(ckm::client::commands::kShowDate)));
    f.settle();
    CK_CHECK(f.client.big_clock() == face);
    CK_CHECK(f.terminal_window()->content_cover() == face);
    if (face != nullptr) CK_CHECK(face->content() == BigClockContent::Date);
}

CK_TEST(a_client_that_cannot_read_the_time_offers_no_clock) {
    // Both halves: without a time source the commands are unavailable and the
    // chord shows nothing; with one (every other case here) they work.
    ClientOptions options = test_options();
    options.local_now = nullptr;
    Fixture f{std::move(options)};
    f.settle();
    CK_CHECK(!f.app.commands().is_enabled(f.id_of(ckm::client::commands::kShowClock)));
    f.show_time();
    CK_CHECK(f.client.big_clock() == nullptr);

    Fixture g;
    g.settle();
    CK_CHECK(g.app.commands().is_enabled(g.id_of(ckm::client::commands::kShowClock)));
}

#endif  // !defined(_WIN32)
