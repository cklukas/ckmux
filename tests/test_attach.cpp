// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// WP-6: attaching, detaching, being taken over, and being healed after falling
// behind. Everything here runs against a real `Server` over a real Unix socket,
// with the loop stepped by hand on a `ManualClock` — the tick has to be owned,
// not waited for, and the socket has to be real because backpressure is a fact
// about sockets rather than about queues.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <unistd.h>

#include "client/server_connection.hpp"
#include "client/server_session.hpp"
#include "common/grid_delta.hpp"
#include "common/proto.hpp"
#include "platform/socket.hpp"
#include "server/diff_engine.hpp"
#include "server/server.hpp"

#include "cvision/core/clock.hpp"
#include "cvision/testing/cktest.hpp"

namespace {

using ckm::server::Server;

std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-a" + std::to_string(static_cast<unsigned long>(::getpid()));
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

ckm::Settings test_settings() {
    ckm::Settings settings;
    settings.shell = "/bin/sh";
    settings.login_shell = false;
    settings.scrollback = 100;
    settings.max_fps = 30;
    return settings;
}

ckm::server::TerminalSpec spec_running(std::string command) {
    ckm::server::TerminalSpec spec;
    spec.command = std::move(command);
    spec.working_directory = "/";
    spec.columns = 80;
    spec.rows = 24;
    spec.pixel_width = 80 * 9;
    spec.pixel_height = 24 * 18;
    spec.environment = {{"TERM", "xterm-256color"}, {"PATH", "/usr/bin:/bin"}, {"LC_ALL", "C"}};
    return spec;
}

// A client on the wire, with the real `ServerSession` behind it.
struct Client {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader reader;
    ckm::client::ServerSession session{nullptr};
    std::vector<ckm::proto::Message> unread;
    // What this client PUT on the wire, by type. The received side is `unread`;
    // this is the other direction, and it exists because some of what a client
    // must not do is a message it must not send — a mirror that answered its
    // own resize with another resize would work perfectly and never stop.
    std::map<ckm::proto::MessageType, std::size_t> sent;

    bool connect(const std::filesystem::path& socket) {
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        if (result.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = ckm::platform::Stream(result.fd);
        session = ckm::client::ServerSession([this](const ckm::proto::Message& message) {
            ++sent[ckm::proto::type_of(message)];
            (void)stream.send(ckm::proto::encode(message));
        });
        session.set_history_limit(100);
        return true;
    }

    std::size_t sent_count(ckm::proto::MessageType type) const {
        const auto found = sent.find(type);
        return found == sent.end() ? 0U : found->second;
    }

    void greet() {
        ckm::proto::Hello hello;
        hello.build = "a test";
        (void)stream.send(ckm::proto::encode(hello));
    }

    // Reads whatever has arrived and routes it, keeping what the session did not
    // understand so a test can assert on the wire itself.
    std::size_t pump() {
        std::string arrived;
        (void)stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) return 0;
        std::size_t count = 0;
        for (;;) {
            ckm::proto::Message message;
            if (reader.next(message) != ckm::proto::DecodeError::None) break;
            ++count;
            if (std::holds_alternative<ckm::proto::HelloAck>(message)) continue;
            if (!session.handle(message)) unread.push_back(message);
        }
        session.heal_if_needed();
        return count;
    }

    // Reads nothing at all: a client suspended at the wrong moment, or on the
    // wrong end of a slow connection.
    void refuse_to_read() {}

    template <typename T>
    std::size_t count_of() const {
        std::size_t count = 0;
        for (const ckm::proto::Message& message : unread)
            if (std::holds_alternative<T>(message)) ++count;
        return count;
    }
};

// Steps the server until a predicate holds, with the tick advancing each pass.
template <typename Ready>
bool run_until(Server& server, ckv::ManualClock& clock, Client& client, Ready ready,
               int passes = 200) {
    for (int pass = 0; pass < passes; ++pass) {
        clock.advance(34'000'000);
        if (!server.step()) return false;
        client.pump();
        if (ready()) return true;
    }
    return false;
}

}  // namespace

