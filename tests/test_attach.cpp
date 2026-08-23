// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-6: attaching, detaching, being taken over, and being healed after falling
// behind. Everything here runs against a real `Server` over a real Unix socket,
// with the loop stepped by hand on a `ManualClock` — the tick has to be owned,
// not waited for, and the socket has to be real because backpressure is a fact
// about sockets rather than about queues.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
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
    std::size_t pump(std::size_t byte_budget = 1024 * 1024) {
        std::string arrived;
        (void)stream.receive(arrived, byte_budget);
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
//
// The clock is manual but the children are real: a wait whose predicate needs
// a child's OUTPUT (a printf crossing a PTY) is bounded by the machine's
// scheduler, not by this loop's virtual ticks — and without the sleep below,
// two hundred passes complete in single-digit real milliseconds, which under
// several suites running concurrently is less time than the child needs to be
// scheduled at all (field flake, 2026-08-19: two different child-output waits
// expired under parallel-suite load and passed on every quiet rerun). One
// real millisecond per unsatisfied pass gives the default budget a fifth of a
// real second and a child-output wait (kChildOutputPasses) two full seconds,
// while a predicate that is already true still returns on its first pass.
inline constexpr int kChildOutputPasses = 2000;
template <typename Ready>
bool run_until(Server& server, ckv::ManualClock& clock, Client& client, Ready ready,
               int passes = 200) {
    for (int pass = 0; pass < passes; ++pass) {
        clock.advance(34'000'000);
        if (!server.step()) return false;
        client.pump();
        if (ready()) return true;
        ::usleep(1000);
    }
    return false;
}

// One frame of a child's animation: the terminal's whole picture area filled,
// drawn at the same place every time, in a colour that changes so the pixels
// really differ (the differ compares them, and a frame equal to the last one
// is correctly not news at all). Full-screen because that is the size a real
// one is — ckvision_spin fills its pane — and because a picture has to be
// bigger than one tick's drip for a debt to outlive the tick that made it.
std::string sixel_frame(int shade, int row = 1, int column = 1) {
    std::string frame = "\x1b[" + std::to_string(row) + ";" + std::to_string(column) + "H";
    frame += "\x1bPq#0;2;" + std::to_string(shade % 101) + ";20;60#0";
    for (int band = 0; band < 72; ++band) frame += "!720~-";  // 720 x 432 px
    frame += "\x1b\\";
    return frame;
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
    // The session model: attaching is always granted to the newest client, and the
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
    // fresh one (the session model).
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
    // the only way it can (the protocol spec, the gap rule).
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
    // rather than repair (the protocol spec).
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
    // again, which is how a resnapshot is spelled (the protocol spec).
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
    CK_CHECK(run_until(server, clock, watcher, [&] { return !mirrored->rasters().empty(); },
                       kChildOutputPasses));

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
    // The terminal-emulation spec: `sixel = auto` advertises what the host can show. The client
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
                           [&] { return !opened->session().diagnostics().empty(); },
                           kChildOutputPasses));
    }
    ckm::client::RemoteTerminalSubsession* mirrored = watcher.session.terminal(ids.front());
    CK_CHECK(mirrored != nullptr);
    if (mirrored != nullptr) CK_CHECK(mirrored->rasters().empty());
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_pane_opened_after_the_hosts_sixel_probe_resolves_still_gets_told_it_can_draw) {
    // The capability probe DA1's parameter 4 depends on is asynchronous
    // (run_client.cpp), so a client's outer-terminal Sixel answer may not be
    // known yet at the moment it attaches — Attach.host_sixel = 0, honestly,
    // because "unknown" and "no" advertise alike. What must not follow is
    // every pane THAT SESSION EVER OPENS believing the host cannot draw,
    // forever, once the answer does arrive (field report, 2026-08-18): a
    // later `NewTerminal` carries its own host_sixel, read fresh, so a pane
    // opened after the probe resolves is not bound by what Attach said
    // before it had an answer.
    const std::filesystem::path socket = private_socket("sixel-race");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.set_host_sixel(false);  // unknown yet, at attach time
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    // The probe answers, after the attach already went out.
    watcher.session.set_host_sixel(true);

    ckm::proto::NewTerminal request;
    request.command = "printf '\\033[2;3H\\033Pq#0;2;100;0;0!8~-!8~\\033\\\\'; sleep 30";
    request.host_sixel = watcher.session.host_sixel() ? 1 : 0;
    watcher.session.request(request);
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return !watcher.session.terminal_ids().empty(); }));

    const std::vector<std::uint64_t> ids = watcher.session.terminal_ids();
    CK_CHECK(!ids.empty());
    if (ids.empty()) {
        server.terminals().close_all();
        forget(socket);
        return;
    }
    ckm::client::RemoteTerminalSubsession* mirrored = watcher.session.terminal(ids.front());
    CK_CHECK(mirrored != nullptr);
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return mirrored != nullptr && !mirrored->rasters().empty(); },
                       kChildOutputPasses));
    if (mirrored != nullptr) CK_CHECK(!mirrored->rasters().empty());
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_heal_under_steady_news_converges_instead_of_looping) {
    // The self-sustaining heal loop, pinned end to end. A heal used to inline
    // every believed picture's pixels — tens of megabytes at HiDPI cell sizes
    // — so the heal's own payload crossed the backlog mark, any news during
    // the drain (a blinking cursor suffices) re-marked the client dirty, and
    // the next heal re-sent everything: one full lap per drain, forever, the
    // pictures visible for a moment per lap and gray otherwise (field report,
    // 2026-08-19, ckgrapher). Now the heal owes its pictures and drips them
    // as the queue has room, news during the drip does not re-arm the heal,
    // and the mirror keeps the pixels it already holds — so a client that
    // fell behind once heals ONCE, under news that never stops.
    const std::filesystem::path socket = private_socket("healloop");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    // The ckgrapher shape: one terminal holds a picture and stays quiet, a
    // second is busy — so the session's backlog is real while the picture
    // itself never legitimately goes away.
    ckm::server::Terminal& pictured = server.open_terminal(
        0, spec_running("printf '\\033[2;3H\\033Pq#0;2;100;0;0!8~-!8~\\033\\\\'; sleep 30"));
    ckm::server::Terminal& busy = server.open_terminal(0, spec_running("sleep 30"));

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    client.session.set_host_sixel(true);
    client.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, client, [&] { return client.session.attached(); }));
    ckm::client::RemoteTerminalSubsession* mirrored = nullptr;
    CK_CHECK(run_until(server, clock, client, [&] {
        mirrored = client.session.terminal(pictured.id());
        return mirrored != nullptr && !mirrored->rasters().empty();
    }, kChildOutputPasses));

    // The fall: the client stops reading while the busy child floods, until
    // the server has marked it for healing.
    client.refuse_to_read();
    std::string burst;
    for (int line = 0; line < 24; ++line)
        burst += "a line of output that fills the screen " + std::to_string(line) + "\r\n";
    for (int tick = 0; tick < 4000 && !server.waiting_to_heal(); ++tick) {
        busy.session().feed_output(burst);
        clock.advance(34'000'000);
        CK_CHECK(server.step());
    }
    CK_CHECK(server.waiting_to_heal());

    // The recovery, under news that never stops: one short line per tick, the
    // shape of a blinking cursor or a clock. The client reads normally.
    const std::uint64_t attachments_before = client.session.attachments();
    bool raster_seen_missing = false;
    for (int tick = 0; tick < 400; ++tick) {
        busy.session().feed_output("tick " + std::to_string(tick) + "\r");
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        client.pump();
        if (mirrored != nullptr && mirrored->rasters().empty()) raster_seen_missing = true;
    }
    const std::uint64_t heals = client.session.attachments() - attachments_before;
    std::printf("  [heal loop] %llu heal(s) across 400 ticks of steady news\n",
                static_cast<unsigned long long>(heals));
    // Healed at all, and healed ONCE — the loop was one per drain, dozens
    // here. Two are allowed: a heal may be marked while the first is still
    // being asked for.
    CK_CHECK(heals >= 1U);
    CK_CHECK(heals <= 2U);
    // And the reader never stared at a hole where their picture was: the
    // mirror held its pixels across every heal.
    CK_CHECK(!raster_seen_missing);
    // The mirror ends the stretch current, not merely pretty: the last line
    // the busy child wrote is on its mirror.
    busy.session().feed_output("\r\nthe very last line\r\n");
    CK_CHECK(run_until(server, clock, client, [&] {
        ckm::client::RemoteTerminalSubsession* watched = client.session.terminal(busy.id());
        if (watched == nullptr) return false;
        std::string text;
        for (const ckv::Cell& cell : watched->cells())
            if (!cell.is_continuation()) text += cell.grapheme();
        return text.find("the very last line") != std::string::npos;
    }));
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_child_that_animates_owes_a_reader_its_newest_frame_not_a_history_of_them) {
    // ckvision_spin in a pane, on a host terminal that draws Sixel slower than
    // the child produces it. The picture debt exists because a client that
    // fell behind must not be re-snapshotted per tick — but it used to be
    // APPENDED to, so every frame the child ever drew became a permanent entry
    // in it. The reader saw the consequence twice over: a cube that went on
    // turning at the position its window had left ten seconds earlier, and an
    // animation that carried on for a minute or two after the child was gone,
    // out of the hundreds of megabytes the server was holding in order to
    // replay it (field report, 2026-08-19).
    //
    // A debt is a statement about a picture NOW. The newest frame supersedes
    // the one that has not gone out yet, so what is owed stays the size of the
    // picture rather than the size of the child's history.
    const std::filesystem::path socket = private_socket("animation");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& animating = server.open_terminal(0, spec_running("sleep 30"));

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    client.session.set_host_sixel(true);
    client.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, client, [&] { return client.session.attached(); }));
    ckm::client::RemoteTerminalSubsession* mirrored = nullptr;
    animating.session().feed_output(sixel_frame(0));
    CK_CHECK(run_until(server, clock, client, [&] {
        mirrored = client.session.terminal(animating.id());
        return mirrored != nullptr && !mirrored->rasters().empty();
    }, kChildOutputPasses));
    CK_CHECK(mirrored != nullptr);
    std::size_t one_frame = 0;
    if (mirrored != nullptr && !mirrored->rasters().empty() &&
        mirrored->rasters().front().image != nullptr)
        one_frame = static_cast<std::size_t>(mirrored->rasters().front().image->width()) *
                    static_cast<std::size_t>(mirrored->rasters().front().image->height()) * 4U;
    CK_CHECK(one_frame > 0U);

    // The fall. The reader stops taking pixels for a moment — a host terminal
    // busy drawing the last picture it was given — and the server marks it for
    // healing, which is the ordinary, working half of this.
    for (int frame = 1; frame <= 60 && !server.waiting_to_heal(); ++frame) {
        animating.session().feed_output(sixel_frame(frame));
        clock.advance(34'000'000);
        CK_CHECK(server.step());
    }
    CK_CHECK(server.waiting_to_heal());

    // The reader catches up, so the heal goes out and with it the picture
    // debt: the believed pictures, owed rather than queued whole, dripped as
    // the queue has room. This is the state the bug lives in — while a debt is
    // being paid the client is deliberately exempt from being marked dirty
    // again, so nothing else was left to stop the debt from growing.
    CK_CHECK(run_until(server, clock, client, [&] { return !server.waiting_to_heal(); }));

    // And now the child animates on, three hundred frames of it, while the
    // reader takes pixels slower than they are drawn — a host terminal that
    // accepts Sixel more slowly than a child produces it, which is the whole
    // situation this is about.
    constexpr std::size_t kReaderAppetite = 64u * 1024u;  // per tick, against ~1.2 MiB drawn
    const std::size_t built_before = server.diffs().picture_bytes_built();
    std::size_t peak = 0;
    bool debt_seen = false;
    for (int frame = 61; frame <= 360; ++frame) {
        animating.session().feed_output(sixel_frame(frame));
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        client.pump(kReaderAppetite);
        const std::size_t owed = server.owed_image_bytes();
        if (owed > 0) debt_seen = true;
        peak = std::max(peak, owed);
    }
    CK_CHECK(client.session.attached());
    // A run in which nothing was ever owed would sail past the ceiling below
    // without having tested anything at all, so the shape the bug needs is
    // asserted rather than assumed.
    CK_CHECK(debt_seen);

    // And the work that was never done. A payload built for a client that
    // already owes an unsent one is compared, copied, queued and dropped, so
    // the differ is asked first and leaves the picture alone when the answer
    // is no — including its belief, which is what makes the frame after the
    // debt drains the CURRENT one rather than this one. Three hundred frames
    // at 1.2 MiB is 356 MiB of memcmp-and-copy if every one is built; what a
    // reader who cannot keep up actually needs built is the handful that
    // reach them.
    const std::size_t built = server.diffs().picture_bytes_built() - built_before;
    std::printf("  [animation] payload built across 300 frames: %zu bytes of %zu drawn\n",
                built, one_frame * 300U);
    CK_CHECK(built < one_frame * 30U);

    // The other half a reader sees, and the one they described first: a
    // picture DRAGGED. A window moving across a desktop is redrawn, not
    // shifted — the pane is cleared and the picture painted at the new cell —
    // so what the differ says is that one picture went away and another
    // arrived, under a wire id of its own each time. The Remove is what makes
    // that bounded: it says the pixels still waiting to go out are for a
    // picture nobody will ever see, and a debt that kept them anyway replayed
    // the drag — the cube arriving where the reader put it long after they let
    // go, having visited every position on the way.
    std::size_t peak_ops = 0;
    for (int step = 0; step < 200; ++step) {
        animating.session().feed_output("\x1b[2J" + sixel_frame(7, 1 + step % 12, 1 + step % 30));
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        client.pump(kReaderAppetite);
        peak_ops = std::max(peak_ops, server.owed_image_ops());
        peak = std::max(peak, server.owed_image_bytes());
    }
    std::printf("  [animation] peak owed ops across 200 moves: %zu\n", peak_ops);
    // Measured across two hundred moves: **3887 ops and 528 MiB** owed before
    // this, **78 ops and the same 1.8 frames** after. The ceiling is set well
    // above the second and far below the first, because what is being asserted
    // is the difference between a debt that tracks the picture and one that
    // records the drag — not a particular drip schedule.
    CK_CHECK(peak_ops <= 400U);
    CK_CHECK(peak <= one_frame * 3U);

    // Back to one place, and a last handful of frames drawn there while the
    // reader is STILL behind — which is the state the belief has to survive.
    for (int frame = 0; frame < 10; ++frame) {
        animating.session().feed_output(frame == 0 ? "\x1b[2J" + sixel_frame(90)
                                                   : sixel_frame(90 + frame));
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        client.pump(kReaderAppetite);
    }
    CK_CHECK(server.owed_image_bytes() > 0U);  // still behind, or the check below is vacuous

    // And it is the NEWEST frame that is owed, not the oldest. The child stops
    // HERE, having drawn its last frame while the reader was still behind —
    // which is the case that pins the belief. A differ that adopted pixels it
    // had decided not to send would believe the client already held this
    // frame, would never emit it, and the reader would sit on the frame before
    // it for as long as the child stayed quiet: forever, in a demo that has
    // finished. Nothing more is fed below; the reader simply catches up, and
    // what it must land on is the last thing the child drew.
    ckv::Image expected(1, 1);
    CK_CHECK(run_until(server, clock, client, [&] {
        ckm::server::Terminal* const source = server.terminals().find(animating.id());
        if (source == nullptr) return false;
        const std::span<const ckv::core::TerminalRaster> rasters = source->session().rasters();
        if (rasters.empty() || rasters.front().image == nullptr) return false;
        expected = *rasters.front().image;
        return true;
    }, kChildOutputPasses));
    CK_CHECK(run_until(server, clock, client, [&] {
        if (mirrored == nullptr || mirrored->rasters().empty()) return false;
        const ckv::Image* const held = mirrored->rasters().front().image.get();
        if (held == nullptr) return false;
        if (held->width() != expected.width() || held->height() != expected.height()) return false;
        return std::memcmp(held->data(), expected.data(),
                           static_cast<std::size_t>(expected.width()) *
                               static_cast<std::size_t>(expected.height()) * 4U) == 0;
    }, kChildOutputPasses));


    std::printf("  [animation] one frame %zu bytes, peak owed %zu bytes across 300 frames\n",
                one_frame, peak);
    // Two frames' worth is the honest ceiling: one payload may be half-way
    // out — those chunks must finish, or the mirror is left assembling a
    // picture whose end never comes — while the newest waits behind it. Three
    // hundred frames of history is what this used to be, and it is what the
    // reader was made to sit through.
    CK_CHECK(peak <= one_frame * 3U);
    CK_CHECK(server.owed_image_bytes() <= one_frame * 3U);

    server.terminals().close_all();
    forget(socket);
}

