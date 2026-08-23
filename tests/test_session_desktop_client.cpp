// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-43 on the client: the session's desktop is the WORLD, this client's
// screen is a VIEW over it, and the pan between them is this client's alone.
// The session layer learns the world from the messages that state it and from
// nothing else; the window layer becomes an extent-and-pan consumer of U7-a,
// where panning follows the focused window and moves paint offsets, never a
// window's rect — which is why an hour of panning must leave the layout the
// server holds byte-identical.
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include "client/client_app.hpp"
#include "client/server_session.hpp"
#include "common/proto.hpp"

#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"

namespace {

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckm::client::ServerSession;
using ckm::client::WindowPlacement;

ckm::proto::Attached attached_to(std::uint64_t session, std::uint16_t columns,
                                 std::uint16_t rows) {
    ckm::proto::Attached attached;
    attached.session = session;
    attached.snapshot.desktop_columns = columns;
    attached.snapshot.desktop_rows = rows;
    return attached;
}

}  // namespace

// --- The session layer: what the wire says, and only what it says ---------

CK_TEST(the_attach_snapshot_teaches_the_client_the_sessions_desktop) {
    ServerSession session([](const ckm::proto::Message&) {});
    std::vector<ckv::Size> announced;
    session.on_session_desktop = [&announced](ckv::Size world) { announced.push_back(world); };

    CK_CHECK(session.session_desktop() == (ckv::Size{0, 0}));
    CK_CHECK(session.handle(attached_to(7, 200, 60)));
    CK_CHECK(session.session_desktop() == (ckv::Size{200, 60}));
    CK_CHECK(announced.size() == 1U);

    // Restating the same world says nothing new.
    CK_CHECK(session.handle(attached_to(7, 200, 60)));
    CK_CHECK(announced.size() == 1U);
}

CK_TEST(a_layout_delta_restates_the_world_and_a_foreign_sessions_does_not) {
    ServerSession session([](const ckm::proto::Message&) {});
    std::vector<ckv::Size> announced;
    session.on_session_desktop = [&announced](ckv::Size world) { announced.push_back(world); };
    CK_CHECK(session.handle(attached_to(7, 200, 60)));

    // The reader resized the session (WP-40); the statement rides the same
    // message the arrangement does.
    ckm::proto::LayoutDelta grown;
    grown.session = 7;
    grown.desktop_columns = 240;
    grown.desktop_rows = 72;
    CK_CHECK(session.handle(ckm::proto::Message{grown}));
    CK_CHECK(session.session_desktop() == (ckv::Size{240, 72}));
    CK_CHECK(announced.size() == 2U);

    // Another session's arrangement describes a world this client is not in.
    ckm::proto::LayoutDelta foreign;
    foreign.session = 9;
    foreign.desktop_columns = 80;
    foreign.desktop_rows = 24;
    CK_CHECK(session.handle(ckm::proto::Message{foreign}));
    CK_CHECK(session.session_desktop() == (ckv::Size{240, 72}));
    CK_CHECK(announced.size() == 2U);
}

