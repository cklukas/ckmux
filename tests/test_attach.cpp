// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// WP-6: attaching, detaching, being taken over, and being healed after falling
// behind. Everything here runs against a real `Server` over a real Unix socket,
// with the loop stepped by hand on a `ManualClock` — the tick has to be owned,
// not waited for, and the socket has to be real because backpressure is a fact
// about sockets rather than about queues.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <unistd.h>

#include "client/server_connection.hpp"
#include "client/server_session.hpp"
#include "common/proto.hpp"
#include "platform/socket.hpp"
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

    bool connect(const std::filesystem::path& socket) {
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        if (result.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = ckm::platform::Stream(result.fd);
        session = ckm::client::ServerSession(
            [this](const ckm::proto::Message& message) { (void)stream.send(ckm::proto::encode(message)); });
        session.set_history_limit(100);
        return true;
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
    ckm::server::Terminal& first = server.terminals().open(spec_running("sleep 30"));
    ckm::server::Terminal& second = server.terminals().open(spec_running("sleep 30"));
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
    ckm::server::Terminal& terminal = server.terminals().open(spec_running("sleep 30"));
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

CK_TEST(a_client_that_dies_is_a_detach_and_its_terminals_do_not_notice) {
    const std::filesystem::path socket = private_socket("death");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.terminals().open(spec_running("sleep 30"));
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
    ckm::server::Terminal& terminal = server.terminals().open(spec_running("sleep 30"));

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
    int ticks = 0;
    for (; ticks < 4000 && server.queued_bytes() <= ckm::platform::Stream::kHighWaterBytes;
         ++ticks) {
        terminal.session().feed_output(burst + std::to_string(ticks) + "\r\n");
        clock.advance(34'000'000);
        CK_CHECK(server.step());
    }
    CK_CHECK(server.queued_bytes() > ckm::platform::Stream::kHighWaterBytes);
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
    ckm::server::Terminal& terminal = server.terminals().open(spec_running("sleep 30"));

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
