// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-2's loop, in-process and on a `ManualClock`. The lifecycle cases run a real
// process (test_server_lifecycle.cpp); these are the ones about WHEN the loop
// does things, and a tick that only happens when real time passes cannot be
// tested by waiting for real time — it can only be tested by owning the clock.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include "client/server_connection.hpp"
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
    directory /= "ckmux-l" + std::to_string(static_cast<unsigned long>(::getpid()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (std::string(name) + ".sock");
}

void forget(const std::filesystem::path& socket) {
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    std::filesystem::remove(socket.parent_path(), ignored);
    // And the directory, which only succeeds once it is empty — so the last
    // case to finish tidies up and the others leave it alone. A test suite that
    // litters a machine's temporary directory with one entry per run is a test
    // suite somebody eventually has to clean up by hand.
}

ckm::Settings test_settings() {
    ckm::Settings settings;
    settings.shell = "/bin/sh";
    settings.login_shell = false;
    settings.scrollback = 100;
    settings.max_fps = 30;
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
    void say(const ckm::proto::Message& message) { (void)stream.send(ckm::proto::encode(message)); }
    // Reads without waiting on real time: the caller steps the server between
    // calls, so anything that was going to arrive already has.
    bool take(ckm::proto::Message& message) {
        std::string arrived;
        (void)stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) return false;
        return reader.next(message) == ckm::proto::DecodeError::None;
    }
};

// Steps the server until the client has something, or until it is clear it
// never will. Two passes are the ordinary case, not one: a connection accepted
// mid-pass was not in the poll set when that pass began, so its first message is
// read on the next one. That costs microseconds, not a tick — which is the
// property the first test below is actually about.
bool pump(Server& server, WireClient& client, ckm::proto::Message& message, int passes = 8) {
    for (int pass = 0; pass < passes; ++pass) {
        if (!server.step()) return false;
        if (client.take(message)) return true;
    }
    return false;
}

// Steps the server a few times and reports whether anything arrived. For the
// cases whose claim is that nothing should.
// The next message of a given kind, skipping `SessionList`. An attach changes
// a session's reader count and the tick states the new one (WP-48), so a case
// about the DELTA path now has one unrelated frame to step over. Skipping it by
// name rather than draining everything keeps the assertion sharp: anything else
// unexpected still lands in `out` and fails the check that follows.
template <class Wanted>
bool take_past_session_lists(WireClient& client, Wanted& out) {
    ckm::proto::Message message;
    while (client.take(message)) {
        if (std::holds_alternative<ckm::proto::SessionList>(message)) continue;
        const Wanted* const wanted = std::get_if<Wanted>(&message);
        if (wanted == nullptr) return false;
        out = *wanted;
        return true;
    }
    return false;
}

bool pump_expecting_silence(Server& server, WireClient& client, int passes = 6) {
    ckm::proto::Message unwanted;
    for (int pass = 0; pass < passes; ++pass) {
        if (!server.step()) return false;
        if (client.take(unwanted)) return false;
    }
    return true;
}

ckm::server::TerminalSpec spec_running(std::string command) {
    ckm::server::TerminalSpec spec;
    spec.command = std::move(command);
    spec.working_directory = "/";
    spec.columns = 40;
    spec.rows = 8;
    spec.pixel_width = 40 * 9;
    spec.pixel_height = 8 * 18;
    spec.environment = {{"TERM", "xterm-256color"}, {"PATH", "/usr/bin:/bin"}, {"LC_ALL", "C"}};
    return spec;
}

}  // namespace

CK_TEST(the_loop_greets_a_client_without_waiting_for_a_tick) {
    // A handshake must not wait on the flush tick. At 30 fps that is 33 ms, and
    // a client that has to wait for one before being told it is connected is a
    // client that feels slow at startup for no reason anyone could see.
    const std::filesystem::path socket = private_socket("greet");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    client.say(hello);

    // A couple of passes, and the clock has not moved at all: nothing in the
    // handshake waits for a tick.
    ckm::proto::Message answer;
    CK_CHECK(pump(server, client, answer));
    CK_CHECK(clock.now_nanos() == 0);
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(answer));
    CK_CHECK(server.greeted_count() == 1U);
    forget(socket);
}