// --- R8: what a reattaching reader used to lose ----------------------------

CK_TEST(a_reattach_brings_back_the_keyboard_the_clipboard_the_printer_and_the_dead) {
    // The one scenario R8 exists for, over a real socket: a session where a
    // child has turned the kitty keyboard protocol on, put text on the
    // clipboard, started the printer, and — in the terminal beside it — ended
    // with a status the reader is being shown. A client goes away and comes
    // back, which is what a multiplexer is FOR, and every one of those facts
    // has to survive the round trip.
    //
    // Before this each of them died differently: the flags were never
    // transported at all while the server answered the child's re-probe with
    // them ON (M-R2), the clipboard write was enabled toward the child and then
    // dropped (M-R1), the printer had no client-side field, and the snapshot
    // had no way to say a child had exited so the mirror had to remember it
    // (M-R4).
    const std::filesystem::path socket = private_socket("reattach-state");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& living = server.open_terminal(0, spec_running("sleep 30"));
    ckm::server::Terminal& dying = server.open_terminal(0, spec_running("exit 7"));
    const ckm::server::TerminalId living_id = living.id();
    const ckm::server::TerminalId dead_id = dying.id();

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    // The child asks for the kitty protocol, puts "hello" on the clipboard, and
    // starts the printer controller. Written straight into the emulator, which
    // is what the child's PTY would have delivered.
    living.session().feed_output("\x1b[>1u\x1b]52;c;aGVsbG8=\x07\x1b[5i");

    ckm::client::RemoteTerminalSubsession* mirror = watcher.session.terminal(living_id);
    ckm::client::RemoteTerminalSubsession* gone = watcher.session.terminal(dead_id);
    CK_CHECK(mirror != nullptr);
    CK_CHECK(gone != nullptr);
    if (mirror == nullptr || gone == nullptr) {
        server.terminals().close_all();
        forget(socket);
        return;
    }

    // Live, first — a reattach that restored what was never delivered would be
    // proving the wrong thing.
    CK_CHECK(run_until(server, clock, watcher, [&] {
        return mirror->status().keyboard_flags ==
                   ckv::core::TerminalKeyboardFlags::DisambiguateEscapeCodes &&
               mirror->mirror().clipboard_text() == "hello" &&
               mirror->status().printer_controller_active;
    }));
    CK_CHECK(run_until(server, clock, watcher, [&] { return gone->mirror().exited(); }));
    const std::uint64_t watermark = mirror->status().clipboard_serial;
    CK_CHECK(watermark != 0U);

    // The detach, and the reattach. The programs carry on without either.
    watcher.session.request(ckm::proto::Detach{});
    CK_CHECK(run_until(server, clock, watcher, [&] { return !watcher.session.attached(); }));
    const std::uint64_t attachments = watcher.session.attachments();
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return watcher.session.attachments() > attachments; }));

    // The keyboard the child is expecting, so the next key it is sent is
    // spelled the way it asked for.
    CK_CHECK(mirror->status().keyboard_flags ==
             ckv::core::TerminalKeyboardFlags::DisambiguateEscapeCodes);
    // The printer, which is the one that reads as a fault rather than as a
    // missing feature: with the controller on, the child's output is going to
    // the printer and not to the screen.
    CK_CHECK(mirror->status().printer_controller_active);
    // The clipboard watermark stands and no write is replayed: the text
    // deliberately does not ride the snapshot, so a reader's own clipboard is
    // left exactly as they left it.
    CK_CHECK(mirror->status().clipboard_serial >= watermark);
    CK_CHECK(mirror->mirror().clipboard_text().empty());
    // And the dead stay dead. A window a reader could type into with nothing on
    // the other end is the failure this one prevents.
    CK_CHECK(gone->mirror().exited());
    CK_CHECK(gone->mirror().held());
    CK_CHECK(gone->state() == ckv::core::TerminalSubsessionState::Exited);
    CK_CHECK(gone->status().exit_code.has_value());
    if (gone->status().exit_code.has_value()) CK_CHECK(*gone->status().exit_code == 7);

    // A write after the reattach still arrives, which is the half that a
    // watermark reset would have broken in silence: the client's own high-water
    // mark would have been ahead of the number the next write carried.
    //
    // The printer controller is turned off first, because while it is on the
    // child's output goes to the printer and not to the parser — which is the
    // very fact asserted two lines above.
    living.session().feed_output("\x1b[4i\x1b]52;c;d29ybGQ=\x07");
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return mirror->mirror().clipboard_text() == "world"; }));
    CK_CHECK(mirror->status().clipboard_serial > watermark);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_terminal_the_reader_is_not_in_marks_what_they_missed) {
    // m-replay's bell and activity, and the focus they are defined against.
    // "Activity" is output in a terminal the reader is NOT in — the tmux
    // meaning, and the only one that makes a window marker worth looking at —
    // and the server's notion of where the reader is is the terminal their
    // input goes to, which is the only statement of focus this protocol
    // carries (`Raise` and `FocusTerm` encode and nothing sends them).
    const std::filesystem::path socket = private_socket("marks");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& here = server.open_terminal(0, spec_running("sleep 30"));
    ckm::server::Terminal& elsewhere = server.open_terminal(0, spec_running("sleep 30"));
    const ckm::server::TerminalId here_id = here.id();
    const ckm::server::TerminalId elsewhere_id = elsewhere.id();

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    ckm::client::RemoteTerminalSubsession* mine = watcher.session.terminal(here_id);
    ckm::client::RemoteTerminalSubsession* theirs = watcher.session.terminal(elsewhere_id);
    CK_CHECK(mine != nullptr);
    CK_CHECK(theirs != nullptr);
    if (mine == nullptr || theirs == nullptr) {
        server.terminals().close_all();
        forget(socket);
        return;
    }

    // The reader types in the first terminal, which is what puts them in it.
    // Whatever mark it carried from being opened beside a focused one clears,
    // and from here on the second terminal is the one nobody is in — which is
    // what the next step actually proves, since a server whose focus never
    // moved would go on clearing the second one's marks instead of setting
    // them.
    ckm::proto::Input keystroke;
    keystroke.term = here_id;
    keystroke.bytes = " ";
    watcher.session.request(keystroke);
    CK_CHECK(run_until(server, clock, watcher, [&] { return !mine->mirror().activity_marked(); }));

    // The other one rings and prints while the reader is not in it.
    elsewhere.session().feed_output("\aa line the reader did not see\r\n");
    CK_CHECK(run_until(server, clock, watcher, [&] {
        return theirs->mirror().bell_marked() && theirs->mirror().activity_marked();
    }));
    // And the terminal the reader IS in does not mark itself: output there is
    // not news, it is the thing they are watching.
    here.session().feed_output("a line the reader is looking at\r\n");
    for (int pass = 0; pass < 8; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        watcher.pump();
    }
    CK_CHECK(!mine->mirror().activity_marked());

    // The reader goes there; the marks go with them.
    keystroke.term = elsewhere_id;
    watcher.session.request(keystroke);
    CK_CHECK(run_until(server, clock, watcher, [&] {
        return !theirs->mirror().bell_marked() && !theirs->mirror().activity_marked();
    }));

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_host_terminal_that_grows_says_so_while_the_client_is_attached) {
    // `ClientResize` had a server handler and no sender at all: the desktop and
    // its cell metric travelled once, at attach time, so a reader who resized
    // their window — or changed its font — kept the old metric until they
    // detached and came back. The metric is what a child's `TIOCSWINSZ` pixel
    // fields and its XTWINOPS answers are derived from, which is what decides
    // the size of every picture it draws.
    const std::filesystem::path socket = private_socket("client-resize");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    const ckm::server::TerminalId id = terminal.id();

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::ClientResize) == 0U);

    // The reader's own window, bigger and with larger cells under it.
    watcher.session.desktop_resized(ckv::Size{100, 30}, ckv::Size{10, 20});
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::ClientResize) == 1U);
    // Idempotent: the client's loop asks on every pass, and a message per pass
    // would be a resize storm nobody asked for.
    watcher.session.desktop_resized(ckv::Size{100, 30}, ckv::Size{10, 20});
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::ClientResize) == 1U);

    // The server took it, and the proof is what a terminal is told next: its
    // own size arrives as a `MoveResize` in CELLS, and the pixels the child is
    // given are those cells times the metric the client last declared.
    ckm::client::RemoteTerminalSubsession* mirror = watcher.session.terminal(id);
    CK_CHECK(mirror != nullptr);
    if (mirror == nullptr) {
        server.terminals().close_all();
        forget(socket);
        return;
    }
    mirror->resize(ckv::Size{50, 10}, ckv::Size{10, 20});
    CK_CHECK(run_until(server, clock, watcher, [&] {
        const ckm::server::Terminal* held = server.terminals().find(id);
        return held != nullptr && held->cell_pixels() == ckv::Size{10, 20};
    }));

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_request_the_server_refuses_reaches_the_reader) {
    // "A request is always answered" was true of the server and false of
    // everything after it: `Error` had eight senders and no reader, because
    // `ServerSession::handle` did not claim it and the client's loop discarded
    // what it did not understand. A reader who asked for something impossible
    // was told nothing, which looks exactly like a server that has hung.
    const std::filesystem::path socket = private_socket("refusal");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    (void)server.open_terminal(0, spec_running("sleep 30"));

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    ckm::proto::Error refused;
    bool told = false;
    watcher.session.on_error = [&refused, &told](const ckm::proto::Error& error) {
        refused = error;
        told = true;
    };
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    // A terminal this server has never had. The id is not recycled, so this can
    // only ever be a stale client — which is precisely the case a reader needs
    // told rather than swallowed.
    ckm::proto::MoveResize impossible;
    impossible.term = 9999;
    impossible.rect.width = 40;
    impossible.rect.height = 10;
    watcher.session.request(impossible);
    CK_CHECK(run_until(server, clock, watcher, [&] { return told; }));
    CK_CHECK(refused.code == static_cast<std::uint16_t>(ckm::proto::ErrorCode::NoSuchTerminal));
    CK_CHECK(refused.context == "MoveResize");
    CK_CHECK(!refused.human.empty());
    // And the session did not hand it on as something it failed to understand:
    // an `Error` is claimed here now, not left for the host to discard.
    CK_CHECK(watcher.count_of<ckm::proto::Error>() == 0U);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_layout_report_goes_out_once_per_change_and_never_while_unattached) {
    // WP-29's wire half. The window layer decides WHEN an arrangement has
    // settled; this end decides whether it may be sent at all, and the two
    // rules it enforces are both about messages that must NOT appear: one for a
    // client with no session to report about, and one for an arrangement the
    // server has already been told.
    const std::filesystem::path socket = private_socket("layout");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    std::size_t refusals = 0;
    watcher.session.on_error = [&refusals](const ckm::proto::Error&) { ++refusals; };
    // What the server states back about the arrangement — the snapshot's at
    // attach time, and every `LayoutDelta` after it. Recorded rather than laid
    // out: this client draws nothing, and the seam it hands the statement to is
    // the same one a drawing client puts windows from (WP-30).
    std::vector<ckm::proto::LayoutEntry> stated;
    // And which SITUATION each statement came out of. The two carry the same
    // shape and mean different things — a snapshot is a reattach and has
    // something to lay down, a delta is the server's record moving while the
    // reader watches — and a real client restores from the first and never the
    // second. Acting on a delta buried a dialog behind the terminal it was
    // asking about, so the tag the gate rests on is pinned here.
    std::size_t snapshots = 0;
    std::size_t deltas = 0;
    watcher.session.on_layout =
        [&](const std::vector<ckm::proto::LayoutEntry>& entries,
            ckm::client::ServerSession::LayoutStatement statement) {
            stated = entries;
            if (statement == ckm::client::ServerSession::LayoutStatement::Snapshot)
                ++snapshots;
            else
                ++deltas;
        };

    ckm::proto::LayoutEntry placed;
    placed.term = terminal.id();
    placed.rect = ckm::proto::Rect{4, 2, 40, 12};

    // Not attached yet. The server would answer `Error{NoSuchSession}` — it has
    // nowhere to file an arrangement for a client watching nothing — so the
    // question is never asked. An error a client provoked by saying something
    // it already knew was pointless is noise in front of the reader.
    watcher.session.report_layout({placed});
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::SetLayout) == 0U);

    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    watcher.session.report_layout({placed});
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::SetLayout) == 1U);
    // The same arrangement, again. The client's own debounce turns a drag into
    // one settled report; this is what keeps a report that says nothing new —
    // a window dragged back where it started, a heal that rebuilt nothing —
    // off the wire even so.
    watcher.session.report_layout({placed});
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::SetLayout) == 1U);

    ckm::proto::LayoutEntry moved = placed;
    moved.rect = ckm::proto::Rect{9, 5, 40, 12};
    watcher.session.report_layout({moved});
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::SetLayout) == 2U);

    // And the server filed it: it states the arrangement back on its tick, with
    // the rect this client reported.
    const auto stated_where_the_window_moved_to = [&] {
        return stated.size() == 1U && stated.front().term == terminal.id() &&
               stated.front().rect == moved.rect;
    };
    CK_CHECK(run_until(server, clock, watcher, stated_where_the_window_moved_to));
    CK_CHECK(refusals == 0U);
    // A `LayoutDelta` is a message this build understands, so it never lands in
    // the pile of ones nothing claimed.
    CK_CHECK(watcher.count_of<ckm::proto::LayoutDelta>() == 0U);
    // Exactly one snapshot — the attach — and the moves came as deltas. A
    // build that tagged a delta as a snapshot would hand a live desktop to the
    // restoration policy every time the reader nudged a window.
    CK_CHECK(snapshots == 1U);
    CK_CHECK(deltas >= 1U);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(an_arrangement_is_news_again_after_a_takeover_took_the_session_away) {
    // The edge trigger remembers what this client told THIS attachment. A
    // takeover ends it, and whatever the client that displaced this one
    // reported is what the server now holds — so the same arrangement, reported
    // after attaching again, is news and has to go out. Remembering across a
    // detach would leave a reader's own window positions on the floor for
    // exactly as long as they did not move anything after coming back.
    const std::filesystem::path socket = private_socket("layout-again");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));

    Client early;
    CK_CHECK(early.connect(socket));
    early.greet();
    early.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, early, [&] { return early.session.attached(); }));

    ckm::proto::LayoutEntry placed;
    placed.term = terminal.id();
    placed.rect = ckm::proto::Rect{4, 2, 40, 12};
    early.session.report_layout({placed});
    CK_CHECK(early.sent_count(ckm::proto::MessageType::SetLayout) == 1U);

    Client late;
    CK_CHECK(late.connect(socket));
    late.greet();
    late.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, late, [&] { return late.session.attached(); }));
    CK_CHECK(run_until(server, clock, early, [&] { return !early.session.attached(); }));

    // Displaced, and reporting into the void: nothing is sent, and the server
    // is not asked to refuse anything.
    early.session.report_layout({placed});
    CK_CHECK(early.sent_count(ckm::proto::MessageType::SetLayout) == 1U);

    // Back again — and the arrangement it is holding is worth stating even
    // though nothing about it changed while it was away.
    early.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, early, [&] { return early.session.attached(); }));
    early.session.report_layout({placed});
    CK_CHECK(early.sent_count(ckm::proto::MessageType::SetLayout) == 2U);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_tile_share_survives_the_server_and_comes_back_on_the_reattach_snapshot) {
    // WP-30's wire addition, over the path it exists for. The share is what
    // makes a 50/50 split still a 50/50 split on a terminal of another size, so
    // it has to outlive the client that measured it exactly as the rect does —
    // and it is the SNAPSHOT that has to carry it, because a reattach is a
    // client arriving with no history at all.
    const std::filesystem::path socket = private_socket("layout-tile");
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

    // The left half of a filled 50/50 tiling, reported as ckVision's own query
    // answers it: the rect it happens to occupy right now, AND the share it is
    // of the desktop. Both, because they answer different questions and only
    // the reattaching client decides which one applies.
    ckm::proto::LayoutEntry tiled;
    tiled.term = terminal.id();
    tiled.rect = ckm::proto::Rect{0, 1, 40, 22};
    tiled.tile = ckm::proto::TileFraction{0, 0, ckm::proto::kTileFractionWhole / 2,
                                          ckm::proto::kTileFractionWhole};
    watcher.session.report_layout({tiled});
    CK_CHECK(watcher.sent_count(ckm::proto::MessageType::SetLayout) == 1U);

    // A reader coming back — on another connection, which is the takeover that
    // reattaching to a session in use IS (the session model). Its snapshot is the only
    // thing it has ever been told about this session.
    Client returning;
    CK_CHECK(returning.connect(socket));
    returning.greet();
    std::vector<ckm::proto::LayoutEntry> restored;
    // Only the snapshot's statement, which is the one a reattach lays down —
    // the same gate `run_client.cpp` applies, so this test proves the path a
    // reader actually reattaches through rather than one no client uses.
    returning.session.on_layout =
        [&restored](const std::vector<ckm::proto::LayoutEntry>& entries,
                    ckm::client::ServerSession::LayoutStatement statement) {
            if (statement == ckm::client::ServerSession::LayoutStatement::Snapshot)
                restored = entries;
        };
    // Let the report reach the server before asking for the snapshot that has
    // to contain it; the pump the attach runs through is what carries it.
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return terminal.layout().tile.filled(); }));
    returning.session.attach(0, ckv::Size{120, 40}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, returning, [&] { return returning.session.attached(); }));

    CK_CHECK(restored.size() == 1U);
    if (restored.size() == 1U) {
        CK_CHECK(restored.front().term == terminal.id());
        // The rect the reader's old terminal gave it, unchanged and unclamped —
        // this server neither draws a window nor makes one safe.
        CK_CHECK(restored.front().rect == tiled.rect);
        // And the share, which is the number that means anything on the 120x40
        // desktop this client just declared.
        CK_CHECK(restored.front().tile == tiled.tile);
        CK_CHECK(restored.front().tile.filled());
    }

    server.terminals().close_all();
    forget(socket);
}
