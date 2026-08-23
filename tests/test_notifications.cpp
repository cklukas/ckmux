// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// What a reader is told without being asked anything (WP-14).
//
// The case this exists for is the silent one: a reader's session is taken over
// from another terminal, or ends under them, and every window they had
// disappears. The server says why — `Detached` carries a reason and a line of
// text — and until this package the client threw it away, because the only
// place it could have gone was stderr, and an attached client's stderr IS the
// screen ckVision is drawing on. So the account goes to a notification over
// the desktop, and these tests are about it being there, saying the right
// thing, and going away again at the right time (or not at all, for the one
// class that must wait for the reader).
#if !defined(_WIN32)

#include <cctype>
#include <string>
#include <string_view>

#include "client/client_app.hpp"
#include "common/proto.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/common_components.hpp"

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;

namespace {

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kToastLife = 5 * kSecond;

ClientOptions test_options() {
    ClientOptions options;
    options.settings.shell = "/bin/cat";
    options.toast_nanos = kToastLife;
    return options;
}

struct Fixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, test_options()};

    void settle() { app.step(clock.now_nanos()); }
    // Moves time and lets the application deliver what is now due — a toast
    // expires on a ckVision timer, and a clock nobody steps is a clock nothing
    // reads.
    void advance(std::int64_t nanos) {
        clock.advance(nanos);
        app.step(clock.now_nanos());
        app.step(clock.now_nanos());  // the teardown is posted, not called
    }
    std::size_t posted() const {
        const auto* centre = const_cast<ClientApp&>(client).notifications();
        return centre == nullptr ? 0U : centre->notifications().size();
    }
    std::string first_line() const {
        const auto* centre = const_cast<ClientApp&>(client).notifications();
        if (centre == nullptr || centre->notifications().empty()) return {};
        return centre->notifications()[0].text;
    }
};

// The same client, with settings a test wants to change first — a `bind`
// line, say, so the help pages have a rebinding to mark.
struct ConfiguredFixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client;

    explicit ConfiguredFixture(ClientOptions options) : client{app, std::move(options)} {}
    void settle() { app.step(clock.now_nanos()); }
};

}  // namespace

// --- What a detach says ----------------------------------------------------

CK_TEST(a_session_taken_over_says_so_and_names_the_session) {
    Fixture f;
    f.settle();
    f.client.set_attached_session(7, "build");

    f.client.report_detached(ckm::proto::DetachReason::Takeover,
                             "taken over by another client on this machine");
    f.settle();

    CK_CHECK(f.posted() == 1U);
    CK_CHECK(f.first_line() == "'build' was taken over by another client on this machine");
    // Persistent: the reader this is for is the one who was not at the
    // keyboard when it happened, and a line that had already faded would leave
    // them an empty desktop with no account of it.
    const auto* centre = f.client.notifications();
    CK_CHECK(centre != nullptr);
    if (centre == nullptr) return;
    CK_CHECK(centre->notifications()[0].persistent);
    CK_CHECK(centre->notifications()[0].severity == ckv::widgets::NotificationSeverity::Warning);
}

CK_TEST(a_session_that_ended_and_a_server_that_stopped_each_say_which) {
    Fixture f;
    f.settle();
    f.client.set_attached_session(7, "build");
    f.client.report_detached(ckm::proto::DetachReason::SessionKilled, "ended by another client");
    f.settle();
    CK_CHECK(f.first_line() == "'build' ended: ended by another client");

    Fixture g;
    g.settle();
    g.client.set_attached_session(9, "notes");
    g.client.report_detached(ckm::proto::DetachReason::ServerShutdown, "the server is shutting down");
    g.settle();
    // Not named after the session: the server going takes every session with
    // it, so naming one of them would be describing the smaller half.
    CK_CHECK(g.first_line() == "The ckmux server stopped: the server is shutting down");
}

CK_TEST(a_detach_the_reader_asked_for_says_nothing) {
    // They pressed the key. The picker is already on its way, and a line
    // telling them what they just did is furniture.
    Fixture f;
    f.settle();
    f.client.set_attached_session(7, "build");
    f.client.report_detached(ckm::proto::DetachReason::User, "you asked");
    f.settle();
    CK_CHECK(f.posted() == 0U);
    CK_CHECK(f.client.notifications() == nullptr);
}

CK_TEST(a_session_with_no_name_still_gets_an_account_of_what_happened) {
    // A client can be taken over before any list has named the session it was
    // watching. The sentence has to work without the name rather than reading
    // "'' was taken over".
    Fixture f;
    f.settle();
    f.client.report_detached(ckm::proto::DetachReason::Takeover, "taken over by another client");
    f.settle();
    CK_CHECK(f.first_line() == "This session was taken over by another client");
}

