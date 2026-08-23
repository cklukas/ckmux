// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// A session's desktop is the session's (WP-40, the session model "Two clients at once").
//
// A terminal is a window on a virtual desktop, and a PTY's size is that
// window's content rect — so no client's screen appears anywhere in the chain
// between a reader's terminal and a child's `TIOCSWINSZ`. That is the property
// these cases pin, because it is what lets two readers of different sizes watch
// one session without either of them reflowing the other's windows or
// SIGWINCHing anybody's child.
//
// It also fixes something that was quietly useless: `Snapshot` and
// `LayoutDelta` have always carried `desktop_columns`/`desktop_rows`, and the
// server filled both from the client it was talking to — telling a client how
// big its own terminal is, which it already knew.
#if !defined(_WIN32)

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "common/config.hpp"
#include "common/proto.hpp"
#include "cvision/testing/cktest.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"
#include "server/terminals.hpp"

namespace {

using ckm::proto::Message;

std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-desktop" + std::to_string(static_cast<unsigned long>(::getpid()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (std::string(name) + ".sock");
}

void forget(const std::filesystem::path& socket) {
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    std::filesystem::remove(socket.parent_path(), ignored);
}

ckm::Settings test_settings(ckm::DesktopSizePolicy policy = ckm::DesktopSizePolicy::Fixed) {
    ckm::Settings settings;
    settings.shell = "/bin/sh";
    settings.login_shell = false;
    settings.scrollback = 100;
    settings.max_fps = 30;
    settings.desktop_size = policy;
    return settings;
}

struct WireClient {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader reader;

    bool connect(const std::filesystem::path& socket) {
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        if (result.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = ckm::platform::Stream(result.fd);
        return true;
    }
    void say(const Message& message) { (void)stream.send(ckm::proto::encode(message)); }
    bool take(Message& message) {
        std::string arrived;
        (void)stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) return false;
        return reader.next(message) == ckm::proto::DecodeError::None;
    }
    void greet() {
        ckm::proto::Hello hello;
        hello.build = std::string(ckm::proto::kBuildIdentity);
        say(hello);
    }
    void attach(std::uint16_t columns, std::uint16_t rows) {
        ckm::proto::Attach request;
        request.session = 0;
        request.columns = columns;
        request.rows = rows;
        request.pixel_width = static_cast<std::uint16_t>(columns * 9);
        request.pixel_height = static_cast<std::uint16_t>(rows * 18);
        say(request);
    }
};

// Steps the server until this client has an `Attached`, and answers with the
// desktop the snapshot named — which is the whole observable of this package.
bool attached_desktop(ckm::server::Server& server, WireClient& client, ckv::Size& desktop,
                      int passes = 16) {
    for (int pass = 0; pass < passes; ++pass) {
        if (!server.step()) return false;
        Message message;
        while (client.take(message)) {
            const auto* attached = std::get_if<ckm::proto::Attached>(&message);
            if (attached == nullptr) continue;
            desktop = ckv::Size{attached->snapshot.desktop_columns,
                                attached->snapshot.desktop_rows};
            return true;
        }
    }
    return false;
}

// The desktop as the server ANNOUNCES it, rather than as a fresh attach reports
// it. Reading it from `LayoutDelta` is both how a real client learns the
// coordinate space moved and the only way to observe a change without
// attaching another client — and attaching is itself an act that can move the
// desktop, so the old test's "ask by re-attaching" could not distinguish the
// answer from the question under two of the three policies.
bool announced_desktop(ckm::server::Server& server, WireClient& client, ckv::Size& desktop,
                       int passes = 16) {
    for (int pass = 0; pass < passes; ++pass) {
        if (!server.step()) return false;
        Message message;
        while (client.take(message)) {
            const auto* layout = std::get_if<ckm::proto::LayoutDelta>(&message);
            if (layout == nullptr) continue;
            if (layout->desktop_columns == 0 || layout->desktop_rows == 0) continue;
            desktop = ckv::Size{layout->desktop_columns, layout->desktop_rows};
            return true;
        }
    }
    return false;
}

}  // namespace