CK_TEST(zeros_are_a_server_that_has_not_said_and_shrink_nothing) {
    ServerSession session([](const ckm::proto::Message&) {});
    CK_CHECK(session.handle(attached_to(7, 200, 60)));

    ckm::proto::LayoutDelta silent;
    silent.session = 7;
    CK_CHECK(session.handle(ckm::proto::Message{silent}));
    CK_CHECK(session.session_desktop() == (ckv::Size{200, 60}));

    // And this client's own resize is the VIEW: it says nothing about the
    // world, however often it is stated (WP-40's rule, read from this side).
    session.desktop_resized(ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(session.session_desktop() == (ckv::Size{200, 60}));
}

CK_TEST(the_world_goes_with_the_connection) {
    ServerSession session([](const ckm::proto::Message&) {});
    CK_CHECK(session.handle(attached_to(7, 200, 60)));
    session.connection_lost();
    CK_CHECK(session.session_desktop() == (ckv::Size{0, 0}));
}

// --- The window layer: extent, pan-follows-focus, and rects that hold ------

namespace {

struct PanFixture {
    ckv::term::HeadlessTerminal terminal{ckv::Size{100, 30}};
    ckv::ManualClock clock;
    ckv::ui::Application app{terminal, clock};
    std::vector<std::vector<WindowPlacement>> reports;
    ClientApp client;

    PanFixture()
        : client{app, [this] {
                     ClientOptions options;
                     options.settings.shell = "/bin/cat";
                     options.report_layout = [this](const std::vector<WindowPlacement>& placed) {
                         reports.push_back(placed);
                     };
                     return options;
                 }()} {
        app.step(0);
    }
};

}  // namespace

CK_TEST(the_world_becomes_the_extent_and_focus_pans_a_small_client_around_it) {
    PanFixture f;
    f.client.set_session_desktop(ckv::Size{200, 60});
    CK_CHECK(f.client.desktop().extent() == (ckv::Size{200, 60}));

    // A second terminal, placed far beyond this client's own screen — a rect
    // the session owns and a 100×30 view cannot show from its origin.
    CK_CHECK(f.app.execute_command(
        f.app.commands().id_for(ckm::client::commands::kNewTerminal).value_or(
            ckv::ui::kInvalidCommand)));
    f.app.step(0);
    CK_CHECK(f.client.desktop().windows().size() == 2U);
    ckv::widgets::Window* const far_window = f.client.desktop().windows()[1];
    ckv::widgets::Window* const near_window = f.client.desktop().windows()[0];
    near_window->set_bounds(ckv::Rect{2, 3, 40, 10});
        far_window->set_bounds(ckv::Rect{150, 45, 40, 10});

    // The far window was created last and is therefore already active;
    // activating the ACTIVE window is a no-event by design. Start from the
    // near one, so each switch below is a real activation.
    f.client.desktop().activate(near_window);

    // Focusing the far terminal pans the view to it: the reader meant to see
    // it. `pan()` is the read U7-a exposes so this is asserted, not inferred.
    f.client.desktop().activate(far_window);
    const ckv::Point toward_far = f.client.desktop().pan();
    CK_CHECK(toward_far.x >= 90);  // 150+40 must fit into a 100-wide view
    CK_CHECK(toward_far.y > 0);

    // And back: focusing the near one returns the view toward the origin.
    f.client.desktop().activate(near_window);
    const ckv::Point toward_near = f.client.desktop().pan();
    CK_CHECK(toward_near.x <= 2);
    CK_CHECK(toward_near.y <= 3);

    // Through it all, nobody's window moved: the rects the session owns are
    // exactly where their readers put them.
    CK_CHECK(near_window->bounds() == (ckv::Rect{2, 3, 40, 10}));
    CK_CHECK(far_window->bounds() == (ckv::Rect{150, 45, 40, 10}));
}

CK_TEST(panning_alone_reports_nothing_and_moves_nothing) {
    PanFixture f;
    f.client.set_session_desktop(ckv::Size{200, 60});
    ckv::widgets::Window* const window = f.client.desktop().windows()[0];
    const ckv::Rect placed = window->bounds();

    // Let the settle machinery deliver the INITIAL placement report first —
    // it fires regardless of anything this test does, and counting it against
    // panning was this test's own first bug.
    for (int i = 0; i < 8; ++i) {
        f.clock.advance(200'000'000);
        f.app.step(0);
    }
    const std::size_t reports_before = f.reports.size();
    // A pure pan — the reader looking elsewhere, no activation, no gesture.
    f.client.desktop().set_pan(ckv::Point{60, 20});
    f.client.desktop().set_pan(ckv::Point{0, 0});
    f.client.desktop().set_pan(ckv::Point{31, 7});
    // Give the settle machinery every chance to misreport: real settle
    // intervals pass on the injected clock, with steps between them.
    for (int i = 0; i < 8; ++i) {
        f.clock.advance(200'000'000);
        f.app.step(0);
    }
    CK_CHECK(window->bounds() == placed);
    // The layout the server would hold is byte-identical to never having
    // panned: no report was even sent, because nothing a report carries
    // changed. This is WP-43's core claim, and the `entries == layout_sent_`
    // early-out makes a leaked offset INTERMITTENT — so the assertion is
    // "nothing was reported", not "what was reported looked right".
    CK_CHECK(f.reports.size() == reports_before);
}

CK_TEST(leaving_the_session_returns_a_desktop_that_is_its_own_viewport) {
    PanFixture f;
    f.client.set_session_desktop(ckv::Size{200, 60});
    f.client.desktop().set_pan(ckv::Point{50, 20});
    f.client.forget_terminals();
    CK_CHECK(f.client.desktop().extent() == (ckv::Size{0, 0}));
    CK_CHECK(f.client.desktop().pan() == (ckv::Point{0, 0}));
}

// --- The picker's row line: how many readers, and which of them is you ----

CK_TEST(a_picker_row_says_how_many_readers_a_session_has) {
    // WP-48's third defect, at the edge where it lived: `SessionInfo::attached`
    // has been a COUNT since WP-44 and the client narrowed it to a bool on
    // arrival, so this line could say a session was busy and never say how
    // busy. "In use" is the one thing a reader can already guess; whether
    // joining puts them in a room with one person or three is what they cannot.
    using ckm::client::session_row_label;
    using ckm::client::SessionRow;

    // Nobody watching: no parenthetical at all, because there is nothing to say.
    CK_CHECK(session_row_label(SessionRow{7, "build", 2, 0}, 0) == "build — 2 terminals");

    // One reader, and it is not this client — attaching is a takeover, and the
    // line says so in the same breath as the count.
    CK_CHECK(session_row_label(SessionRow{7, "build", 1, 1}, 0) ==
             "build — 1 terminal  (1 reader — attaching takes it over)");
    CK_CHECK(session_row_label(SessionRow{7, "build", 3, 4}, 0) ==
             "build — 3 terminals  (4 readers — attaching takes it over)");

    // The row this client holds. The server counts THIS reader among the
    // session's own, so a row reporting its own reader as company would tell
    // every single-client reader that somebody else was in the room with them.
    CK_CHECK(session_row_label(SessionRow{7, "build", 2, 1}, 7) == "build — 2 terminals  (this client)");
    CK_CHECK(session_row_label(SessionRow{7, "build", 2, 2}, 7) ==
             "build — 2 terminals  (this client, and 1 other reader)");
    CK_CHECK(session_row_label(SessionRow{7, "build", 2, 3}, 7) ==
             "build — 2 terminals  (this client, and 2 other readers)");

    // A session with no name is still pointed at by something.
    CK_CHECK(session_row_label(SessionRow{9, "", 1, 0}, 0) == "session 9 — 1 terminal");
}

CK_TEST(the_watched_row_never_reports_a_reader_it_cannot_have) {
    // A guard on the subtraction rather than on the sentence. `readers` is what
    // the server said and `watched` is what this client believes, and the two
    // are separately timed: a list in flight while this client attached, or a
    // stale row for a session it has just left, can put a 0 next to the id it
    // holds. Saturating rather than wrapping is the difference between "(this
    // client)" and a row claiming 4294967295 other readers.
    using ckm::client::session_row_label;
    using ckm::client::SessionRow;
    CK_CHECK(session_row_label(SessionRow{7, "build", 1, 0}, 7) == "build — 1 terminal  (this client)");
}