// --- How long one stays ----------------------------------------------------

CK_TEST(an_ordinary_toast_takes_itself_away) {
    Fixture f;
    f.settle();
    f.client.notify("Took over 'build'");
    f.settle();
    CK_CHECK(f.posted() == 1U);

    f.advance(kToastLife / 2);
    CK_CHECK(f.posted() == 1U);  // not yet

    f.advance(kToastLife);
    CK_CHECK(f.posted() == 0U);
    // And the surface goes with it: an idle ckmux carries no notification
    // chrome at all, so nothing sits over the reader's windows taking clicks.
    CK_CHECK(f.client.notifications() == nullptr);
}

CK_TEST(the_one_the_reader_must_not_miss_stays_until_it_is_dismissed) {
    Fixture f;
    f.settle();
    f.client.set_attached_session(7, "build");
    f.client.report_detached(ckm::proto::DetachReason::Takeover, "taken over");
    f.settle();

    f.advance(kToastLife * 10);
    CK_CHECK(f.posted() == 1U);

    // Dismissed by the reader, which is the only thing that takes it.
    auto* centre = f.client.notifications();
    CK_CHECK(centre != nullptr);
    if (centre == nullptr) return;
    centre->dismiss(0);
    f.settle();
    f.settle();
    CK_CHECK(f.client.notifications() == nullptr);
}

CK_TEST(nothing_is_posted_for_an_empty_line) {
    Fixture f;
    f.settle();
    f.client.notify("");
    f.settle();
    CK_CHECK(f.client.notifications() == nullptr);
}

// --- Where it sits ---------------------------------------------------------

CK_TEST(a_toast_stays_inside_the_desktops_own_content_area) {
    Fixture f;
    f.settle();
    f.client.notify("Config reloaded");
    f.settle();

    const auto* centre = f.client.notifications();
    CK_CHECK(centre != nullptr);
    if (centre == nullptr) return;
    const ckv::Rect where = centre->absolute_bounds();
    const ckv::Rect area = f.client.desktop().content_area();

    // Top-right, and inside the area rather than over the chrome: a toast that
    // covered the window bar would be sitting on the one row a reader reaches
    // for to get a minimized terminal back (WP-34).
    CK_CHECK(where.y >= area.y);
    CK_CHECK(where.y + where.height <= area.y + area.height);
    CK_CHECK(where.x + where.width == area.x + area.width);
    CK_CHECK(where.x >= area.x);
    CK_CHECK(where.height == 1);
}

CK_TEST(two_lines_make_a_two_row_surface_and_one_removal_shrinks_it) {
    Fixture f;
    f.settle();
    f.client.notify("first", ckv::widgets::NotificationSeverity::Info, /*persistent=*/true);
    f.client.notify("second", ckv::widgets::NotificationSeverity::Info, /*persistent=*/true);
    f.settle();

    auto* centre = f.client.notifications();
    CK_CHECK(centre != nullptr);
    if (centre == nullptr) return;
    CK_CHECK(centre->absolute_bounds().height == 2);

    centre->dismiss(0);
    f.settle();
    f.settle();
    centre = f.client.notifications();
    CK_CHECK(centre != nullptr);
    if (centre == nullptr) return;
    // The host hears about a change it did not make and re-sizes: without
    // that, the surface would keep a row for a line that is gone.
    CK_CHECK(centre->absolute_bounds().height == 1);
    CK_CHECK(centre->notifications()[0].text == "second");
}

CK_TEST(a_toast_never_takes_the_keyboard_from_the_program) {
    // The whole point of a notification rather than a message box: it is news,
    // not a question, so it must not be a focus stop between the reader and
    // the terminal they are typing into.
    Fixture f;
    f.settle();
    ckv::ui::View* const focused_before = f.app.focused();
    f.client.notify("Config reloaded");
    f.settle();
    CK_CHECK(f.app.focused() == focused_before);
    const auto* centre = f.client.notifications();
    CK_CHECK(centre != nullptr);
    if (centre == nullptr) return;
    CK_CHECK(centre->focus_policy() == ckv::ui::FocusPolicy::None);
}


// --- The help pages (WP-14) ------------------------------------------------
//
// "Everything is in the menu, and every menu entry shows the key that reaches
// it" is only half a promise: the other half is that F1 lands on a page about
// what the reader is actually doing, and that the page listing the keys is
// generated from the same table that dispatches them — including the reader's
// own rebindings, marked rather than silently applied.