CK_TEST(the_first_client_to_attach_sets_the_sessions_desktop) {
    // A session made by `ckmux new` has no screen of its own to take a size
    // from, so the first reader to arrive supplies one. After that it is the
    // session's.
    const std::filesystem::path socket = private_socket("first");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient reader;
    CK_CHECK(reader.connect(socket));
    reader.greet();
    reader.attach(100, 30);

    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, reader, desktop));
    CK_CHECK(desktop.width == 100 && desktop.height == 30);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_second_client_of_another_size_does_not_move_the_desktop) {
    // The rule the whole package exists for. Under the default policy a
    // reader arriving on a laptop does not reflow the windows of a session
    // arranged on a large screen — and does not SIGWINCH a single child in it.
    const std::filesystem::path socket = private_socket("fixed");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient wide;
    CK_CHECK(wide.connect(socket));
    wide.greet();
    wide.attach(200, 60);
    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, wide, desktop));
    CK_CHECK(desktop.width == 200 && desktop.height == 60);

    WireClient narrow;
    CK_CHECK(narrow.connect(socket));
    narrow.greet();
    narrow.attach(80, 24);
    ckv::Size after{0, 0};
    CK_CHECK(attached_desktop(server, narrow, after));
    // The newcomer is TOLD the session's desktop, not its own screen. What it
    // does about the difference is its business (WP-43); what it must not do
    // is change everybody else's coordinate space by arriving.
    CK_CHECK(after.width == 200 && after.height == 60);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(fit_smallest_shrinks_for_a_newcomer_and_stays_shrunk) {
    // Offered for readers arriving from tmux, and deliberately STICKY: it
    // shrinks to a newcomer and does not grow back when they leave. A session
    // whose geometry oscillates with people's attach cycles is worse than one
    // that is merely too small — every oscillation is a reflow and a SIGWINCH
    // storm through every child.
    const std::filesystem::path socket = private_socket("smallest");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(
        ckm::server::Server::Options{socket, test_settings(ckm::DesktopSizePolicy::FitSmallest)},
        clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient wide;
    CK_CHECK(wide.connect(socket));
    wide.greet();
    wide.attach(200, 60);
    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, wide, desktop));
    CK_CHECK(desktop.width == 200 && desktop.height == 60);

    {
        WireClient narrow;
        CK_CHECK(narrow.connect(socket));
        narrow.greet();
        narrow.attach(80, 24);
        ckv::Size shrunk{0, 0};
        CK_CHECK(attached_desktop(server, narrow, shrunk));
        CK_CHECK(shrunk.width == 80 && shrunk.height == 24);
    }  // and the small reader goes

    WireClient wide_again;
    CK_CHECK(wide_again.connect(socket));
    wide_again.greet();
    wide_again.attach(200, 60);
    ckv::Size after{0, 0};
    CK_CHECK(attached_desktop(server, wide_again, after));
    CK_CHECK(after.width == 80 && after.height == 24);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(fit_latest_takes_whoever_arrived_last) {
    const std::filesystem::path socket = private_socket("latest");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(
        ckm::server::Server::Options{socket, test_settings(ckm::DesktopSizePolicy::FitLatest)},
        clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient wide;
    CK_CHECK(wide.connect(socket));
    wide.greet();
    wide.attach(200, 60);
    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, wide, desktop));

    WireClient narrow;
    CK_CHECK(narrow.connect(socket));
    narrow.greet();
    narrow.attach(80, 24);
    ckv::Size after{0, 0};
    CK_CHECK(attached_desktop(server, narrow, after));
    CK_CHECK(after.width == 80 && after.height == 24);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_sole_readers_resize_moves_the_desktop_because_nobody_else_is_here) {
    // THE CASE THAT WAS ASSERTED BACKWARDS. This test previously required that
    // a `ClientResize` change nothing about the session, under every policy,
    // and it made that requirement with ONE reader attached. So the defect a
    // reader actually hits — resize your terminal, and half your windows are
    // cut off with no way to reach them, because the desktop is still the size
    // your terminal happened to be at first attach — was not merely missed by
    // the suite. It was pinned by it. Field report, 2026-08-20.
    //
    // WP-40's rule is sound and survives below: a reader must not reflow
    // somebody else's arrangement by dragging their own window corner. But
    // "somebody else" has to exist for that to protect anyone, and in the
    // ordinary session it does not.
    //
    // Under EVERY policy, because with one reader there is no competing claim
    // for the policy to arbitrate: the policy decides between readers, and
    // there is only one.
    for (const ckm::DesktopSizePolicy policy :
         {ckm::DesktopSizePolicy::Fixed, ckm::DesktopSizePolicy::FitSmallest,
          ckm::DesktopSizePolicy::FitLatest}) {
        const std::filesystem::path socket = private_socket("resize-sole");
        forget(socket);
        ckv::ManualClock clock;
        ckm::server::Server server(ckm::server::Server::Options{socket, test_settings(policy)},
                                   clock);
        CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

        WireClient reader;
        CK_CHECK(reader.connect(socket));
        reader.greet();
        reader.attach(120, 40);
        ckv::Size desktop{0, 0};
        CK_CHECK(attached_desktop(server, reader, desktop));
        CK_CHECK(desktop.width == 120 && desktop.height == 40);

        // Smaller: the shape of the report. A reader who shrinks their host
        // terminal must not be left with windows beyond its edge.
        ckm::proto::ClientResize smaller;
        smaller.columns = 60;
        smaller.rows = 20;
        smaller.pixel_width = 60 * 9;
        smaller.pixel_height = 20 * 18;
        reader.say(smaller);
        ckv::Size shrunk{0, 0};
        CK_CHECK(announced_desktop(server, reader, shrunk));
        CK_CHECK(shrunk.width == 60 && shrunk.height == 20);

        // And larger, which is the half a shrink-only fix would leave broken:
        // the reader who makes their window bigger must get the space.
        ckm::proto::ClientResize bigger;
        bigger.columns = 200;
        bigger.rows = 60;
        bigger.pixel_width = 200 * 9;
        bigger.pixel_height = 60 * 18;
        reader.say(bigger);
        ckv::Size grown{0, 0};
        CK_CHECK(announced_desktop(server, reader, grown));
        CK_CHECK(grown.width == 200 && grown.height == 60);

        server.terminals().close_all();
        forget(socket);
    }
}