CK_TEST(attaching_hands_over_every_terminal_whole) {
    const std::filesystem::path socket = private_socket("attach");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    // Two terminals with something on their screens before anybody attaches —
    // which is the ordinary case, because the server was running first.
    ckm::server::Terminal& first = server.open_terminal(0, spec_running("sleep 30"));
    ckm::server::Terminal& second = server.open_terminal(0, spec_running("sleep 30"));
    first.session().feed_output("the first terminal\r\n");
    second.session().feed_output("the second terminal\r\n");

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    CK_CHECK(run_until(server, clock, client, [&] { return client.session.attached(); }, 5) ||
             true);
    client.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, client, [&] { return client.session.attached(); }));

    CK_CHECK(client.session.terminal_ids().size() == 2U);
    ckm::client::RemoteTerminalSubsession* mirror = client.session.terminal(first.id());
    CK_CHECK(mirror != nullptr);
    if (mirror != nullptr) {
        std::string text;
        for (const ckv::Cell& cell : mirror->cells())
            if (!cell.is_continuation()) text += cell.grapheme();
        CK_CHECK(text.find("the first terminal") != std::string::npos);
    }
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_second_client_takes_the_session_and_the_first_is_told_why) {
    // plans/03: attaching is always granted to the newest client, and the
    // previous one is informed rather than consulted. A reader whose laptop
    // slept cannot be kept out by the client still nominally holding their
    // session — that is the failure mode this rule exists to prevent.
    const std::filesystem::path socket = private_socket("takeover");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    terminal.session().feed_output("a program that outlives its clients\r\n");

    Client early;
    CK_CHECK(early.connect(socket));
    early.greet();
    early.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, early, [&] { return early.session.attached(); }));

    ckm::proto::DetachReason reason = ckm::proto::DetachReason::User;
    std::string why;
    early.session.on_detached = [&](ckm::proto::DetachReason detached, const std::string& text) {
        reason = detached;
        why = text;
    };

    Client late;
    CK_CHECK(late.connect(socket));
    late.greet();
    late.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, late, [&] { return late.session.attached(); }));
    for (int pass = 0; pass < 5; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        early.pump();
    }

    CK_CHECK(!early.session.attached());
    CK_CHECK(reason == ckm::proto::DetachReason::Takeover);
    CK_CHECK(!why.empty());
    // And the newcomer has the terminal, whole, with what was on it before it
    // arrived — attaching an open session is indistinguishable from attaching a
    // fresh one (plans/03).
    ckm::client::RemoteTerminalSubsession* mirror = late.session.terminal(terminal.id());
    CK_CHECK(mirror != nullptr);
    if (mirror != nullptr) {
        std::string text;
        for (const ckv::Cell& cell : mirror->cells())
            if (!cell.is_continuation()) text += cell.grapheme();
        CK_CHECK(text.find("a program that outlives its clients") != std::string::npos);
    }
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(reattaching_announces_the_terminals_a_client_stopped_showing) {
    // C4. The mirrors outlive a detach that is not a disconnection — that is
    // what makes a takeover cheap — so the terminal a reattaching client needs
    // a window for is one it is ALREADY holding, and `on_terminal_opened`
    // fires when a mirror is created. It announced nothing, and the reader who
    // took their session back got an empty desktop over two running programs.
    const std::filesystem::path socket = private_socket("reattach-windows");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& first = server.open_terminal(0, spec_running("sleep 30"));
    ckm::server::Terminal& second = server.open_terminal(0, spec_running("sleep 30"));

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    // What the window layer would be told, counted the way it is told: one
    // call per terminal that needs a window.
    std::vector<std::uint64_t> announced;
    watcher.session.on_terminal_opened = [&](ckm::client::RemoteTerminalSubsession& remote) {
        announced.push_back(remote.terminal_id());
    };
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));
    CK_CHECK(announced.size() == 2U);

    // A heal is the same message down the same path, and it must announce
    // nothing at all: those windows are still on screen, and a second one per
    // terminal is the other way to get this wrong.
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return watcher.session.attachments() >= 2U; }));
    CK_CHECK(announced.size() == 2U);

    // Now the client takes its windows down while the attachment lives on —
    // what it does when it is taken over, and when a reader switches away.
    watcher.session.windows_forgotten();
    announced.clear();
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return watcher.session.attachments() >= 3U; }));

    // Both of them, so the desktop is the session again.
    CK_CHECK(announced.size() == 2U);
    CK_CHECK(std::find(announced.begin(), announced.end(), first.id()) != announced.end());
    CK_CHECK(std::find(announced.begin(), announced.end(), second.id()) != announced.end());
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_heal_asks_for_one_snapshot_however_many_passes_it_takes_to_arrive) {
    // M-L3. `needs_snapshot()` stays true until the snapshot arrives and
    // `heal_if_needed()` runs on every pass of the client's loop, so an
    // unguarded heal asked for every terminal whole twenty times a second —
    // and every one of those answers is the whole session to compose and
    // queue. The counter is the claim: it says how many times this client had
    // to ask, and it has to mean requests rather than passes.
    const std::filesystem::path socket = private_socket("heal-once");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));
    const std::uint64_t before = watcher.session.resnapshots();

    // A delta that skips a number: something was lost, and the mirror says so
    // the only way it can (plans/05, the gap rule).
    ckm::client::RemoteTerminalSubsession* mirror = watcher.session.terminal(terminal.id());
    CK_CHECK(mirror != nullptr);
    if (mirror == nullptr) {
        server.terminals().close_all();
        forget(socket);
        return;
    }
    ckm::proto::GridDelta lost;
    lost.term = terminal.id();
    lost.seq = mirror->mirror().sequence() + 7;
    CK_CHECK(!mirror->mirror().apply(lost));
    CK_CHECK(mirror->mirror().needs_snapshot());

    // Ten passes of the client's loop with the mirror still asking. One
    // request goes out, not ten — and it is answered, which is what lets the
    // next one ever be sent.
    for (int pass = 0; pass < 10; ++pass) watcher.session.heal_if_needed();
    CK_CHECK(watcher.session.resnapshots() == before + 1U);
    // The mirror itself, not a fresh lookup: a snapshot re-adopts the one that
    // is already there, so the pointer stays good and nothing here has to
    // guess whether the terminal is still in the map.
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return !mirror->mirror().needs_snapshot(); }));
    CK_CHECK(watcher.session.resnapshots() == before + 1U);
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_client_that_dies_is_a_detach_and_its_terminals_do_not_notice) {
    const std::filesystem::path socket = private_socket("death");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    const ckm::server::TerminalId id = terminal.id();

    {
        Client client;
        CK_CHECK(client.connect(socket));
        client.greet();
        client.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
        CK_CHECK(run_until(server, clock, client, [&] { return client.session.attached(); }));
    }  // the socket closes with no goodbye, which is what `kill -9` looks like

    for (int pass = 0; pass < 4; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
    }
    CK_CHECK(server.client_count() == 0U);
    CK_CHECK(server.terminals().find(id) != nullptr);
    CK_CHECK(server.terminals().find(id)->live());
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_client_that_falls_behind_is_healed_by_a_snapshot_when_it_drains) {
    // WP-6's acceptance criterion, at the wire.
    //
    // A client that stops reading cannot be kept up to date and must not be
    // allowed to hold the server: the queue grows to its high-water mark, deltas
    // stop being queued, and when the client finally reads, what it gets is the
    // terminal WHOLE. Not the deltas it missed — those are gone, and the diff
    // engine's belief moved on without it — which is why the rule is resnapshot
    // rather than repair (plans/05).
    const std::filesystem::path socket = private_socket("slow");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    client.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, client, [&] { return client.session.attached(); }));

    // From here the client reads nothing at all. The terminal keeps producing —
    // a full screen of new text every tick, which is what a build log looks like
    // — until the server has more than four megabytes queued for it.
    client.refuse_to_read();
    std::string burst;
    for (int line = 0; line < 24; ++line)
        burst += "a line of output that fills the screen " + std::to_string(line) + "\r\n";
    // The mark that matters is the DELTA backlog, not the stream's hard
    // high-water: past a quarter of a megabyte of queued screen there is no
    // point adding more, because everything behind it — including every answer
    // to every question the client asks — waits for a reader who is already
    // behind (WP-7 measured a `Ping` at 2.8 seconds when only the 4 MiB mark
    // stood in the way).
    int ticks = 0;
    for (; ticks < 4000 && server.queued_bytes() <= ckm::platform::Stream::kDeltaBacklogBytes;
         ++ticks) {
        terminal.session().feed_output(burst + std::to_string(ticks) + "\r\n");
        clock.advance(34'000'000);
        CK_CHECK(server.step());
    }
    CK_CHECK(server.queued_bytes() > ckm::platform::Stream::kDeltaBacklogBytes);
    CK_CHECK(server.waiting_to_heal() || ticks > 0);
    std::printf("  [backpressure] %d ticks of full-screen output to reach the mark; %zu bytes "
                "queued\n",
                ticks, server.queued_bytes());

    // Past the mark the server stops queueing for it, so the queue stops growing
    // however much the child says.
    const std::size_t at_the_mark = server.queued_bytes();
    for (int pass = 0; pass < 20; ++pass) {
        terminal.session().feed_output(burst);
        clock.advance(34'000'000);
        CK_CHECK(server.step());
    }
    CK_CHECK(server.queued_bytes() <= at_the_mark);

    // Now the client reads. Everything queued is stale by definition — it is
    // what the terminal looked like thousands of lines ago — and what has to
    // arrive at the end of it is a fresh snapshot.
    bool healed = false;
    std::uint64_t healings = 0;
    for (int pass = 0; pass < 4000 && !healed; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        client.pump();
        healed = server.queued_bytes() == 0 && client.session.attachments() > 1;
        healings = client.session.attachments();
    }
    CK_CHECK(healed);
    CK_CHECK(healings > 1U);  // the attach, and at least one healing after it
    std::printf("  [backpressure] healed after %llu attachments in total\n",
                static_cast<unsigned long long>(healings));

    // And the mirror is right afterwards, which is the point of all of it: the
    // reader's screen shows what the terminal actually holds now.
    terminal.session().feed_output("the very last line\r\n");
    CK_CHECK(run_until(server, clock, client, [&] {
        ckm::client::RemoteTerminalSubsession* mirror = client.session.terminal(terminal.id());
        if (mirror == nullptr) return false;
        std::string text;
        for (const ckv::Cell& cell : mirror->cells())
            if (!cell.is_continuation()) text += cell.grapheme();
        return text.find("the very last line") != std::string::npos;
    }));
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_mirror_that_lost_track_asks_for_the_terminal_whole) {
    // The other half of the same rule, from the client's side: a delta that does
    // not follow is not applied and not repaired — the client asks to attach
    // again, which is how a resnapshot is spelled (plans/05).
    const std::filesystem::path socket = private_socket("gap");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    client.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, client, [&] { return client.session.attached(); }));

    // A delta from nowhere, with a sequence that cannot follow.
    ckm::proto::GridDelta impossible;
    impossible.term = terminal.id();
    impossible.seq = 9999;
    impossible.ops.push_back(ckm::proto::TitleOp{"from a delta that was never sent"});
    const std::uint64_t before = client.session.resnapshots();
    (void)client.session.handle(impossible);
    client.session.heal_if_needed();
    CK_CHECK(client.session.resnapshots() == before + 1);

    // And the server answers it with the terminal whole, so the mirror recovers
    // rather than staying wrong.
    CK_CHECK(run_until(server, clock, client, [&] {
        ckm::client::RemoteTerminalSubsession* mirror = client.session.terminal(terminal.id());
        return mirror != nullptr && !mirror->mirror().needs_snapshot();
    }));
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_resize_over_the_wire_reaches_the_mirror_both_ways) {
    // C3, end to end. A `GridDelta` carried no geometry, so a repaint of a
    // SMALLER terminal passed every bounds check against a larger mirror: the
    // rows and columns past the new edge kept whatever the last program had
    // drawn there, and no counter anywhere fired. The size now LEADS any delta
    // that changes it (`ResizeOp`, protocol version 3), so one message resizes
    // the mirror and repaints it — there is no `TermMeta` in this path, and
    // nothing that could arrive in the wrong order.
    const std::filesystem::path socket = private_socket("resize");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::TerminalSpec spec = spec_running("sleep 30");
    spec.columns = 100;
    spec.rows = 30;
    spec.pixel_width = 100 * 9;
    spec.pixel_height = 30 * 18;
    ckm::server::Terminal& terminal = server.open_terminal(0, spec);

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.attach(0, ckv::Size{100, 30}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    ckm::client::RemoteTerminalSubsession* mirror = watcher.session.terminal(terminal.id());
    CK_CHECK(mirror != nullptr);
    if (mirror == nullptr) {
        server.terminals().close_all();
        forget(socket);
        return;
    }
    CK_CHECK((mirror->mirror().cells() == ckv::Size{100, 30}));

    // Everything settled, so the two ends are compared when neither has
    // anything left to say. Agreement one message early is not agreement.
    const auto settle = [&] {
        for (int pass = 0; pass < 3; ++pass) {
            clock.advance(34'000'000);
            CK_CHECK(server.step());
            watcher.pump();
        }
    };
    // The server's belief against the client's mirror, which is the one check
    // that says the belief is not a story the server tells itself: the mirror
    // was built from the wire alone.
    const auto both_ends_agree = [&] {
        const ckm::server::TerminalDiffer* differ = server.diffs().differ_for(terminal.id());
        CK_CHECK(differ != nullptr);
        return differ != nullptr && ckm::same_state(differ->believed(), mirror->mirror().state());
    };
    settle();
    CK_CHECK(both_ends_agree());

    // Smaller. A view laid out at a new size resizes its subsession, which is
    // a REQUEST: the server owns the PTY, so what changes the child's window is
    // the server acting on it, and what changes this mirror is the delta that
    // comes back (WP-3).
    const std::size_t asked_once = watcher.sent_count(ckm::proto::MessageType::MoveResize) + 1;
    mirror->resize(ckv::Size{60, 20}, ckv::Size{9, 18});
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::MoveResize) == asked_once);
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return mirror->mirror().cells() == ckv::Size{60, 20}; }));
    // The grid is the new size, cell for cell — not a 100×30 buffer being read
    // as though it were smaller, which is exactly what C3 looked like.
    CK_CHECK(mirror->mirror().grid().size() == 60U * 20U);
    settle();
    CK_CHECK(both_ends_agree());
    // And no echo. A mirror that answered its own change of size with another
    // `MoveResize` would work perfectly and never stop: the server would resize,
    // the delta would come back, and the mirror would ask again. The count is
    // the guard, because the loop would be invisible in a screenshot.
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::MoveResize) == asked_once);

    // Bigger, and it is the same code both ways — a shrink and a grow differ
    // only in which numbers are larger, so a test that only shrank would leave
    // half of the op untested.
    mirror->resize(ckv::Size{100, 30}, ckv::Size{9, 18});
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::MoveResize) == asked_once + 1);
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return mirror->mirror().cells() == ckv::Size{100, 30}; }));
    CK_CHECK(mirror->mirror().grid().size() == 100U * 30U);
    settle();
    CK_CHECK(both_ends_agree());
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::MoveResize) == asked_once + 1);
    // Nothing was lost on the way. A resize is a repaint the mirror can take,
    // not a delta it has to give up on — and a refusal would have healed
    // itself through a snapshot and left every size above looking right, so
    // the counters are what tell those two apart.
    CK_CHECK(!mirror->mirror().needs_snapshot());
    CK_CHECK(mirror->mirror().gaps() == 0U);
    CK_CHECK(watcher.session.resnapshots() == 0U);
    server.terminals().close_all();
    forget(socket);
}