namespace {

bool topic_exists(const ClientApp& client, const std::string& key) {
    // MemoryHelpProvider answers an unknown key with a "Not Found" topic
    // rather than failing, so the check is whether the page is real.
    const ckv::widgets::HelpTopic topic = client.help().topic(key);
    return !topic.body.empty() && topic.title != "Not Found";
}

}  // namespace

CK_TEST(every_surface_that_names_a_help_page_has_one) {
    // A help key with no topic behind it is an F1 that apologises. These are
    // the keys ckmux names — on views (`set_help_context_key`), on menu items
    // (`with_help`), and from `show_all_keys`.
    Fixture f;
    f.settle();
    for (const char* key : {"ckmux.terminal", "ckmux.prefix", "ckmux.keys", "ckmux.keys.all",
                            "ckmux.switcher", "ckmux.copy", "ckmux.picker", "ckmux.move",
                            "ckmux.notice"}) {
        CK_CHECK(topic_exists(f.client, key));
    }
    // And the negative partner, so this test cannot pass by the provider
    // having become generous: a key nothing declares is still not a page.
    CK_CHECK(!topic_exists(f.client, "ckmux.no.such.page"));
}

CK_TEST(the_complete_listing_covers_chorded_and_menu_only_commands) {
    Fixture f;
    f.settle();
    const std::string body = f.client.help().topic("ckmux.keys.all").body;

    // A chorded one, with its chord.
    CK_CHECK(body.find("^B c") != std::string::npos);
    CK_CHECK(body.find("new term") != std::string::npos);
    // One this package added, which is how the page proves it is generated
    // rather than written: nobody edited this text when WP-34 landed.
    CK_CHECK(body.find("^B _") != std::string::npos);
    CK_CHECK(body.find("minimize") != std::string::npos);
    // And a command no key reaches, listed under its own heading rather than
    // left out — "everything is in the menu" is a promise about these.
    CK_CHECK(body.find("In the menus only") != std::string::npos);
    CK_CHECK(body.find("settings") != std::string::npos);
    // With nothing rebound, the page says so rather than showing an empty
    // "Changed by your configuration" heading.
    CK_CHECK(body.find("Changed by your configuration") == std::string::npos);
    CK_CHECK(body.find("ckmux's own default") != std::string::npos);
}

CK_TEST(a_rebinding_is_marked_with_what_it_replaced) {
    // The reader whose muscle memory stopped working needs to be told that
    // their own file is why, and told it here.
    ClientOptions options = test_options();
    // An `unbind` is a directive with no action; this is the ordinary form —
    // "make ^B C open a terminal" — which moves the chord off ^B c.
    options.settings.binds.push_back(
        ckm::BindDirective{ckm::KeyContext::Terminal, "C", ckm::Action::NewTerminal});
    ConfiguredFixture f{std::move(options)};
    f.settle();
    const std::string body = f.client.help().topic("ckmux.keys.all").body;

    CK_CHECK(body.find("Changed by your configuration") != std::string::npos);
    // Both halves: what it is now, and what it was.
    CK_CHECK(body.find("new term: now ^B C, was ^B c") != std::string::npos);
    // And the listing above shows the chord that now works, not the default.
    CK_CHECK(body.find("^B C") != std::string::npos);
}

CK_TEST(the_window_bar_page_describes_the_bar_as_it_now_behaves) {
    // The page went stale the moment WP-34 and WP-35 landed: it said the row
    // appears only with more than one terminal, and its right-click list
    // predated Minimize, Show and Rename. A help page that describes last
    // month's application is worse than none, because a reader believes it.
    Fixture f;
    f.settle();
    const std::string body = f.client.help().topic("ckmux.switcher").body;

    CK_CHECK(body.find("put away") != std::string::npos);
    CK_CHECK(body.find("only way back") != std::string::npos);
    CK_CHECK(body.find("Minimize or Show") != std::string::npos);
    CK_CHECK(body.find("Rename") != std::string::npos);
    CK_CHECK(body.find("status bar") != std::string::npos);
    // The marks are named from the widget's own table rather than spelled
    // again here, so a glyph change cannot leave the page lying.
    for (const auto status : {ckv::widgets::WindowSwitcherBar::Status::Active,
                              ckv::widgets::WindowSwitcherBar::Status::Visible,
                              ckv::widgets::WindowSwitcherBar::Status::Minimized}) {
        CK_CHECK(body.find(std::string(
                     ckv::widgets::WindowSwitcherBar::status_glyph(status))) != std::string::npos);
    }
    // And it says the half a reader cannot get from the marks alone. Since
    // ckVision D-063 the same mark stands for the terminal you are in and
    // for the ones behind it, and what tells them apart is the colour the
    // frame already uses on its own controls — a page that listed the marks
    // and stopped would describe a distinction the bar no longer draws.
    // Named here, not composed: the `_` mark is one character wide and would
    // be found in any sentence with an underscore in it, so this pair is
    // what actually holds the paragraph in place.
    CK_CHECK(body.find("window control as a mark") != std::string::npos);
    CK_CHECK(body.find("lit in the control colour") != std::string::npos);
    CK_CHECK(body.find("behind it draw the same mark plainly") != std::string::npos);
}