CK_TEST(a_resize_leaves_the_desktop_alone_once_a_second_reader_is_watching) {
    // WP-40's actual rule, kept: with somebody else attached, one reader's
    // screen must not reflow the session under them. This is the case the old
    // test meant to make and did not — it never attached a second reader while
    // the resize happened.
    //
    // Under `Fixed`, the default. The other two policies deliberately follow a
    // client's size and are covered where they are configured.
    const std::filesystem::path socket = private_socket("resize-shared");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(
        ckm::server::Server::Options{socket, test_settings(ckm::DesktopSizePolicy::Fixed)}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient wide;
    CK_CHECK(wide.connect(socket));
    wide.greet();
    wide.attach(200, 60);
    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, wide, desktop));
    CK_CHECK(desktop.width == 200 && desktop.height == 60);

    // WITH THE SHARE OPT-IN, and that detail is the test. A plain `Attach`
    // TAKES the session — the previous holder is detached (the session model) — so two
    // clients connecting without it leaves exactly one reader, and the case
    // would quietly become the sole-reader case above while looking like it
    // covered sharing. That is how the first draft of this test failed: it
    // asserted the desktop had not moved, in a session where the fix was
    // correctly moving it for the one reader left.
    WireClient narrow;
    CK_CHECK(narrow.connect(socket));
    narrow.greet();
    ckm::proto::Attach join;
    join.session = 0;
    join.columns = 80;
    join.rows = 24;
    join.pixel_width = 80 * 9;
    join.pixel_height = 24 * 18;
    join.share = 1;
    narrow.say(join);
    ckv::Size joined{0, 0};
    CK_CHECK(attached_desktop(server, narrow, joined));
    CK_CHECK(joined.width == 200 && joined.height == 60);

    // Now the second reader resizes. Two readers are attached, so this is
    // exactly the act WP-40 forbids.
    ckm::proto::ClientResize resize;
    resize.columns = 60;
    resize.rows = 20;
    resize.pixel_width = 60 * 9;
    resize.pixel_height = 20 * 18;
    narrow.say(resize);
    for (int pass = 0; pass < 8; ++pass) (void)server.step();

    // Asked of a third arrival, because that is the one observation that does
    // not perturb what it measures — and asked at the SAME size the session
    // already has, so that a policy which follows a newcomer cannot make this
    // assertion pass by coincidence.
    WireClient third;
    CK_CHECK(third.connect(socket));
    third.greet();
    third.attach(200, 60);
    ckv::Size after{0, 0};
    CK_CHECK(attached_desktop(server, third, after));
    CK_CHECK(after.width == 200 && after.height == 60);

    server.terminals().close_all();
    forget(socket);
}