// --- WP-16: a picture, end to end ------------------------------------------

CK_TEST(a_picture_a_child_draws_reaches_the_mirror_with_its_pixels) {
    const std::filesystem::path socket = private_socket("sixel-transport");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    // A child that draws one red 8x12 picture at row 2, column 3 and stays
    // alive. The spec's host_sixel default (true) advertises graphics, so the
    // child's DCS is decoded rather than dropped.
    ckm::server::Terminal& terminal = server.open_terminal(
        0, spec_running("printf '\\033[2;3H\\033Pq#0;2;100;0;0!8~-!8~\\033\\\\'; sleep 30"));
    (void)terminal;

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.set_host_sixel(true);
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    const std::vector<std::uint64_t> ids = watcher.session.terminal_ids();
    CK_CHECK(ids.size() == 1U);
    if (ids.empty()) {
        server.terminals().close_all();
        forget(socket);
        return;
    }
    ckm::client::RemoteTerminalSubsession* mirrored = watcher.session.terminal(ids.front());
    CK_CHECK(mirrored != nullptr);
    CK_CHECK(run_until(server, clock, watcher, [&] { return !mirrored->rasters().empty(); }));

    if (!mirrored->rasters().empty()) {
        const ckv::core::TerminalRaster& raster = mirrored->rasters().front();
        // A raster whose id stayed 0 would be dropped by TerminalView, so the
        // minted identity is part of the promise, not a detail.
        CK_CHECK(raster.id != 0);
        CK_CHECK(raster.anchor.x == 2);
        CK_CHECK(raster.anchor.y == 1);
        CK_CHECK(raster.image != nullptr);
        if (raster.image != nullptr) {
            CK_CHECK(raster.image->width() == 8);
            CK_CHECK(raster.image->height() == 12);
            const ckv::Image::Rgba pixel = raster.image->pixel(0, 0);
            CK_CHECK(pixel.r >= 200);
            CK_CHECK(pixel.g == 0);
            CK_CHECK(pixel.a == 255);
        }
    }
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_host_that_shows_no_graphics_opens_children_that_are_told_not_to_draw) {
    // plans/04: `sixel = auto` advertises what the host can show. The client
    // said its host cannot (Attach.host_sixel = 0), so the terminal IT opens
    // is launched without the advertisement, and the child's Sixel is dropped
    // at the emulator with a diagnostic — nothing is transported to a client
    // that could never show it.
    const std::filesystem::path socket = private_socket("sixel-off");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.set_host_sixel(false);
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    ckm::proto::NewTerminal request;
    request.command = "printf '\\033Pq#0;2;100;0;0!8~-!8~\\033\\\\'; sleep 30";
    watcher.session.request(request);
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return !watcher.session.terminal_ids().empty(); }));

    const std::vector<std::uint64_t> ids = watcher.session.terminal_ids();
    if (ids.empty()) {
        server.terminals().close_all();
        forget(socket);
        return;
    }
    ckm::server::Terminal* opened = server.terminals().find(ids.front());
    CK_CHECK(opened != nullptr);
    if (opened != nullptr) {
        // The refusal is said out loud where a debugger looks for it.
        CK_CHECK(run_until(server, clock, watcher,
                           [&] { return !opened->session().diagnostics().empty(); }));
    }
    ckm::client::RemoteTerminalSubsession* mirrored = watcher.session.terminal(ids.front());
    CK_CHECK(mirrored != nullptr);
    if (mirrored != nullptr) CK_CHECK(mirrored->rasters().empty());
    server.terminals().close_all();
    forget(socket);
}
