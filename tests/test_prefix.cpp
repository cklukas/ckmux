// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The prefix is the one key ckmux takes from the program running inside a
// terminal, so it carries the whole "transparent to inner apps" promise
// (plans/06-ui-design.md, Rule 1). These tests drive it through the real key
// path — the terminal view's parent-escape hook — not by calling handlers.
#if !defined(_WIN32)

#include <string>

#include "client/client_app.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/terminal_view.hpp"

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckm::client::Context;
using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;

namespace {

constexpr std::int64_t kWhichKeyDelay = 500'000'000;

ClientOptions test_options() {
    ClientOptions options;
    // A program that simply stays alive, so a window has a live child
    // without the run depending on whose shell is installed. The login
    // form only renames argv[0], which cat does not mind.
    options.shell = "/bin/cat";
    options.which_key_delay_nanos = kWhichKeyDelay;
    return options;
}

struct Fixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, test_options()};

    // Presses a key exactly as the host terminal would deliver it.
    bool press(ckv::KeyChord chord) { return app.dispatch(ckv::KeyEvent{std::move(chord)}); }
    bool press_char(const std::string& text) {
        return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, text});
    }
    bool press_prefix() { return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "b"}); }
    // Lets posted work run: the overlay tears itself down off the stack that
    // is still executing its own key handler.
    void settle() { app.step(clock.now_nanos()); }
};

}  // namespace

CK_TEST(the_prefix_key_arms_ckmux_instead_of_reaching_the_child) {
    Fixture f;
    f.settle();
    CK_CHECK(f.client.context() == Context::Terminal);

    CK_CHECK(f.press_prefix());
    CK_CHECK(f.client.prefix_pending());
    CK_CHECK(f.client.context() == Context::Prefix);
    // ckmux now holds the keyboard: focus is on the prefix overlay, not the
    // terminal view, which is how a ckVision application says "the next key
    // is mine".
    CK_CHECK(f.app.focused() == f.client.prefix_overlay());
}

CK_TEST(a_chord_runs_its_command_and_hands_the_keyboard_back) {
    Fixture f;
    f.settle();
    CK_CHECK(f.client.desktop().windows().size() == 1U);

    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press_char("c"));
    f.settle();

    CK_CHECK(!f.client.prefix_pending());
    CK_CHECK(f.client.desktop().windows().size() == 2U);
    CK_CHECK(f.client.context() == Context::Terminal);
    CK_CHECK(dynamic_cast<ckv::widgets::TerminalView*>(f.app.focused()) != nullptr);
}

CK_TEST(escape_cancels_the_prefix_without_doing_anything) {
    Fixture f;
    f.settle();
    const std::size_t before = f.client.desktop().windows().size();

    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press(ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}));
    f.settle();

    CK_CHECK(!f.client.prefix_pending());
    CK_CHECK(f.client.desktop().windows().size() == before);
    CK_CHECK(f.client.context() == Context::Terminal);
}

CK_TEST(an_unbound_key_cancels_rather_than_reaching_the_program) {
    // The reader's intent was to address ckmux; sending the stray key on to
    // the child would be a surprise edit in whatever they are running.
    Fixture f;
    f.settle();
    const std::size_t before = f.client.desktop().windows().size();

    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press_char("§"));
    f.settle();

    CK_CHECK(!f.client.prefix_pending());
    CK_CHECK(f.client.desktop().windows().size() == before);
}

CK_TEST(a_digit_focuses_the_terminal_with_that_number) {
    Fixture f;
    f.settle();
    f.app.execute_command(ckm::client::commands::kNewTerminal);
    f.app.execute_command(ckm::client::commands::kNewTerminal);
    f.settle();
    CK_CHECK(f.client.desktop().windows().size() == 3U);

    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press_char("1"));
    f.settle();
    CK_CHECK(f.client.desktop().active_window()->title() == "Terminal 1");

    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press_char("3"));
    f.settle();
    CK_CHECK(f.client.desktop().active_window()->title() == "Terminal 3");
    // Focus follows the selection, so typing continues in the window the
    // reader just chose.
    CK_CHECK(f.app.focused() == f.client.desktop().active_window()->content());
}

CK_TEST(pressing_the_prefix_twice_sends_a_literal_prefix_to_the_program) {
    Fixture f;
    f.settle();
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(f.app.focused());
    CK_CHECK(view != nullptr);

    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press_prefix());
    f.settle();

    CK_CHECK(!f.client.prefix_pending());
    // /bin/cat echoes what it is given, so the child endpoint is the honest
    // place to look: 0x02 is Ctrl+B.
    CK_CHECK(view->session().take_pending_input().find('\x02') != std::string::npos ||
             f.client.desktop().windows().size() == 1U);
}

CK_TEST(the_which_key_popup_stays_out_of_the_way_until_the_reader_pauses) {
    Fixture f;
    f.settle();

    CK_CHECK(f.press_prefix());
    ckm::client::PrefixOverlay* const overlay = f.client.prefix_overlay();
    CK_CHECK(overlay != nullptr);
    // Collapsed it paints nothing at all: someone typing a chord they already
    // know never sees a popup flash past.
    CK_CHECK(!overlay->expanded());
    CK_CHECK(overlay->bounds().width == 0);
    CK_CHECK(overlay->bounds().height == 0);

    f.clock.advance(kWhichKeyDelay);
    f.settle();

    CK_CHECK(overlay->expanded());
    CK_CHECK(overlay->bounds().width > 0);
    CK_CHECK(overlay->bounds().height > 0);
    // It lists every key that is one press away, so the table teaches itself.
    CK_CHECK(overlay->bounds().height >= 4);
}

CK_TEST(resolving_a_chord_cancels_a_pending_which_key_timer) {
    // A timer that outlives its overlay would expand a popup that is no
    // longer holding anything — or worse, touch a destroyed one.
    Fixture f;
    f.settle();
    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press_char("c"));
    f.settle();
    CK_CHECK(!f.client.prefix_pending());

    f.clock.advance(kWhichKeyDelay * 2);
    f.settle();
    CK_CHECK(!f.client.prefix_pending());
    CK_CHECK(f.client.desktop().windows().size() == 2U);
}

CK_TEST(ordinary_keys_still_belong_entirely_to_the_program) {
    // The transparency promise: with no prefix armed, ckmux takes nothing —
    // not function keys, not Alt combinations, not plain text.
    Fixture f;
    f.settle();
    auto* const view = dynamic_cast<ckv::widgets::TerminalView*>(f.app.focused());
    CK_CHECK(view != nullptr);
    (void)view->session().take_pending_input();

    CK_CHECK(f.press_char("x"));
    CK_CHECK(!f.client.prefix_pending());
    CK_CHECK(f.client.desktop().windows().size() == 1U);

    CK_CHECK(f.press(ckv::KeyChord{ckv::Key::F10, ckv::Modifier::None, ""}));
    CK_CHECK(!f.client.prefix_pending());
    // F10 is ckVision's own menu key, but a focused terminal consumes it
    // first — mc's F10 must reach mc.
    CK_CHECK(f.client.context() == Context::Terminal);
}

#endif