// --- The one path that DOES change it (WP-40) ------------------------------

CK_TEST(a_reader_who_asks_gets_the_desktop_resized_for_everybody) {
    // `SetDesktopSize` is the act, as against `ClientResize`'s fact. It is the
    // only message that changes the coordinate space a session's windows are
    // arranged in, and it exists so that doing so is something a reader chose
    // rather than something their window manager did to them.
    const std::filesystem::path socket = private_socket("fit");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient reader;
    CK_CHECK(reader.connect(socket));
    reader.greet();
    reader.attach(200, 60);
    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, reader, desktop));
    CK_CHECK(desktop.width == 200 && desktop.height == 60);

    ckm::proto::SetDesktopSize fit;
    fit.columns = 90;
    fit.rows = 30;
    reader.say(fit);
    for (int pass = 0; pass < 8; ++pass) (void)server.step();

    // Asked for, so it moved — and the next client to arrive is told the new
    // space rather than the old one, under the `fixed` policy that otherwise
    // leaves it alone.
    WireClient later;
    CK_CHECK(later.connect(socket));
    later.greet();
    later.attach(200, 60);
    ckv::Size after{0, 0};
    CK_CHECK(attached_desktop(server, later, after));
    CK_CHECK(after.width == 90 && after.height == 30);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_nonsensical_fit_is_refused_rather_than_applied) {
    // Zero columns is not a desktop. A server that took it would leave every
    // window in the session clamped into nothing, and the reader who asked
    // would have no way to say it back.
    const std::filesystem::path socket = private_socket("zero");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient reader;
    CK_CHECK(reader.connect(socket));
    reader.greet();
    reader.attach(120, 40);
    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, reader, desktop));

    ckm::proto::SetDesktopSize fit;
    fit.columns = 0;
    fit.rows = 0;
    reader.say(fit);
    for (int pass = 0; pass < 8; ++pass) (void)server.step();

    WireClient later;
    CK_CHECK(later.connect(socket));
    later.greet();
    later.attach(200, 60);
    ckv::Size after{0, 0};
    CK_CHECK(attached_desktop(server, later, after));
    CK_CHECK(after.width == 120 && after.height == 40);

    server.terminals().close_all();
    forget(socket);
}


// --- Two readers at once (WP-44) -------------------------------------------
//
// The opt-in, and what the second client is owed at the moment it arrives: a
// snapshot of its own, and no `Detached` for the reader already there. Without
// the flag nothing changes — takeover is the contract and stays the default.

namespace {

// Attaches with the share opt-in, and answers whether this client got its own
// `Attached` — which is what "a snapshot each" means on the wire.
bool attach_sharing(ckm::server::Server& server, WireClient& client, std::uint16_t columns,
                    std::uint16_t rows) {
    ckm::proto::Attach request;
    request.session = 0;
    request.columns = columns;
    request.rows = rows;
    request.share = 1;
    client.say(request);
    ckv::Size ignored{0, 0};
    return attached_desktop(server, client, ignored);
}

}  // namespace

