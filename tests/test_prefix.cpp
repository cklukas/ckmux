// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The prefix is the one key ckmux takes from the program running inside a
// terminal, so it carries the whole "transparent to inner apps" promise
// (the interface spec, Rule 1). These tests drive it through the real key
// path — the terminal view's parent-escape hook — not by calling handlers.
#if !defined(_WIN32)

#include <string>
#include <variant>

#include "client/client_app.hpp"
#include "client/remote_terminal.hpp"
#include "common/proto.hpp"
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

// A command's id, looked up the way everything durable does: by key. Ids are
// assigned by the registry at runtime (ckVision D-013), so a test cannot hold
// one as a constant — it asks the registry the application actually declared
// into.
ckv::ui::CommandId id_of(Application& app, std::string_view key) {
    return app.commands().id_for(key).value_or(ckv::ui::kInvalidCommand);
}

ClientOptions test_options() {
    ClientOptions options;
    // A program that simply stays alive, so a window has a live child
    // without the run depending on whose shell is installed. The login
    // form only renames argv[0], which cat does not mind.
    options.settings.shell = "/bin/cat";
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

// A client whose one terminal records what is typed at it instead of forking
// anything.
//
// The seam is the one an attached client drives (WP-5), so the bytes recorded
// here are the bytes a program would have been given. `/bin/cat` would echo
// them back eventually, and "eventually" is the problem: a test that waits for
// a child either races it or — as this one did — asserts nothing at all.
struct TypedIntoFixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    std::string typed;
    // Declared before the client: the client opens its window while it is
    // being constructed, and that window borrows this.
    ckm::client::RemoteTerminalSubsession child{
        1, ckv::term::embedded_xterm_sixel_profile(),
        [this](const ckm::proto::Message& message) {
            if (const auto* input = std::get_if<ckm::proto::Input>(&message)) typed += input->bytes;
        }};
    ClientApp client{app, [this] {
        ClientOptions options = test_options();
        options.terminal_source =
            [this](ckm::client::TerminalRequest) -> ckv::term::TerminalSubsession& { return child; };
        return options;
    }()};

    bool press(ckv::KeyChord chord) { return app.dispatch(ckv::KeyEvent{std::move(chord)}); }
    bool press_prefix() { return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "b"}); }
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
    f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal));
    f.app.execute_command(id_of(f.app, ckm::client::commands::kNewTerminal));
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
    TypedIntoFixture f;
    // The terminal has a size before anything is typed at it, exactly as one
    // the server has announced does.
    f.child.mirror().open(Size{80, 24});
    f.settle();
    CK_CHECK(dynamic_cast<ckv::widgets::TerminalView*>(f.app.focused()) != nullptr);
    f.typed.clear();

    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press_prefix());
    f.settle();

    CK_CHECK(!f.client.prefix_pending());
    // What the program received, and the whole of it: 0x02 is what a terminal
    // sends for Ctrl+B, and a reader who typed ^B ^B into their editor wants
    // that one byte — not a second copy of it from the press that armed the
    // prefix, and not nothing at all.
    CK_CHECK(f.typed == std::string("\x02", 1));
}

CK_TEST(an_ordinary_key_reaches_the_program_as_the_bytes_a_terminal_sends) {
    // The other half of the same claim, and the control the test above needs:
    // the prefix is the ONE key ckmux keeps, so anything else has to arrive at
    // the child encoded the way the outer terminal would have encoded it.
    TypedIntoFixture f;
    f.child.mirror().open(Size{80, 24});
    f.settle();
    f.typed.clear();

    CK_CHECK(f.press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "x"}));
    CK_CHECK(f.press(ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}));
    f.settle();
    CK_CHECK(f.typed == "x\r");
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

// --- Rebinding (WP-12) ---------------------------------------------------

namespace {

// A client whose configuration rebound some keys, built the way the real one
// is: directives from a file, applied at construction.
struct ReboundFixture {
    static ClientOptions options() {
        ClientOptions o = test_options();
        // Only bindings ckmux can honour: one that cannot would be reported
        // as a warning, and a warning opens a dialog over the desktop at
        // startup — which is the right behaviour and the wrong fixture. The
        // refusal has its own test below.
        o.settings.binds = {
            ckm::BindDirective{ckm::KeyContext::Terminal, "C", ckm::Action::NewTerminal},
            ckm::BindDirective{ckm::KeyContext::Terminal, "c", std::nullopt},  // unbind
        };
        return o;
    }
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, options()};

    bool press(ckv::KeyChord chord) { return app.dispatch(ckv::KeyEvent{std::move(chord)}); }
    bool press_char(const std::string& text) {
        return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, text});
    }
    bool press_prefix() { return press(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "b"}); }
    void settle() { app.step(clock.now_nanos()); }
};

bool any_contains(const std::vector<std::string>& labels, std::string_view text) {
    for (const std::string& label : labels)
        if (label.find(text) != std::string::npos) return true;
    return false;
}

}  // namespace

CK_TEST(a_rebound_key_opens_the_terminal_and_the_old_one_no_longer_does) {
    ReboundFixture f;
    f.settle();
    CK_CHECK(f.client.desktop().windows().size() == 1U);

    // The old chord reaches nothing: it was unbound, so the prefix resolves
    // to no command rather than to whatever used to hold it.
    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press_char("c"));
    f.settle();
    CK_CHECK(f.client.desktop().windows().size() == 1U);

    // The new one does what the file said.
    CK_CHECK(f.press_prefix());
    CK_CHECK(f.press_char("C"));
    f.settle();
    CK_CHECK(f.client.desktop().windows().size() == 2U);
}

CK_TEST(a_rebinding_reaches_the_footer_that_advertises_it) {
    // The whole point of one table: what dispatches and what the reader is
    // shown cannot disagree. Before this, the footer read "^B c new term"
    // while `c` did nothing.
    ReboundFixture f;
    f.settle();
    const std::vector<std::string> labels = f.client.footer_labels();
    CK_CHECK(any_contains(labels, "^B C"));
    CK_CHECK(!any_contains(labels, "^B c "));
}

CK_TEST(a_binding_ckmux_cannot_honour_is_reported_rather_than_ignored) {
    // A copy-mode bind: the context has no keys yet, so it is refused out
    // loud. A binding that looks live in a file and does nothing in the
    // program is the defect this project pins tests against.
    ckm::client::Keymap keymap;
    std::vector<std::string> problems;
    const std::vector<ckm::BindDirective> directives{
        ckm::BindDirective{ckm::KeyContext::Terminal, "C", ckm::Action::NewTerminal},
        ckm::BindDirective{ckm::KeyContext::Terminal, "c", std::nullopt},
        ckm::BindDirective{ckm::KeyContext::CopyMode, "v", ckm::Action::Zoom}};
    keymap.apply(directives, problems);
    CK_CHECK(problems.size() == 1U);
    CK_CHECK(problems[0].find("copy-mode") != std::string::npos);
    // ...and the honoured ones still took effect.
    CK_CHECK(keymap.find("C") != nullptr);
    CK_CHECK(keymap.find("c") == nullptr);
}

CK_TEST(the_digits_cannot_be_rebound_because_they_carry_a_value) {
    ckm::client::Keymap keymap;
    std::vector<std::string> problems;
    const std::vector<ckm::BindDirective> directives{
        ckm::BindDirective{ckm::KeyContext::Terminal, "4", ckm::Action::Detach}};
    keymap.apply(directives, problems);
    CK_CHECK(problems.size() == 1U);
    CK_CHECK(problems[0].find("cannot be rebound") != std::string::npos);
    CK_CHECK(keymap.find("4") == nullptr);
}

#endif
