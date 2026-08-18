// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// WP-2's loop, in-process and on a `ManualClock`. The lifecycle cases run a real
// process (test_server_lifecycle.cpp); these are the ones about WHEN the loop
// does things, and a tick that only happens when real time passes cannot be
// tested by waiting for real time — it can only be tested by owning the clock.
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>

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
    ckm::proto::Message delta_message;
    CK_CHECK(client.take(delta_message));
    const auto* delta = std::get_if<ckm::proto::GridDelta>(&delta_message);
    CK_CHECK(delta != nullptr);
    if (delta != nullptr) {
        CK_CHECK(delta->term == terminal.id());
        CK_CHECK(delta->seq == 1U);
        CK_CHECK(!delta->ops.empty());
    }

    // And nothing further while the child stays quiet, however many ticks pass:
    // a tick with no news is not a frame with no content, it is no frame.
    for (int tick = 0; tick < 5; ++tick) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        ckm::proto::Message nothing;
        CK_CHECK(!client.take(nothing));
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
    // terminal on the machine for everybody (plans/02). So the server queues,
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
    // server with no sessions left stops (plans/03).
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
    // plans/03's move-terminal row: membership changes, the PTY does not. The
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

    ckm::proto::NewSession second;
    second.name = "two";
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