CK_TEST(two_clients_may_share_one_session_when_both_ask) {
    const std::filesystem::path socket = private_socket("share");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient first;
    CK_CHECK(first.connect(socket));
    first.greet();
    CK_CHECK(attach_sharing(server, first, 120, 40));

    // The first reader must not be told anything: nobody took their session.
    bool first_detached = false;
    WireClient second;
    CK_CHECK(second.connect(socket));
    second.greet();
    CK_CHECK(attach_sharing(server, second, 120, 40));

    for (int pass = 0; pass < 8; ++pass) {
        (void)server.step();
        Message message;
        while (first.take(message))
            if (std::holds_alternative<ckm::proto::Detached>(message)) first_detached = true;
    }
    CK_CHECK(!first_detached);

    // And the session says how many readers it has, which is the one fact
    // about simultaneity a reader ever sees.
    first.say(ckm::proto::ListSessions{});
    std::uint8_t attached = 0;
    for (int pass = 0; pass < 8; ++pass) {
        (void)server.step();
        Message message;
        while (first.take(message)) {
            const auto* list = std::get_if<ckm::proto::SessionList>(&message);
            if (list == nullptr || list->sessions.empty()) continue;
            attached = list->sessions.front().attached;
        }
    }
    CK_CHECK(attached == 2);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_client_that_does_not_ask_still_takes_the_session) {
    // The default path, unchanged and load-bearing: a reader whose laptop slept
    // cannot be kept out by the client nominally holding their session, and
    // that is a product requirement rather than a v1 simplification.
    const std::filesystem::path socket = private_socket("still-takes");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient held;
    CK_CHECK(held.connect(socket));
    held.greet();
    CK_CHECK(attach_sharing(server, held, 120, 40));

    WireClient taker;
    CK_CHECK(taker.connect(socket));
    taker.greet();
    taker.attach(120, 40);  // no share flag
    ckv::Size ignored{0, 0};
    CK_CHECK(attached_desktop(server, taker, ignored));

    bool told = false;
    ckm::proto::DetachReason why = ckm::proto::DetachReason::User;
    for (int pass = 0; pass < 8; ++pass) {
        (void)server.step();
        Message message;
        while (held.take(message)) {
            const auto* detached = std::get_if<ckm::proto::Detached>(&message);
            if (detached == nullptr) continue;
            told = true;
            why = detached->reason;
        }
    }
    CK_CHECK(told);
    CK_CHECK(why == ckm::proto::DetachReason::Takeover);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(one_reader_leaving_leaves_the_other_watching) {
    const std::filesystem::path socket = private_socket("one-leaves");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient staying;
    CK_CHECK(staying.connect(socket));
    staying.greet();
    CK_CHECK(attach_sharing(server, staying, 120, 40));
    {
        WireClient leaving;
        CK_CHECK(leaving.connect(socket));
        leaving.greet();
        CK_CHECK(attach_sharing(server, leaving, 120, 40));
    }  // its socket closes
    for (int pass = 0; pass < 8; ++pass) (void)server.step();

    // The session is still watched, and still says so.
    staying.say(ckm::proto::ListSessions{});
    std::uint8_t attached = 0;
    for (int pass = 0; pass < 8; ++pass) {
        (void)server.step();
        Message message;
        while (staying.take(message)) {
            const auto* list = std::get_if<ckm::proto::SessionList>(&message);
            if (list == nullptr || list->sessions.empty()) continue;
            attached = list->sessions.front().attached;
        }
    }
    CK_CHECK(attached == 1);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(both_readers_hear_about_a_terminal_that_opens) {
    // The fan-out itself: `client_attached_to` returned the FIRST watcher and
    // was called at twelve broadcast sites, every one of which would have left
    // the second reader looking at a screen that stopped changing.
    const std::filesystem::path socket = private_socket("both-hear");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient first;
    WireClient second;
    CK_CHECK(first.connect(socket));
    first.greet();
    CK_CHECK(attach_sharing(server, first, 120, 40));
    CK_CHECK(second.connect(socket));
    second.greet();
    CK_CHECK(attach_sharing(server, second, 120, 40));

    ckm::proto::NewTerminal ask;
    ask.command = "/bin/sh";
    first.say(ask);

    bool first_heard = false;
    bool second_heard = false;
    for (int pass = 0; pass < 20; ++pass) {
        (void)server.step();
        Message message;
        while (first.take(message))
            if (std::holds_alternative<ckm::proto::TermOpened>(message)) first_heard = true;
        while (second.take(message))
            if (std::holds_alternative<ckm::proto::TermOpened>(message)) second_heard = true;
    }
    CK_CHECK(first_heard);
    CK_CHECK(second_heard);

    server.terminals().close_all();
    forget(socket);
}

#endif  // !defined(_WIN32)