CK_TEST(a_terminals_output_reaches_a_client_when_the_tick_is_due_and_not_before) {
    // The flush tick, which is the whole reason the loop has a deadline: a
    // child that writes forty times between two ticks costs one delta, and a
    // client hears about it when the tick comes round.
    const std::filesystem::path socket = private_socket("tick");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    client.say(hello);
    ckm::proto::Message greeting;
    CK_CHECK(pump(server, client, greeting));
    // And attached: deltas go to the client that holds the session, not to
    // everyone who has said hello. WP-2 sent them to any greeted UI client
    // because there was nothing to attach to yet; WP-6 narrowed it, as it said
    // it would.
    ckm::proto::Attach attach;
    attach.columns = 40;
    attach.rows = 8;
    client.say(attach);
    ckm::proto::Message attached;
    CK_CHECK(pump(server, client, attached));
    CK_CHECK(std::holds_alternative<ckm::proto::Attached>(attached));

    // A terminal with something to say. Fed through the seam rather than run as
    // a child, so this test is about the loop's timing and not a shell's.
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    terminal.session().feed_output("what the child printed\r\n");

    // The tick is not due: the clock has not moved since the pass that consumed
    // this tick's turn.
    CK_CHECK(pump_expecting_silence(server, client));

    // 33 ms later at 30 fps, it is.
    clock.advance(34'000'000);
    CK_CHECK(server.step());
    ckm::proto::GridDelta delta;
    CK_CHECK(take_past_session_lists(client, delta));
    CK_CHECK(delta.term == terminal.id());
    CK_CHECK(delta.seq == 1U);
    CK_CHECK(!delta.ops.empty());

    // And nothing further while the child stays quiet, however many ticks pass:
    // a tick with no news is not a frame with no content, it is no frame.
    for (int tick = 0; tick < 5; ++tick) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        ckm::proto::Message nothing;
        while (client.take(nothing))
            CK_CHECK(std::holds_alternative<ckm::proto::SessionList>(nothing));
    }

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_cli_client_is_not_sent_a_screenful_of_deltas) {
    // `ckmux ls` and `ckmux kill-server` are clients too, and they have no
    // screen. Sending them terminal deltas would be sending megabytes to a
    // process that is about to print one line and exit.
    const std::filesystem::path socket = private_socket("cli");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient utility;
    CK_CHECK(utility.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    hello.client_kind = ckm::proto::ClientKind::Cli;
    utility.say(hello);
    ckm::proto::Message greeting;
    CK_CHECK(pump(server, utility, greeting));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(greeting));
    // Attached, so that what this pins is "a CLI client is sent no deltas" and
    // not merely "a client that never attached is sent no deltas".
    ckm::proto::Attach attach;
    attach.columns = 40;
    attach.rows = 8;
    utility.say(attach);
    ckm::proto::Message attached;
    CK_CHECK(pump(server, utility, attached));
    CK_CHECK(std::holds_alternative<ckm::proto::Attached>(attached));

    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    terminal.session().feed_output("output no CLI asked for\r\n");
    clock.advance(34'000'000);
    CK_CHECK(pump_expecting_silence(server, utility));
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_client_that_goes_away_is_a_detach_and_the_terminals_stay) {
    // The promise of the whole project, at the loop's own scale: a client dying
    // is end-of-stream on a socket, and the programs the server started are
    // still running afterwards. This is what a `kill -9` on a client looks like
    // from in here.
    const std::filesystem::path socket = private_socket("detach");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    const ckm::server::TerminalId id = terminal.id();

    {
        WireClient client;
        CK_CHECK(client.connect(socket));
        ckm::proto::Hello hello;
        hello.build = "a test";
        client.say(hello);
        ckm::proto::Message greeting;
        CK_CHECK(pump(server, client, greeting));
        CK_CHECK(server.greeted_count() == 1U);
    }  // the client's socket closes here, without saying goodbye

    CK_CHECK(server.step());
    CK_CHECK(server.client_count() == 0U);
    CK_CHECK(server.terminals().find(id) != nullptr);
    CK_CHECK(server.terminals().find(id)->live());

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(the_loop_survives_a_client_that_never_reads_what_it_asked_for) {
    // A wedged client must not be able to stall the loop that reads PTYs — if it
    // could, one client suspended at the wrong moment would freeze every
    // terminal on the machine for everybody (the architecture spec). So the server queues,
    // notices the queue is too big, and keeps going.
    const std::filesystem::path socket = private_socket("wedged");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient sulker;
    CK_CHECK(sulker.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    sulker.say(hello);
    ckm::proto::Message greeting;
    CK_CHECK(pump(server, sulker, greeting));

    // A terminal producing a great deal, and a client that never reads a byte of
    // it. Enough ticks that the socket's own buffers are long past full.
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    const auto started = std::chrono::steady_clock::now();
    for (int tick = 0; tick < 400; ++tick) {
        std::string burst;
        for (int line = 0; line < 40; ++line)
            burst += "row " + std::to_string(tick) + "-" + std::to_string(line) + "\r\n";
        terminal.session().feed_output(burst);
        clock.advance(34'000'000);
        CK_CHECK(server.step());
    }
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    // Four hundred passes over a client that reads nothing, and the loop never
    // blocked on it. The bound is generous on purpose — what is being checked is
    // that nothing waited for a socket, not how fast this machine is.
    CK_CHECK(spent < 10'000);
    CK_CHECK(server.client_count() == 1U);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_second_starter_loses_the_lock_rather_than_replacing_the_first_server) {
    // The in-process half of the race, and the two ways a starter can lose.
    //
    // While a server lives it holds the start lock, so a second starter's answer
    // is `Racing` — the lock is what settles it, and the remedy is to go back to
    // connecting. `AlreadyRunning` is the other case: a live server that does
    // NOT hold the lock, which happens when somebody deletes the lock file or a
    // server from an older build is listening. It exists so that even then
    // nothing unlinks a socket other clients are using.
    const std::filesystem::path socket = private_socket("second");
    forget(socket);
    ckv::ManualClock clock;
    Server first(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(first.start() == Server::StartStatus::Listening);

    Server racing(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(racing.start() == Server::StartStatus::Racing);

    // Now take the lock file out from under it, which is the only way a live
    // server ends up unlocked.
    std::error_code ignored;
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    Server unlocked(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(unlocked.start() == Server::StartStatus::AlreadyRunning);

    // Through both of those, the first server is still the one listening and its
    // socket was never touched.
    CK_CHECK(std::filesystem::exists(socket));
    WireClient client;
    CK_CHECK(client.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    client.say(hello);
    ckm::proto::Message answer;
    CK_CHECK(pump(first, client, answer));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(answer));
    forget(socket);
}

CK_TEST(an_exhausted_descriptor_table_does_not_spin_the_loop) {
    // What a server does when it cannot accept: nothing, for a moment, and
    // without burning a core to do it. `accept()` fails with EMFILE while the
    // connection stays pending, and a pending connection is what keeps a
    // listener readable — so a loop that reads "cannot accept" as "ask again"
    // polls a descriptor that is ready instantly, for the life of the process,
    // at 100 % of one core (M-S2).
    //
    // Checked by the clock rather than by the poll set, because the poll set is
    // not the property: a listener that is still watched but no longer asked
    // from spins exactly as badly as one that is asked. What has to be true is
    // that the passes SLEEP.
    const std::filesystem::path socket = private_socket("nofd");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    // The connection the server will fail to take. It does not survive that
    // failure on every system — a BSD `accept` takes the connection off the
    // queue before it allocates a descriptor for it, so the one that answers
    // EMFILE has already destroyed what it could not hand over — which is why
    // the resumption below is measured with a connection made afterwards
    // rather than with this one.
    WireClient rejected;
    CK_CHECK(rejected.connect(socket));

    // A descriptor table made to run out: the soft limit comes down to the next
    // free descriptor number, so everything already open stays open and the
    // next one asked for is refused. That is EMFILE as a server meets it,
    // without opening ten thousand files to get there.
    const int next_free = ::open("/dev/null", O_RDONLY);
    CK_CHECK(next_free >= 0);
    if (next_free < 0) return;
    (void)::close(next_free);
    struct ::rlimit previous{};
    CK_CHECK(::getrlimit(RLIMIT_NOFILE, &previous) == 0);
    struct ::rlimit tight = previous;
    tight.rlim_cur = static_cast<::rlim_t>(next_free);
    CK_CHECK(::setrlimit(RLIMIT_NOFILE, &tight) == 0);

    CK_CHECK(server.step());
    CK_CHECK(server.client_count() == 0U);

    // The descriptors come back, and a fresh connection arrives on the socket —
    // so the listener is readable again, which is exactly the state the spin
    // lived in. The pause still holds through it: it is a decision about time,
    // not a guess about the table.
    CK_CHECK(::setrlimit(RLIMIT_NOFILE, &previous) == 0);
    WireClient arriving;
    CK_CHECK(arriving.connect(socket));
    const auto started = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 5; ++pass) {
        CK_CHECK(server.step());
        CK_CHECK(server.client_count() == 0U);
    }
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    // Five passes that each waited for their own tick — a sixth of a second at
    // 30 fps. A spinning loop does the same five in microseconds, and the bound
    // is set well below what these actually cost so that a slow machine cannot
    // fail it and a spin cannot pass it.
    CK_CHECK(spent > 50);

    // A second later the listener is watched again and the connection that has
    // been waiting all along is taken — and then served, which is the whole
    // claim: the pause ends by itself, without anybody having to reconnect and
    // without the server having spent the second asking.
    clock.advance(1'100'000'000);
    CK_CHECK(server.step());
    // At least one: on a system whose failed accept left the first connection
    // on the queue, this pass takes both.
    CK_CHECK(server.client_count() >= 1U);
    ckm::proto::Hello hello;
    hello.build = "a test";
    arriving.say(hello);
    ckm::proto::Message answer;
    CK_CHECK(pump(server, arriving, answer));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(answer));
    forget(socket);
}

namespace {

// Steps the server — advancing the manual clock one tick per pass so the
// flush tick, and with it advance_closes, actually runs — until a message of
// the wanted alternative arrives. Child exits are real-world events, so this
// waits on real time too: bounded, not slept blindly.
template <typename Wanted>
bool await_message(Server& server, ckv::ManualClock& clock, WireClient& client, Wanted& out,
                   int wait_ms = 6000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        clock.advance(34'000'000);
        // Drained AFTER the step even when the step stopped the server: the
        // stopping pass can be the very one that flushed the wanted frame.
        const bool alive = server.step();
        ckm::proto::Message message;
        while (client.take(message)) {
            if (const Wanted* wanted = std::get_if<Wanted>(&message)) {
                out = *wanted;
                return true;
            }
        }
        if (!alive) return false;
        ::usleep(5'000);
    }
    return false;
}

// Greets and attaches one client to the default session, swallowing the
// handshake so a test starts at the part it is about.
bool greet_and_attach(Server& server, WireClient& client) {
    ckm::proto::Hello hello;
    hello.build = "a test";
    client.say(hello);
    ckm::proto::Message greeting;
    if (!pump(server, client, greeting)) return false;
    if (!std::holds_alternative<ckm::proto::HelloAck>(greeting)) return false;
    ckm::proto::Attach attach;
    attach.columns = 40;
    attach.rows = 8;
    client.say(attach);
    ckm::proto::Message attached;
    if (!pump(server, client, attached)) return false;
    return std::holds_alternative<ckm::proto::Attached>(attached);
}

// Waits until the child has drawn `marker` — the only way a test can know a
// real child's trap is installed before it starts sending it signals.
bool await_marker(Server& server, ckv::ManualClock& clock, ckm::server::Terminal& terminal,
                  std::string_view marker, int wait_ms = 6000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        clock.advance(34'000'000);
        if (!server.step()) return false;
        const ckv::core::TerminalSnapshot snapshot = terminal.session().snapshot();
        std::string text;
        for (const ckv::Cell& cell : snapshot.cell_buffer) text += cell.grapheme();
        if (text.find(marker) != std::string::npos) return true;
        ::usleep(5'000);
    }
    return false;
}

}  // namespace

CK_TEST(closing_asks_first_and_the_window_falls_when_the_child_exits) {
    // The close dialog's first promise: the program is ASKED — SIGHUP, then
    // SIGTERM — and the terminal goes when the child does, not when a timer
    // says so. The grace here is an hour, so a TermClosed can only mean the
    // exit was observed.
    const std::filesystem::path socket = private_socket("graceclose");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 3600"));
    const ckm::server::TerminalId id = terminal.id();

    ckm::proto::CloseTerminal close_it;
    close_it.term = id;
    close_it.force = 1;
    close_it.grace_seconds = 3600;
    client.say(close_it);

    ckm::proto::TermClosed closed;
    CK_CHECK(await_message(server, clock, client, closed));
    CK_CHECK(closed.term == id);
    CK_CHECK(closed.exited == 1);
    CK_CHECK(server.terminals().find(id) == nullptr);
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(unticked_kill_keeps_a_program_that_declines_to_quit) {
    // The dialog's unticked promise: a program that ignores the asking keeps
    // running, and its terminal with it. Then the ticked variant ends it, so
    // the same child proves both halves.
    const std::filesystem::path socket = private_socket("keptclose");
    forget(socket);
    ckm::Settings settings = test_settings();
    // Isolated from the empty-session rule: this test is about the grace.
    settings.kill_empty_session = false;
    ckv::ManualClock clock;
    Server server(Server::Options{socket, settings}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    ckm::server::Terminal& terminal = server.open_terminal(
        0, spec_running("trap '' HUP TERM; printf T-READY; while :; do sleep 1; done"));
    const ckm::server::TerminalId id = terminal.id();
    CK_CHECK(await_marker(server, clock, terminal, "T-READY"));

    ckm::proto::CloseTerminal close_it;
    close_it.term = id;
    close_it.force = 0;
    close_it.grace_seconds = 1;
    client.say(close_it);

    // Well past the grace in manual time, with real time for the ignored
    // signals to land. No TermClosed may arrive: the reader said not to kill.
    for (int pass = 0; pass < 40; ++pass) {
        clock.advance(100'000'000);
        CK_CHECK(server.step());
        ckm::proto::Message message;
        while (client.take(message))
            CK_CHECK(!std::holds_alternative<ckm::proto::TermClosed>(message));
        ::usleep(5'000);
    }
    ckm::server::Terminal* const kept = server.terminals().find(id);
    CK_CHECK(kept != nullptr);
    CK_CHECK(kept != nullptr && kept->live());

    // The reader changes their mind: the same close with the kill ticked and
    // no grace ends even a program that ignores every polite signal.
    close_it.force = 1;
    close_it.grace_seconds = 0;
    client.say(close_it);
    ckm::proto::TermClosed closed;
    CK_CHECK(await_message(server, clock, client, closed));
    CK_CHECK(closed.term == id);
    CK_CHECK(server.terminals().find(id) == nullptr);
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_session_emptied_by_a_close_ends_and_takes_the_server_with_it) {
    // kill-empty-session, on the explicit-close path: the last terminal's
    // close ends the session, the watcher is detached to the picker, and a
    // server with no sessions left stops (the session model).
    const std::filesystem::path socket = private_socket("emptyclose");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 3600"));

    ckm::proto::CloseTerminal close_it;
    close_it.term = terminal.id();
    client.say(close_it);

    ckm::proto::Detached detached;
    CK_CHECK(await_message(server, clock, client, detached));
    CK_CHECK(detached.reason == ckm::proto::DetachReason::SessionKilled);
    // A server with nothing to serve is a process holding a socket for
    // nobody: the next step says it stopped.
    clock.advance(34'000'000);
    CK_CHECK(!server.step());
    forget(socket);
}

CK_TEST(a_move_reparents_the_terminal_and_the_child_never_notices) {
    // The session model's move-terminal row: membership changes, the PTY does not. The
    // source's watcher sees the window leave WITHOUT an exit; the counts move
    // one session to the other; the child is still alive afterwards.
    const std::filesystem::path socket = private_socket("moveterm");
    forget(socket);
    ckm::Settings settings = test_settings();
    settings.kill_empty_session = false;
    ckv::ManualClock clock;
    Server server(Server::Options{socket, settings}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 3600"));
    const ckm::server::TerminalId id = terminal.id();

    // A destination that does not exist is an error, said out loud.
    ckm::proto::MoveTerminal wrong;
    wrong.term = id;
    wrong.destination_session = 77;
    client.say(wrong);
    ckm::proto::Error refusal;
    CK_CHECK(await_message(server, clock, client, refusal));
    CK_CHECK(refusal.code == static_cast<std::uint16_t>(ckm::proto::ErrorCode::NoSuchSession));

    // Quiesced first, so that the list read below is the one this `NewSession`
    // produced. The attach at the top of this case left a reader count to state
    // (WP-48) and the tick states it, so there can be a `SessionList` already
    // in flight — naming one session, correctly, and a case that took "the next
    // list" would read that one and conclude the second session had not been
    // made. Draining is the precondition, not a workaround: "the list after the
    // act" is only a well-formed question once nothing is pending from before.
    for (int pass = 0; pass < 8; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        ckm::proto::Message settled;
        while (client.take(settled)) { /* drained */ }
    }

    ckm::proto::NewSession second;
    second.name = "two";
    // An empty destination, asked for explicitly: `spawn_first` is honoured
    // now (it used to be silently ignored, and this test leaned on the
    // defect), and what a move test needs is a session with nothing in it.
    second.spawn_first = 0;
    client.say(second);
    ckm::proto::SessionList with_two;
    CK_CHECK(await_message(server, clock, client, with_two));
    CK_CHECK(with_two.sessions.size() == 2U);
    const std::uint64_t destination = with_two.sessions.back().id;

    ckm::proto::MoveTerminal move;
    move.term = id;
    move.destination_session = destination;
    client.say(move);

    // The window leaves without a death notice: exited says nothing exited.
    ckm::proto::TermClosed left;
    CK_CHECK(await_message(server, clock, client, left));
    CK_CHECK(left.term == id);
    CK_CHECK(left.exited == 0);

    ckm::proto::ListSessions ask;
    client.say(ask);
    ckm::proto::SessionList after;
    CK_CHECK(await_message(server, clock, client, after));
    CK_CHECK(after.sessions.size() == 2U);
    for (const ckm::proto::SessionInfo& info : after.sessions) {
        if (info.id == destination) CK_CHECK(info.terminals == 1U);
        else CK_CHECK(info.terminals == 0U);
    }
    ckm::server::Terminal* const moved = server.terminals().find(id);
    CK_CHECK(moved != nullptr);
    CK_CHECK(moved != nullptr && moved->live());
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_terminal_cannot_be_asked_to_be_larger_than_a_grid_may_be) {
    // A rect on the wire is two `u16`s, and every one of the 65535 values in
    // each was granted: a client could ask this server to allocate four billion
    // cells, and the server would try. The ceiling is the protocol's own, so
    // both ends refuse the same number — and it is also what makes the attach
    // snapshot bounded by arithmetic rather than by hope, because the budget is
    // computed over grids that have a maximum size (R1).
    //
    // Asked one dimension at a time on purpose: proving that a ceiling holds
    // does not require a test that allocates what a missing ceiling would.
    const std::filesystem::path socket = private_socket("toobig");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    ckm::proto::NewTerminal wide;
    wide.command = "sleep 30";
    wide.rect.width = 65535;
    wide.rect.height = 2;
    client.say(wide);

    ckm::proto::TermOpened opened;
    CK_CHECK(await_message(server, clock, client, opened));
    CK_CHECK(opened.columns == ckm::proto::kMaxGridColumns);
    CK_CHECK(opened.rows == 2);

    // And a resize is held to it too — the same client, the same terminal, and
    // the other dimension.
    ckm::proto::MoveResize taller;
    taller.term = opened.term;
    taller.rect.width = 3;
    taller.rect.height = 65535;
    client.say(taller);
    for (int pass = 0; pass < 8; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
    }
    ckm::server::Terminal* const terminal = server.terminals().find(opened.term);
    CK_CHECK(terminal != nullptr);
    if (terminal != nullptr) {
        CK_CHECK(terminal->columns() == 3);
        CK_CHECK(terminal->rows() == static_cast<int>(ckm::proto::kMaxGridRows));
    }

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_child_that_exits_on_its_own_closes_its_window) {
    // A window whose program ends by itself, with nobody having asked. The exit
    // is the one change a terminal makes after it stops making any others, so a
    // transport gated on damage — which this is — has to look at exactly that
    // flag; before it did, a shell that ran out of input left a window that
    // went on looking alive over nothing at all (m-exit-msg).
    const std::filesystem::path socket = private_socket("selfexit");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("exit 3"));
    const ckm::server::TerminalId id = terminal.id();

    ckm::proto::TermClosed closed;
    CK_CHECK(await_message(server, clock, client, closed));
    CK_CHECK(closed.term == id);
    CK_CHECK(closed.exited == 1);
    // What it exited with, not a stand-in: "exited 3" and "exited 0" are
    // different windows to a reader.
    CK_CHECK(closed.exit_status == 3);
    // Held, because the reader's default keeps the window of a program that
    // failed — what it printed there is the only evidence there is (the session model
    // on-exit). So the terminal is still the server's, and its last screen with
    // it.
    CK_CHECK(closed.hold == 1);
    CK_CHECK(server.terminals().find(id) != nullptr);

    // And exactly once. `live()` is false forever afterwards, so a server that
    // announced what it saw rather than what it had not yet said would send one
    // of these every frame, for as long as the reader left the window open.
    for (int tick = 0; tick < 20; ++tick) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        ckm::proto::Message message;
        while (client.take(message))
            CK_CHECK(!std::holds_alternative<ckm::proto::TermClosed>(message));
        ::usleep(2'000);
    }

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_client_that_never_reads_is_let_go_rather_than_queued_for) {
    // Three marks, three questions. The backlog mark asks whether more SCREEN
    // is worth queueing, the high-water mark asks whether the stream is in
    // trouble, and this one asks whether the peer is reading AT ALL — the only
    // one of the three whose answer is to end the connection.
    //
    // Driven with answers rather than with screen, because screen is already
    // bounded: past a quarter of a megabyte a client is sent no deltas at all.
    // What nothing bounded was a client that keeps ASKING and never collects,
    // and thirty-two megabytes of a server's memory held for one such peer is
    // what this ends.
    const std::filesystem::path socket = private_socket("laggard");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient deaf;
    CK_CHECK(deaf.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    deaf.say(hello);

    // Enough sessions that one answer is worth having. A hundred is a lot for
    // one machine and nothing for a protocol: the point is that the size of an
    // answer is the reader's business and the queue holding it is the server's.
    for (int index = 0; index < 100; ++index) {
        ckm::proto::NewSession make;
        make.name = "a-named-session-" + std::to_string(index);
        deaf.say(make);
    }
    // Four and a half kilobytes an answer, eight thousand times: comfortably
    // past the limit, from a client that has not read a byte of any of it.
    for (int index = 0; index < 8'000; ++index) deaf.say(ckm::proto::ListSessions{});

    std::size_t highest_queued = 0;
    for (int pass = 0; pass < 200 && server.client_count() > 0; ++pass) {
        (void)deaf.stream.flush();
        CK_CHECK(server.step());
        highest_queued = std::max(highest_queued, server.queued_bytes());
        clock.advance(34'000'000);
    }
    // Let go, rather than queued for. No message went with it: a connection
    // with thirty-two megabytes waiting is one nothing further will be read
    // from, so the client meets EOF — a path it already treats as a lost
    // connection — and the reason lives in the server's log.
    CK_CHECK(server.client_count() == 0U);
    CK_CHECK(highest_queued <= ckm::platform::Stream::kHardLimitBytes);

    // And the server is unharmed: one peer that stopped reading is not a reason
    // to stop serving the others.
    WireClient ordinary;
    CK_CHECK(ordinary.connect(socket));
    ckm::proto::Hello greeting;
    greeting.build = "a test";
    ordinary.say(greeting);
    ckm::proto::Message answer;
    CK_CHECK(pump(server, ordinary, answer));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(answer));
    forget(socket);
}

CK_TEST(a_program_that_ignores_sigterm_does_not_hold_the_loop) {
    // The escalation at the end of a grace has to cost a SIGNAL, not a wait.
    //
    // `Terminals::close()` escalates too — ckVision reaps for a second, sends
    // SIGKILL, and reaps for two more — and every one of those seconds is spent
    // inside this loop, with every other terminal on the machine frozen behind
    // it. Three programs that ignore SIGTERM used to cost one pass of three
    // seconds; a session of ten cost half a minute (M-S3). So the server asks,
    // watches, kills, watches, and only then closes — and by the time it closes
    // there is nothing left to wait for.
    const std::filesystem::path socket = private_socket("nokillwait");
    forget(socket);
    ckm::Settings settings = test_settings();
    settings.kill_empty_session = false;
    ckv::ManualClock clock;
    Server server(Server::Options{socket, settings}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));
    // A second session, so that ending the first one does not end the server
    // with it — what is being timed here is the loop, and a loop that has
    // stopped cannot be slow.
    ckm::proto::NewSession keep;
    keep.name = "keep";
    client.say(keep);
    ckm::proto::SessionList listed;
    CK_CHECK(await_message(server, clock, client, listed));

    // Three programs that decline every polite signal, each waited for until it
    // has drawn its marker — a trap that is not installed yet is a program that
    // would have gone on the asking.
    std::vector<ckm::server::TerminalId> ids;
    for (int index = 0; index < 3; ++index) {
        ckm::server::Terminal& terminal = server.open_terminal(
            0, spec_running("trap '' HUP TERM; printf T-READY; while :; do sleep 1; done"));
        ids.push_back(terminal.id());
        CK_CHECK(await_marker(server, clock, terminal, "T-READY"));
    }

    ckm::proto::KillSession kill;
    kill.force = 1;
    kill.grace_seconds = 0;  // no grace at all: the escalation is due immediately
    client.say(kill);

    // Every pass from here is timed, and the terminals have to be gone by the
    // end of it. Both halves matter: a loop that never blocks but never ends
    // the children either would pass one of these and fail the promise.
    long slowest_ms = 0;
    bool gone = false;
    for (int pass = 0; pass < 300 && !gone; ++pass) {
        clock.advance(34'000'000);
        const auto pass_started = std::chrono::steady_clock::now();
        CK_CHECK(server.step());
        const long spent = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - pass_started)
                                                 .count());
        slowest_ms = std::max(slowest_ms, spent);
        gone = true;
        for (const ckm::server::TerminalId id : ids)
            if (server.terminals().find(id) != nullptr) gone = false;
        ::usleep(5'000);
    }
    CK_CHECK(gone);
    // One pass of the loop, however much killing it had to do. The bound is
    // generous — the passes actually cost under a millisecond — because what is
    // being pinned is that nothing WAITED, not how fast this machine is.
    CK_CHECK(slowest_ms < 500);

    // And the connection was answerable throughout, which is what "does not
    // hold the loop" means to a reader: their other session is still theirs.
    client.say(ckm::proto::Ping{4242});
    ckm::proto::Pong pong;
    CK_CHECK(await_message(server, clock, client, pong));
    CK_CHECK(pong.nonce == 4242U);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_move_to_a_new_session_creates_one_around_the_terminal) {
    // The close dialog's "a new session" answer, at the wire: the flag —
    // not a magic id — creates the destination and the terminal arrives in
    // it. With kill-empty-session on (the default), the emptied source ends
    // and its watcher lands in the picker, which is exactly what the reader
    // who moved their last terminal away should see.
    const std::filesystem::path socket = private_socket("movenew");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 3600"));
    const ckm::server::TerminalId id = terminal.id();

    ckm::proto::MoveTerminal move;
    move.term = id;
    move.to_new_session = 1;
    client.say(move);

    ckm::proto::Detached detached;
    CK_CHECK(await_message(server, clock, client, detached));
    CK_CHECK(detached.reason == ckm::proto::DetachReason::SessionKilled);
    // The program survived its session: it lives in the created one.
    ckm::server::Terminal* const moved = server.terminals().find(id);
    CK_CHECK(moved != nullptr);
    CK_CHECK(moved != nullptr && moved->live());
    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_flood_in_one_session_does_not_re_snapshot_another) {
    // C2. The latch a flooded terminal sets is sticky — it clears only when a
    // snapshot is taken — and the scan that reads it used to dirty every
    // attached client on the server, with no session filter at all. One noisy
    // shell in one session therefore turned delta delivery into a full snapshot
    // per tick for every reader on the machine, including the ones whose
    // sessions that terminal has nothing to do with, and it did not stop.
    const std::filesystem::path socket = private_socket("floodfilter");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    // The reader watching the flood, who has stopped reading. That is what
    // keeps the latch on: a client that cannot be healed is a snapshot that is
    // never taken, and a snapshot that is never taken is a latch that stays set
    // for as long as the server runs.
    WireClient watcher;
    CK_CHECK(watcher.connect(socket));
    CK_CHECK(greet_and_attach(server, watcher));

    // And a reader in another session entirely, who reads everything promptly
    // and is owed nothing but news of terminals they actually have.
    WireClient other;
    CK_CHECK(other.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    other.say(hello);
    ckm::proto::Message greeting;
    CK_CHECK(pump(server, other, greeting));
    ckm::proto::NewSession second;
    second.name = "second";
    other.say(second);
    ckm::proto::SessionList listed;
    CK_CHECK(await_message(server, clock, other, listed));
    CK_CHECK(listed.sessions.size() == 2U);
    ckm::proto::Attach attach;
    attach.session = listed.sessions.back().id;
    attach.columns = 40;
    attach.rows = 8;
    other.say(attach);
    ckm::proto::Attached attached;
    CK_CHECK(await_message(server, clock, other, attached));

    // The noisy terminal, in the FIRST session — the one the silent watcher
    // holds and the other reader has never seen.
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    std::string burst;
    for (int line = 0; line < 24; ++line)
        burst += "a line of output that fills the screen " + std::to_string(line) + "\r\n";

    // Enough queued for the silent watcher that it is past the mark and cannot
    // be healed while it stays silent.
    int ticks = 0;
    for (; ticks < 4000 && server.queued_bytes() <= ckm::platform::Stream::kDeltaBacklogBytes;
         ++ticks) {
        terminal.session().feed_output(burst);
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        ckm::proto::Message arrived;
        while (other.take(arrived)) {
        }
    }
    CK_CHECK(server.queued_bytes() > ckm::platform::Stream::kDeltaBacklogBytes);

    // The flood: more scrolled away in one tick than a delta may carry, which
    // is what makes that terminal say it needs a snapshot rather than sending
    // one — and the latch stays set until somebody takes it.
    std::string flood;
    for (int line = 0; line < 400; ++line)
        flood += "scrolled away " + std::to_string(line) + "\r\n";
    terminal.session().feed_output(flood);
    clock.advance(34'000'000);
    CK_CHECK(server.step());

    int snapshots_for_the_other = 0;
    for (int tick = 0; tick < 40; ++tick) {
        // The program carries on saying a little — which is what keeps the
        // latch being noticed: a differ with no damage to look at never gets to
        // see that its terminal has calmed down, so a flood followed by total
        // silence sets nothing at all. And because the watcher of that session
        // is past its mark and cannot be healed, the latch is never taken, and
        // it is set again on every tick. That is the "forever" of C2.
        terminal.session().feed_output("still going\r\n");
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        ckm::proto::Message arrived;
        while (other.take(arrived))
            if (std::holds_alternative<ckm::proto::Attached>(arrived)) ++snapshots_for_the_other;
    }
    // Not one. Not one per tick, and not one at all: nothing about that
    // terminal is this reader's business.
    CK_CHECK(snapshots_for_the_other == 0);
    // And the flood WAS noticed, by the client it concerns — which is the half
    // of this that has to keep working.
    CK_CHECK(server.waiting_to_heal());

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_client_that_drains_slowly_is_healed_once_and_not_forever) {
    // The other half of M-S5. The latch is sticky and the heal gate used to
    // want a queue that was completely empty while the delta gate stopped at a
    // quarter of a megabyte — so a client whose socket drained slower than a
    // snapshot could oscillate between the two, and `send_snapshot` cleared the
    // very flag its own overflow had just set. What a reader who fell behind is
    // owed is one snapshot, and then deltas again.
    const std::filesystem::path socket = private_socket("healonce");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    std::string flood;
    for (int line = 0; line < 400; ++line)
        flood += "scrolled away " + std::to_string(line) + "\r\n";
    terminal.session().feed_output(flood);
    clock.advance(34'000'000);
    CK_CHECK(server.step());
    // And then the program calms down and says one more thing, which is what a
    // flood actually looks like: `yes` for a while, then a prompt. A terminal
    // that says nothing further reports no damage at all, and a differ with no
    // damage to look at is a differ that never gets to notice it has calmed —
    // so the history stays behind until the next thing the child prints, by
    // design (diff_engine's own early return).
    terminal.session().feed_output("and then it calmed down\r\n");

    int snapshots = 0;
    for (int tick = 0; tick < 60; ++tick) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        ckm::proto::Message arrived;
        while (client.take(arrived))
            if (std::holds_alternative<ckm::proto::Attached>(arrived)) ++snapshots;
    }
    // Exactly one, over sixty ticks of a terminal that has been quiet since.
    // The latch is cleared by the snapshot that answers it, and a client that
    // is up to date is not dirty.
    CK_CHECK(snapshots == 1);
    CK_CHECK(!server.waiting_to_heal());

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_snapshot_that_fills_the_queue_is_not_asked_for_twice) {
    // A repair does not mark what it repaired as broken.
    //
    // A snapshot is held to a four-megabyte budget and the stream's high-water
    // mark is four megabytes, so a session with a real history lands on the
    // wrong side of the mark every single time it is sent. `Stream::send`
    // answers false — meaning "stop adding", because the bytes ARE queued and
    // will arrive — and a server that read that as "this client is behind"
    // marked it for another snapshot, queued a second copy of the same four
    // megabytes the moment the first drained, and did it again. That is the
    // M-S5 loop at four-megabyte granularity instead of per tick, on a client
    // that is missing nothing at all. What a client that has really missed
    // something needs is said by the delta gate, which marks it when it
    // actually skips a delta.
    const std::filesystem::path socket = private_socket("bigsnap");
    forget(socket);
    ckm::Settings settings = test_settings();
    settings.scrollback = 2000;
    ckv::ManualClock clock;
    Server server(Server::Options{socket, settings}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    // A terminal with a history worth sending: two hundred columns of text with
    // no two neighbours alike, because a screenful of blanks run-length encodes
    // to almost nothing and would make this measure nothing.
    ckm::server::TerminalSpec spec = spec_running("sleep 30");
    spec.columns = 200;
    spec.rows = 10;
    spec.pixel_width = 200 * 9;
    spec.pixel_height = 10 * 18;
    ckm::server::Terminal& terminal = server.open_terminal(0, spec);
    std::string chunk;
    for (std::size_t line = 0; line < 1'500; ++line) {
        // One column short of the width, so no line depends on how a write
        // that lands exactly on the last column is wrapped.
        for (std::size_t column = 0; column + 1 < 200U; ++column)
            chunk.push_back(static_cast<char>('a' + ((line * 7 + column) % 26)));
        chunk += "\r\n";
        // Fed in pieces the emulator's own step budget can swallow whole: a
        // larger call would leave a tail pending and the next one would find
        // less room than it needed.
        if (chunk.size() < (16U << 10U)) continue;
        terminal.session().feed_output(chunk);
        chunk.clear();
    }
    if (!chunk.empty()) terminal.session().feed_output(chunk);
    // The history really is there before anybody attaches. Said out loud
    // because everything below is a measurement of what it costs to send, and
    // a shortfall here — a parser budget swallowed, a scrollback setting that
    // did not reach the emulator — would quietly turn that measurement into
    // nothing that passed.
    CK_CHECK(terminal.scrollback().size() / 200U > 1'200U);

    WireClient client;
    CK_CHECK(client.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    client.say(hello);
    ckm::proto::Message greeting;
    CK_CHECK(pump(server, client, greeting));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(greeting));

    // Something already waiting on the connection before the snapshot goes,
    // from a client that asked and has not collected — so that the snapshot
    // lands on top of it and the queue is over the mark rather than a few
    // kilobytes under it. Answers, not screen: what is being set up is a full
    // queue, and the client is owed none of it back.
    for (int round = 0; round < 400 && server.queued_bytes() < 512u * 1024u; ++round) {
        for (int index = 0; index < 2'000; ++index) client.say(ckm::proto::Ping{7});
        // Pushed all the way out before the next round, and it takes more than
        // one flush: a write takes what the socket will hold and queues the
        // rest, so a round that pushed once left more behind than it sent. A
        // backlog on THIS side is the opposite of what is being built — it
        // would mean the asking never arrived, and the `Attach` below would sit
        // behind a megabyte of pings that nothing ever pushes.
        for (int push = 0; push < 500 && client.stream.wants_write(); ++push) {
            (void)client.stream.flush();
            CK_CHECK(server.step());
        }
    }
    CK_CHECK(server.queued_bytes() >= 512u * 1024u);
    CK_CHECK(!client.stream.wants_write());

    ckm::proto::Attach attach;
    attach.columns = 200;
    attach.rows = 10;
    client.say(attach);
    for (int pass = 0; pass < 8 && server.queued_bytes() <= ckm::platform::Stream::kHighWaterBytes;
         ++pass) {
        (void)client.stream.flush();
        CK_CHECK(server.step());
    }
    // The measurement this case rests on: the snapshot really did take the
    // queue past the high-water mark, which is what makes `send` answer false
    // for a client that has missed nothing.
    CK_CHECK(server.queued_bytes() > ckm::platform::Stream::kHighWaterBytes);

    // And now it drains, at the rate a socket moves: one pass of the loop puts
    // a buffer's worth into the pipe and the client takes a buffer's worth out,
    // and a Unix socket's buffer is small — eight kilobytes where this was
    // measured — so four and a half megabytes is some hundreds of passes, and
    // how many is a fact about the host rather than about this server. Hence a
    // loop that runs until the queue is empty and then sixty passes more,
    // rather than a guessed count: sixty is far longer than the heal gate needs
    // to notice a drained queue and put a second snapshot on it, which is the
    // thing that must not happen.
    int snapshots = 0;
    int drained_at = -1;
    int tick = 0;
    ckm::proto::Attached received;
    for (; tick < 4'000; ++tick) {
        clock.advance(34'000'000);
        (void)client.stream.flush();
        CK_CHECK(server.step());
        std::string arrived;
        (void)client.stream.receive(arrived, 64u * 1024u);
        if (!arrived.empty()) CK_CHECK(client.reader.append(arrived));
        ckm::proto::Message message;
        while (client.reader.next(message) == ckm::proto::DecodeError::None)
            if (const auto* attached = std::get_if<ckm::proto::Attached>(&message)) {
                ++snapshots;
                received = *attached;
            }
        if (drained_at < 0 && server.queued_bytes() == 0U) drained_at = tick;
        if (drained_at >= 0 && tick > drained_at + 60) break;
    }
    // Said out loud, so that a failure here reads as a measurement rather than
    // as a riddle: how many snapshots crossed, what is still queued, and how
    // long it took to get there.
    std::printf("  [snapshot drain] %d Attached, %zu bytes left after %d ticks\n", snapshots,
                server.queued_bytes(), tick);

    // One. Not one per drain cycle, and nothing left waiting to be healed: the
    // client has everything the server had when it attached.
    CK_CHECK(snapshots == 1);
    CK_CHECK(!server.waiting_to_heal());
    CK_CHECK(server.queued_bytes() == 0U);
    // Complete, which a partial frame could never be: a snapshot that had been
    // cut short would not decode at all, and this one carries the terminal's
    // whole screen and the history that fitted the budget.
    CK_CHECK(received.snapshot.terminals.size() == 1U);
    if (received.snapshot.terminals.size() == 1U) {
        const ckm::proto::TerminalState& state = received.snapshot.terminals.front();
        CK_CHECK(state.term == terminal.id());
        CK_CHECK(state.columns == 200);
        CK_CHECK(state.rows == 10);
        CK_CHECK(!state.grid.empty());
        // Most of what the server held, which is what a four-megabyte budget
        // buys at this width — and far more than a truncated frame could
        // possibly carry.
        CK_CHECK(state.scrollback.size() > 800U);
    }

    server.terminals().close_all();
    forget(socket);
}

namespace {

// One window's place, as a client would report it.
ckm::proto::LayoutEntry placed(std::uint64_t term, std::int16_t x, std::int16_t y,
                               std::uint16_t width, std::uint16_t height, std::uint16_t z,
                               std::uint8_t zoomed = 0) {
    return ckm::proto::LayoutEntry{term, ckm::proto::Rect{x, y, width, height}, z, zoomed,
                                   ckm::proto::TileFraction{}};
}

// The entry for one terminal, whatever order the message carried them in: the
// server states a session's windows in the order the session holds them, which
// is not the order a client reported them in.
const ckm::proto::LayoutEntry* entry_for(const std::vector<ckm::proto::LayoutEntry>& entries,
                                         std::uint64_t term) {
    for (const ckm::proto::LayoutEntry& entry : entries)
        if (entry.term == term) return &entry;
    return nullptr;
}

const ckm::proto::TerminalState* state_for(const ckm::proto::Attached& attached,
                                           std::uint64_t term) {
    for (const ckm::proto::TerminalState& state : attached.snapshot.terminals)
        if (state.term == term) return &state;
    return nullptr;
}

}  // namespace

CK_TEST(a_reported_layout_outlives_the_client_that_reported_it) {
    // WP-28's whole claim, and the thing M-R3 named as missing: a window's place
    // is session state, held by the server, and it survives the client that
    // arranged it exactly as the screen and the history already do (the session model).
    // Set, stored, and stated again on the snapshot the next attach is given —
    // with nothing on the other end interpreting it, because what a reattaching
    // client DOES with a layout is a restoration policy this package
    // deliberately has none of (WP-30).
    const std::filesystem::path socket = private_socket("layout");
    forget(socket);
    ckm::Settings settings = test_settings();
    settings.kill_empty_session = false;
    ckv::ManualClock clock;
    Server server(Server::Options{socket, settings}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    const ckm::server::TerminalId first =
        server.open_terminal(0, spec_running("sleep 3600")).id();
    const ckm::server::TerminalId second =
        server.open_terminal(0, spec_running("sleep 3600")).id();

    // Two windows, one of them dragged off the left edge of the desktop and one
    // maximized over it. The negative origin is deliberate: it is an
    // arrangement a reader can really make, it is the case WP-30's move rule
    // exists for, and a codec or a store that clamped it away would look
    // correct everywhere else.
    ckm::proto::SetLayout report;
    report.entries.push_back(placed(first, -3, 2, 30, 9, 0));
    report.entries.push_back(placed(second, 12, 1, 24, 6, 1, /*zoomed=*/1));
    client.say(report);

    // The watching client is told, in one message for the whole session: a
    // z-order is a position among windows, so stating one window's without the
    // others' says nothing a mirror could apply.
    ckm::proto::LayoutDelta stated;
    CK_CHECK(await_message(server, clock, client, stated));
    CK_CHECK(stated.entries.size() == 2U);
    CK_CHECK(stated.desktop_columns == 40 && stated.desktop_rows == 8);
    const ckm::proto::LayoutEntry* const said_first = entry_for(stated.entries, first);
    const ckm::proto::LayoutEntry* const said_second = entry_for(stated.entries, second);
    CK_CHECK(said_first != nullptr && said_second != nullptr);
    if (said_first != nullptr) CK_CHECK(said_first->rect == (ckm::proto::Rect{-3, 2, 30, 9}));
    if (said_second != nullptr) {
        CK_CHECK(said_second->z_order == 1);
        CK_CHECK(said_second->zoomed == 1);
    }

    // The client goes. Its terminals stay, and so does what it said about where
    // their windows were — checked against the server's own state, with nobody
    // attached to be told it, because "the storage is real" is a claim about the
    // server and not about a message.
    client.say(ckm::proto::Detach{});
    ckm::proto::Detached gone;
    CK_CHECK(await_message(server, clock, client, gone));
    const ckm::server::Terminal* const held = server.terminals().find(first);
    CK_CHECK(held != nullptr);
    if (held != nullptr) {
        CK_CHECK(held->layout().rect == (ckm::proto::Rect{-3, 2, 30, 9}));
        CK_CHECK(!held->layout().zoomed);
    }

    // And it comes back on the snapshot, which is where a reattaching client
    // finds every other thing it lost — the grid, the history, the modes, the
    // exit — rather than in a message of its own.
    ckm::proto::Attach again;
    again.columns = 40;
    again.rows = 8;
    client.say(again);
    ckm::proto::Attached snapshot;
    CK_CHECK(await_message(server, clock, client, snapshot));
    const ckm::proto::TerminalState* const restated_first = state_for(snapshot, first);
    const ckm::proto::TerminalState* const restated_second = state_for(snapshot, second);
    CK_CHECK(restated_first != nullptr && restated_second != nullptr);
    if (restated_first != nullptr) {
        CK_CHECK(restated_first->rect == (ckm::proto::Rect{-3, 2, 30, 9}));
        CK_CHECK(restated_first->z_order == 0);
        CK_CHECK(restated_first->zoomed == 0);
    }
    if (restated_second != nullptr) {
        CK_CHECK(restated_second->rect == (ckm::proto::Rect{12, 1, 24, 6}));
        CK_CHECK(restated_second->z_order == 1);
        CK_CHECK(restated_second->zoomed == 1);
    }

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_layout_is_stated_when_it_moves_and_not_once_a_tick) {
    // Edge-triggered, like the marks: a client that reports the same
    // arrangement again — which is what a mirror applying what it was told
    // produces — must not be answered. An unconditional producer and a client
    // that applies what it is told are a loop with no end, and it would run at
    // tick rate for as long as a session was attached.
    const std::filesystem::path socket = private_socket("layoutonce");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));
    const ckm::server::TerminalId term = server.open_terminal(0, spec_running("sleep 3600")).id();

    ckm::proto::SetLayout report;
    report.entries.push_back(placed(term, 4, 4, 20, 6, 0));
    client.say(report);
    ckm::proto::LayoutDelta stated;
    CK_CHECK(await_message(server, clock, client, stated));
    CK_CHECK(stated.entries.size() == 1U);

    // Ticks pass with the arrangement unchanged, and then the same report is
    // made again. Neither produces a second statement of it.
    const auto no_layout_for = [&](int passes) {
        for (int pass = 0; pass < passes; ++pass) {
            clock.advance(34'000'000);
            if (!server.step()) return false;
            ckm::proto::Message arrived;
            while (client.take(arrived))
                if (std::holds_alternative<ckm::proto::LayoutDelta>(arrived)) return false;
        }
        return true;
    };
    CK_CHECK(no_layout_for(6));
    client.say(report);
    CK_CHECK(no_layout_for(6));

    // A window that really moves is news again.
    ckm::proto::SetLayout moved;
    moved.entries.push_back(placed(term, 5, 4, 20, 6, 0));
    client.say(moved);
    ckm::proto::LayoutDelta after;
    CK_CHECK(await_message(server, clock, client, after));
    CK_CHECK(after.entries.size() == 1U);
    CK_CHECK(after.entries.front().rect.x == 5);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_layout_report_is_filed_against_a_session_or_answered) {
    // Two halves of one rule. A terminal a report names that this client does
    // not watch — one that closed a moment ago, one that moved away — is STALE
    // rather than wrong, and is skipped in silence: a report describes an
    // arrangement at a moment, and a window leaving it races the report every
    // time. A client with no session at all has said something that cannot be
    // filed anywhere, and is told so, because a request that goes unanswered
    // looks to a reader exactly like a server that has hung.
    const std::filesystem::path socket = private_socket("layoutstale");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient watcher;
    CK_CHECK(watcher.connect(socket));
    CK_CHECK(greet_and_attach(server, watcher));
    const ckm::server::TerminalId term =
        server.open_terminal(0, spec_running("sleep 3600")).id();

    // One real window and one id that never existed, in the same report. The
    // real one is filed; the connection stays up.
    ckm::proto::SetLayout mixed;
    mixed.entries.push_back(placed(4242, 0, 0, 10, 4, 0));
    mixed.entries.push_back(placed(term, 6, 3, 18, 5, 0));
    watcher.say(mixed);
    ckm::proto::LayoutDelta stated;
    CK_CHECK(await_message(server, clock, watcher, stated));
    CK_CHECK(stated.entries.size() == 1U);
    CK_CHECK(stated.entries.front().term == term);
    CK_CHECK(stated.entries.front().rect == (ckm::proto::Rect{6, 3, 18, 5}));

    // And a client that greeted but never attached is answered rather than
    // ignored — and rather than being allowed to state an arrangement for a
    // session it is not watching.
    WireClient stranger;
    CK_CHECK(stranger.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    stranger.say(hello);
    ckm::proto::Message greeting;
    CK_CHECK(pump(server, stranger, greeting));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(greeting));
    ckm::proto::SetLayout unattached;
    unattached.entries.push_back(placed(term, 1, 1, 8, 4, 0));
    stranger.say(unattached);
    ckm::proto::Error refusal;
    CK_CHECK(await_message(server, clock, stranger, refusal));
    CK_CHECK(refusal.code == static_cast<std::uint16_t>(ckm::proto::ErrorCode::NoSuchSession));
    const ckm::server::Terminal* const untouched = server.terminals().find(term);
    CK_CHECK(untouched != nullptr);
    if (untouched != nullptr)
        CK_CHECK(untouched->layout().rect == (ckm::proto::Rect{6, 3, 18, 5}));

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(new_session_spawns_its_first_terminal_and_the_command_reaches_the_child) {
    // The fields `NewSession` always carried and the handler ignored (found by
    // WP-11): `spawn_first` defaults to ON, so the silent half was the
    // default — `ckmux new 'cmd'` made an empty session and dropped the
    // command on the floor.
    const std::filesystem::path socket = private_socket("spawn");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    client.say(hello);
    ckm::proto::Message greeting;
    CK_CHECK(pump(server, client, greeting));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(greeting));

    ckm::proto::NewSession request;
    request.name = "work";
    request.command = "printf NS-STAMP; sleep 30";
    client.say(request);
    ckm::proto::Message answer;
    CK_CHECK(pump(server, client, answer));
    const auto* list = std::get_if<ckm::proto::SessionList>(&answer);
    CK_CHECK(list != nullptr);
    bool counted = false;
    if (list != nullptr) {
        for (const ckm::proto::SessionInfo& info : list->sessions)
            if (info.name == "work") counted = info.terminals == 1;
    }
    CK_CHECK(counted);

    // The command is not merely stored: the child is real, and what it printed
    // is on the terminal's own grid. Stepped against the wall deadline because
    // a child schedules itself; reaching the deadline is the failure.
    bool stamped = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!stamped && std::chrono::steady_clock::now() < deadline) {
        CK_CHECK(server.step());
        for (const auto id : server.terminals().ids()) {
            ckm::server::Terminal* const terminal = server.terminals().find(id);
            if (terminal == nullptr) continue;
            const ckv::term::TerminalSnapshot snapshot = terminal->snapshot();
            std::string text;
            for (const ckv::Cell& cell : snapshot.cell_buffer)
                if (!cell.is_continuation()) text += cell.grapheme();
            if (text.find("NS-STAMP") != std::string::npos) stamped = true;
        }
    }
    CK_CHECK(stamped);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(new_session_spawn_first_off_means_an_empty_session_on_purpose) {
    const std::filesystem::path socket = private_socket("nospawn");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    client.say(hello);
    ckm::proto::Message greeting;
    CK_CHECK(pump(server, client, greeting));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(greeting));

    ckm::proto::NewSession request;
    request.name = "empty-on-purpose";
    request.spawn_first = 0;
    client.say(request);
    ckm::proto::Message answer;
    CK_CHECK(pump(server, client, answer));
    const auto* list = std::get_if<ckm::proto::SessionList>(&answer);
    CK_CHECK(list != nullptr);
    bool empty = false;
    if (list != nullptr) {
        for (const ckm::proto::SessionInfo& info : list->sessions)
            if (info.name == "empty-on-purpose") empty = info.terminals == 0;
    }
    CK_CHECK(empty);
    CK_CHECK(server.terminals().ids().empty());

    forget(socket);
}

CK_TEST(a_session_at_its_terminal_limit_is_told_so_rather_than_growing) {
    // The session model's new-terminal row: "Session at terminal limit (config, default
    // 64) → error". A limit rather than none because a session is something a
    // reader manages by hand, and a script asking for terminals in a loop
    // would otherwise take the machine's descriptors with it.
    const std::filesystem::path socket = private_socket("limit");
    forget(socket);
    ckm::Settings settings = test_settings();
    // Two, so the test is about the boundary rather than about 64 PTYs.
    settings.max_terminals = 2;
    ckv::ManualClock clock;
    Server server(Server::Options{socket, settings}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    // Up to the limit is allowed, and each one really arrives — a limit that
    // refused early would pass an "is it refused" test while costing a reader
    // a terminal they were entitled to.
    for (int wanted = 0; wanted < 2; ++wanted) {
        ckm::proto::NewTerminal ask;
        ask.command = "sleep 3600";
        client.say(ask);
        ckm::proto::TermOpened opened;
        CK_CHECK(await_message(server, clock, client, opened));
        CK_CHECK(opened.term != 0U);
    }

    // The one past it is refused, and refused with a sentence that names the
    // key a reader would have to change — an error saying only "no" leaves
    // them to guess which of the several limits in this program they met.
    ckm::proto::NewTerminal too_many;
    too_many.command = "sleep 3600";
    client.say(too_many);
    ckm::proto::Error refusal;
    CK_CHECK(await_message(server, clock, client, refusal));
    CK_CHECK(refusal.code == static_cast<std::uint16_t>(ckm::proto::ErrorCode::LimitReached));
    CK_CHECK(refusal.context == "NewTerminal");
    CK_CHECK(refusal.human.find("max-terminals") != std::string::npos);
    // Not a "no such" code: the reader named something real and the answer is
    // still no, which is a different sentence and a different remedy.
    CK_CHECK(refusal.code != static_cast<std::uint16_t>(ckm::proto::ErrorCode::NoSuchSession));

    // And the session is unchanged by the refusal rather than left in some
    // half-grown state. Asked over the wire, because that is the count a
    // reader's picker shows and therefore the one that matters.
    client.say(ckm::proto::ListSessions{});
    ckm::proto::SessionList listed;
    CK_CHECK(await_message(server, clock, client, listed));
    CK_CHECK(!listed.sessions.empty());
    if (!listed.sessions.empty()) CK_CHECK(listed.sessions.front().terminals == 2U);
}

CK_TEST(a_readers_focus_is_their_own_and_the_session_only_says_where_to_start) {
    // WP-41. `Session::focused` was written by every `Input` and read by every
    // snapshot and layout delta, which is one answer for a question that has
    // one per reader: with two clients attached, two readers being "in" one
    // terminal is nonsense, and a `LayoutDelta` carrying somebody else's focus
    // moves this reader's cursor because that one clicked.
    //
    // The split is seed-versus-current. The session keeps `last_focused` —
    // where a reader STARTS, which is what `Snapshot::focused_term` has always
    // meant on the wire: what a newcomer is told, once. Each client keeps its
    // own current focus. Simultaneous attach is WP-44's, so what is provable
    // today is that the two readings are now DIFFERENT things, which is the
    // part that has to be right before a second client exists.
    const std::filesystem::path socket = private_socket("focus-own");
    forget(socket);
    ckm::Settings settings = test_settings();
    settings.kill_empty_session = false;
    ckv::ManualClock clock;
    Server server(Server::Options{socket, settings}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient first;
    CK_CHECK(first.connect(socket));
    CK_CHECK(greet_and_attach(server, first));

    ckm::server::Terminal& one = server.open_terminal(0, spec_running("sleep 3600"));
    ckm::server::Terminal& two = server.open_terminal(0, spec_running("sleep 3600"));
    CK_CHECK(one.id() != two.id());

    // This reader goes to the first terminal by typing into it, which is the
    // only statement of focus the protocol carries.
    ckm::proto::Input keys;
    keys.term = one.id();
    keys.bytes = "x";
    first.say(keys);
    (void)pump_expecting_silence(server, first, 3);

    // A second reader arrives. Takeover is still the rule (WP-44 changes
    // that), so what this shows is the SEED: they are told where the last
    // reader was, rather than nothing.
    WireClient second;
    CK_CHECK(second.connect(socket));
    ckm::proto::Hello hello;
    hello.build = "a test";
    second.say(hello);
    ckm::proto::Message greeting;
    CK_CHECK(pump(server, second, greeting));
    ckm::proto::Attach attach;
    attach.columns = 40;
    attach.rows = 8;
    second.say(attach);
    ckm::proto::Message attached;
    CK_CHECK(pump(server, second, attached));
    const auto* const handed = std::get_if<ckm::proto::Attached>(&attached);
    CK_CHECK(handed != nullptr);
    if (handed != nullptr) {
        // Where the last reader was, not zero and not the newest terminal.
        CK_CHECK(handed->snapshot.focused_term == one.id());
    }

    // And once THIS reader states a focus of their own, they are given theirs
    // rather than the session's seed — which has to be shown on a case where
    // the two DIFFER, or the assertion proves nothing. `Input` moves both at
    // once, so it cannot separate them; opening a terminal moves the seed and
    // leaves the reader where they are.
    //
    // Found by mutation: an earlier version focused `two` and re-attached,
    // and a snapshot that ignored the client entirely and always seeded passed
    // it, because after that `Input` the seed was `two` as well.
    ckm::proto::Input theirs;
    theirs.term = two.id();
    theirs.bytes = "y";
    second.say(theirs);
    (void)pump_expecting_silence(server, second, 3);

    // A third terminal, opened while the reader sits in the second. The seed
    // follows the new terminal; the reader does not.
    ckm::server::Terminal& three = server.open_terminal(0, spec_running("sleep 3600"));
    CK_CHECK(three.id() != two.id());

    second.say(attach);
    ckm::proto::Message again;
    CK_CHECK(pump(server, second, again));
    const auto* const second_time = std::get_if<ckm::proto::Attached>(&again);
    CK_CHECK(second_time != nullptr);
    if (second_time != nullptr) {
        // Theirs, not the seed — and the two are now provably different.
        CK_CHECK(second_time->snapshot.focused_term == two.id());
        CK_CHECK(second_time->snapshot.focused_term != three.id());
    }
}

CK_TEST(a_snapshot_states_what_a_terminal_has_counted_so_an_arriving_reader_starts_even) {
    // WP-41. The two `TerminalState` serials need a PRODUCER, not a
    // round-trip: `ef8de87` found `Attach.share` declared, encoded, decoded,
    // server-handled and produced by nothing, and a codec catalogue would have
    // passed on the day it shipped unreachable. So this asserts a NON-ZERO
    // count arriving through an actual snapshot from an actual server.
    //
    // What it buys a reader: a client attaching to a session whose terminal
    // has already rung learns that count as ANSWERED, rather than reading the
    // session's whole history as news — which for a long-lived terminal is a
    // mark that is up on arrival and every arrival after.
    const std::filesystem::path socket = private_socket("snap-serials");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    // A child that rings, so the count is genuinely non-zero before anybody
    // attaches — the arriving reader is exactly who this is for.
    ckm::server::Terminal& terminal =
        server.open_terminal(0, spec_running("printf '\\a'; sleep 3600"));
    const ckm::server::TerminalId id = terminal.id();

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    // Let the bell reach the emulator and be counted. Bounded: a real child is
    // involved, so the only honest wait is a wait, and the bound fails the
    // test rather than hanging it.
    for (int i = 0; i < 200 && terminal.bell_serial() == 0; ++i) {
        clock.advance(20'000'000);
        (void)server.step();
        ::usleep(2000);
    }
    const std::uint32_t counted = terminal.bell_serial();
    CK_CHECK(counted > 0);

    // Attach again: the snapshot is re-stated, and it must carry the count.
    ckm::proto::Attach again;
    again.columns = 40;
    again.rows = 8;
    client.say(again);
    ckm::proto::Attached handed;
    CK_CHECK(await_message(server, clock, client, handed));

    const ckm::proto::TerminalState* stated = nullptr;
    for (const ckm::proto::TerminalState& one : handed.snapshot.terminals)
        if (one.term == id) stated = &one;
    CK_CHECK(stated != nullptr);
    if (stated != nullptr) {
        CK_CHECK(stated->bell_serial == counted);
        CK_CHECK(stated->bell_serial > 0);   // not a default arriving by luck
    }
}

CK_TEST(a_second_bell_is_announced_and_not_merely_counted) {
    // The defect found by asking how the serial TRAVELS rather than whether it
    // exists. `mark_bell()` returns early when the level is already up, so a
    // second ring left `marks_announced_` true and produced no `TermMeta` at
    // all: the count incremented on the server and never reached the wire —
    // produced, and unreachable, in precisely the case the serial was added
    // for. `count_bell()` now says the terminal has something new to say.
    //
    // Activity deliberately does NOT do this. It is counted on every tick a
    // terminal writes, so announcing each count is a message per tick for the
    // whole of a build; three suites said so when it was tried. A reader needs
    // "rang again" at full resolution and "wrote since you looked" at none.
    const std::filesystem::path socket = private_socket("second-bell");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));

    // Two rings with a gap, in a terminal nobody is focused on — which is the
    // only case where the level stays up between them.
    ckm::server::Terminal& terminal = server.open_terminal(
        0, spec_running("printf '\\a'; sleep 0.3; printf '\\a'; sleep 3600"));
    (void)terminal;

    // Bounded: real child, real bells. What is asserted is that a meta arrives
    // carrying a count of two — not that two metas arrive, because how the
    // server batches its ticks is its own business.
    std::uint32_t highest = 0;
    for (int i = 0; i < 400 && highest < 2; ++i) {
        clock.advance(20'000'000);
        (void)server.step();
        ckm::proto::Message any;
        while (client.take(any)) {
            if (const auto* meta = std::get_if<ckm::proto::TermMeta>(&any))
                highest = std::max(highest, meta->bell_serial);
        }
        ::usleep(2000);
    }
    // One would mean the first ring travelled and the second did not, which is
    // exactly the shape this test exists to catch.
    CK_CHECK(highest >= 2);
}