CK_TEST(help_all_keybindings_is_in_the_help_menu_and_opens_the_listing) {
    Fixture f;
    f.settle();
    auto* const bar = dynamic_cast<ckv::widgets::MenuBar*>(f.client.desktop().top_dock());
    CK_CHECK(bar != nullptr);
    if (bar == nullptr) return;
    const ckv::ui::CommandId all =
        f.app.commands().id_for(ckm::client::commands::kAllKeys).value_or(ckv::ui::kInvalidCommand);
    CK_CHECK(all != ckv::ui::kInvalidCommand);
    // Found by title rather than by `menus().back()`, for the reason
    // test_minimize_chrome learned the hard way: a package that adds a
    // top-level menu must not turn this into a failure about something else.
    const ckv::widgets::MenuBarItem* help_menu = nullptr;
    for (const ckv::widgets::MenuBarItem& menu : bar->menus())
        if (menu.label == "&Help") help_menu = &menu;
    CK_CHECK(help_menu != nullptr);
    if (help_menu == nullptr) return;
    bool in_help_menu = false;
    for (const ckv::widgets::MenuItem& item : help_menu->items)
        if (item.command() == all) in_help_menu = true;
    CK_CHECK(in_help_menu);

    // And it opens something the reader can read, rather than declaring a
    // command nothing answers. NOT modal, deliberately: help is consulted
    // WHILE doing the thing it explains (the interface spec), so the viewer is a window
    // the reader can leave standing rather than one that locks them out of
    // the terminal behind it.
    const std::size_t before = f.client.desktop().windows().size();
    CK_CHECK(f.app.execute_command(all));
    f.settle();
    f.settle();
    CK_CHECK(!f.app.is_modal());
    CK_CHECK(f.client.desktop().windows().size() == before + 1U);
    // Showing the complete listing rather than whatever page the focused
    // surface would have answered with.
    const ckv::widgets::Window* const opened = f.client.desktop().windows().back();
    CK_CHECK(opened != nullptr);
    if (opened == nullptr) return;
    CK_CHECK(f.client.desktop().active_window() == opened);
}

CK_TEST(no_menu_offers_one_letter_twice) {
    // A mnemonic collision makes the second item untypeable — the letter opens
    // the first one every time. It is invisible in a screenshot and invisible
    // in a click-driven test, and this package nearly shipped one: "&All
    // Keybindings…" landed in a Help menu that already had "&About ckmux…".
    //
    // Checked across every menu rather than the one this package touched,
    // because the next collision will be somebody else's and will arrive the
    // same way — by adding a perfectly reasonable item to a menu whose other
    // letters they did not look at.
    Fixture f;
    f.settle();
    auto* const bar = dynamic_cast<ckv::widgets::MenuBar*>(f.client.desktop().top_dock());
    CK_CHECK(bar != nullptr);
    if (bar == nullptr) return;

    for (const ckv::widgets::MenuBarItem& menu : bar->menus()) {
        std::string seen;
        for (const ckv::widgets::MenuItem& item : menu.items) {
            const std::string label = item.label();
            const std::size_t amp = label.find('&');
            if (amp == std::string::npos || amp + 1 >= label.size()) continue;
            const char letter = static_cast<char>(std::tolower(label[amp + 1]));
            CK_CHECK(seen.find(letter) == std::string::npos);
            seen += letter;
        }
    }

    // And the top-level titles are a menu of their own by the same rule.
    std::string bar_letters;
    for (const ckv::widgets::MenuBarItem& menu : bar->menus()) {
        const std::size_t amp = menu.label.find('&');
        if (amp == std::string::npos || amp + 1 >= menu.label.size()) continue;
        const char letter = static_cast<char>(std::tolower(menu.label[amp + 1]));
        CK_CHECK(bar_letters.find(letter) == std::string::npos);
        bar_letters += letter;
    }
}

#endif  // !defined(_WIN32)
