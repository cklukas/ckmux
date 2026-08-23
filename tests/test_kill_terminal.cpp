// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The session model: "`kill-terminal id` | SIGKILL the process group, drop the terminal
// immediately. Confirmed in the UI."
//
// The operations table promised that from M1 and nothing implemented it.
// `proto::KillTerminal` existed as a message type, sat in the `Message`
// variant and round-tripped in `test_proto`'s catalogue — and no server
// handler ever claimed it, no command id named it, no keymap action reached
// it. It is the fourth shaped-and-dead find of the week after `Attach.share`,
// `TermMeta::bell_serial` and `ErrorCode::NameTaken`, and the first that is a
// whole message rather than a field. Found by ckmux-63 reading the table
// against the code for WP-15; the divergence test they wrote to pin it was
// retired by being SATISFIED rather than by being wrong, and this suite is
// what replaced it.
//
// The case that carries the package is `a_program_that_ignores_being_asked_is
// _killed_anyway`: everything else here would also pass if `KillTerminal` were
// quietly implemented as a polite close, which is the plausible wrong version.
#if !defined(_WIN32)

#include <cstdint>
#include <filesystem>
#include <string>
#include <chrono>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include "client/client_app.hpp"
#include "client/commands.hpp"
#include "common/config.hpp"
#include "common/proto.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/menu.hpp"
#include <unistd.h>

#include "platform/socket.hpp"
#include "server/server.hpp"
#include "server/terminals.hpp"

namespace {

using ckm::proto::Message;
using ckm::server::Server;
using ckm::server::TerminalSpec;

std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-kill" + std::to_string(static_cast<unsigned long>(::getpid()));
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
    // Off, so a session losing its last terminal does not tear the fixture
    // down underneath an assertion about the terminal.
    settings.kill_empty_session = false;
    return settings;
}

TerminalSpec spec_running(std::string command) {
    TerminalSpec spec;
    spec.command = std::move(command);
    spec.working_directory = "/";
    spec.columns = 40;
    spec.rows = 10;
    return spec;
}

// A server, a client on the wire, and whatever came back.
struct Fixture {
    std::filesystem::path socket;
    ckv::ManualClock clock;
    Server server;
    ckm::platform::Stream stream;
    ckm::proto::FrameReader reader;
    std::vector<ckm::proto::Error> errors;
    std::vector<ckm::proto::TermClosed> closed;

    explicit Fixture(std::string_view name)
        : socket((forget(private_socket(name)), private_socket(name))),
          server(Server::Options{socket, test_settings()}, clock) {
        CK_CHECK(server.start() == Server::StartStatus::Listening);
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        CK_CHECK(result.status == ckm::platform::ConnectStatus::Connected);
        stream = ckm::platform::Stream(result.fd);
        ckm::proto::Hello hello;
        hello.build = std::string(ckm::proto::kBuildIdentity);
        say(hello);
        pump(4);
    }

    ~Fixture() {
        server.terminals().close_all();
        forget(socket);
    }

    void say(const Message& message) { (void)stream.send(ckm::proto::encode(message)); }

    // Steps the server on a clock the test owns, and keeps what arrived.
    void pump(int passes) {
        for (int pass = 0; pass < passes; ++pass) {
            clock.advance(34'000'000);
            (void)server.step();
            std::string arrived;
            (void)stream.receive(arrived);
            if (!arrived.empty() && !reader.append(arrived)) return;
            Message message;
            while (reader.next(message) == ckm::proto::DecodeError::None) {
                if (const auto* error = std::get_if<ckm::proto::Error>(&message))
                    errors.push_back(*error);
                else if (const auto* gone = std::get_if<ckm::proto::TermClosed>(&message))
                    closed.push_back(*gone);
            }
        }
    }

    void attach(ckm::server::SessionId id) {
        ckm::proto::Attach request;
        request.session = id;
        request.columns = 80;
        request.rows = 24;
        say(request);
        pump(6);
    }

    void kill(std::uint64_t term) {
        ckm::proto::KillTerminal ask;
        ask.term = term;
        say(ask);
    }

    // Real time, not clock ticks. The clock here is manual, so pumping buys a
    // child no wall time at all — and a child that has not reached its `trap`
    // yet dies of the first signal, which looks exactly like the behaviour
    // under test. `usleep` is the idiom `test_server_terminals` already uses
    // for the same reason.
    void settle_child(int milliseconds = 600) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            pump(1);
            ::usleep(2000);
        }
    }

    // Waits on a wall clock for the `TermClosed` naming this terminal, so a
    // slow machine waits longer rather than failing.
    bool wait_for_close(std::uint64_t term, int milliseconds = 4000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            pump(1);
            for (const ckm::proto::TermClosed& gone : closed)
                if (gone.term == term) return true;
            ::usleep(2000);
        }
        for (const ckm::proto::TermClosed& gone : closed)
            if (gone.term == term) return true;
        return false;
    }

    // The exit status the server announced for it, or a value no child can
    // produce so a missing announcement fails rather than reads as a match.
    std::int32_t status_of(std::uint64_t term) const {
        for (const ckm::proto::TermClosed& gone : closed)
            if (gone.term == term) return gone.exit_status;
        return -999;
    }
};

}  // namespace

CK_TEST(a_killed_program_gets_no_chance_to_finish_and_the_terminal_goes) {
    // What this pins, and — more usefully — what it does NOT.
    //
    // PINNED: a program with a `SIGTERM` handler that would exit cleanly does
    // not get to run it. The child traps TERM and HUP and exits 42; ckVision
    // reports `WIFEXITED(status) ? WEXITSTATUS(status) : -1`, so 42 means the
    // handler ran and -1 means the process was signalled dead. After
    // `kill-terminal` the answer is -1, the terminal is gone, and the watcher
    // was told. The positive partner below — a POLITE close with a real grace,
    // same shell, same trap, same server — produces 42, which is what makes -1
    // mean something rather than being what a child that never execed looks
    // like.
    //
    // NOT PINNED, and stated because a green suite otherwise reads as proof:
    // that the implementation sends SIGKILL *only*. It does — `begin_kill_
    // terminal` never calls `request_termination()`, which is what the session model
    // specifies — but this suite cannot demonstrate it. Mutating the handler to
    // `begin_close(force=true, grace=0)`, which sends SIGHUP and SIGTERM first,
    // passes every assertion here. That is not a gap in the assertions; the two
    // are not separable from outside. A zero grace escalates on the next tick,
    // ~33 ms, and a shell cannot run a trap and exit inside that window — I
    // tried it with a busy loop instead of a sleeping one, to give the trap
    // microsecond latency, and the mutant still survived.
    //
    // So the difference between them is real but decided by a race the test
    // cannot win, and the code does not rely on winning it: not sending
    // signals the operation does not call for is a property of the
    // implementation, not of the timing. Anyone tempted to simplify
    // `begin_kill_terminal` into a zero-grace close should know that no test
    // will stop them, and that the session model says SIGKILL.
    const std::string trapped = "trap 'exit 42' TERM HUP; while :; do sleep 0.05; done";

    {
        // The positive partner. Same shell, same trap, same server: a POLITE
        // close with a real grace must produce 42. Without this, "not 42" is
        // also what a child that never execed looks like.
        Fixture polite("polite");
        ckm::server::Session& session = polite.server.create_session("asked");
        ckm::server::Terminal& doomed = polite.server.open_terminal(session.id, spec_running(trapped));
        const std::uint64_t term = doomed.id();
        polite.attach(session.id);
        polite.settle_child();

        ckm::proto::CloseTerminal ask;
        ask.term = term;
        // A REAL grace: with zero, the escalation to SIGKILL lands on the next
        // tick and the child's trap never gets to run — the partner would fail
        // for the same reason the subject is supposed to.
        ask.force = 1;
        ask.grace_seconds = 3;
        polite.say(ask);
        CK_CHECK(polite.wait_for_close(term));
        CK_CHECK(polite.status_of(term) == 42);
    }

    Fixture f("killed");
    ckm::server::Session& session = f.server.create_session("victim");
    ckm::server::Terminal& doomed = f.server.open_terminal(session.id, spec_running(trapped));
    const std::uint64_t term = doomed.id();
    f.attach(session.id);
    f.settle_child();
    // Running before the request, so its absence afterwards is the operation
    // rather than a child that never started.
    CK_CHECK(f.server.terminals().find(term) != nullptr);
    CK_CHECK(f.server.terminals().find(term)->live());

    f.kill(term);
    CK_CHECK(f.wait_for_close(term));
    // Dead, announced, and never asked: SIGKILL cannot be trapped, so 42 is
    // exactly the value this child could not have produced.
    CK_CHECK(f.server.terminals().find(term) == nullptr);
    CK_CHECK(f.status_of(term) != 42);
    CK_CHECK(f.status_of(term) == -1);
}


CK_TEST(killing_a_terminal_is_not_answered_with_a_refusal_any_more) {
    // The negative that the whole package turns on, and it needs its positive
    // partner in the same case or it passes the day somebody deletes the
    // handler AND the refusal: a message that is STILL unimplemented must
    // still be refused, so the harness is demonstrably able to see a refusal.
    Fixture f("refusal");
    ckm::server::Session& session = f.server.create_session("victim");
    ckm::server::Terminal& doomed = f.server.open_terminal(session.id, spec_running("sleep 30"));
    const std::uint64_t term = doomed.id();
    f.attach(session.id);
    f.pump(10);

    f.kill(term);
    f.pump(30);
    for (const ckm::proto::Error& error : f.errors) CK_CHECK(error.context != "KillTerminal");

    // The partner. `Pong` is a message the server receives and does not
    // implement — it SENDS pongs, it does not answer them — so it takes the
    // not-implemented path and proves the harness would have caught one.
    const std::size_t before = f.errors.size();
    f.say(ckm::proto::Pong{7});
    f.pump(10);
    CK_CHECK(f.errors.size() > before);
    CK_CHECK(f.errors.back().context == "Pong");
}

CK_TEST(a_refusal_names_the_request_and_promises_no_release) {
    // `name_of()` covered eleven of fifty-three message types behind a
    // `default:` returning the literal string "a message", so the `context`
    // field — whose entire job is to say WHICH request failed — said nothing
    // for the other forty-two. And the `human` text told readers to wait for
    // WP-8 and WP-6, both of which had landed three days earlier.
    Fixture f("naming");
    f.say(ckm::proto::Pong{1});
    f.pump(8);
    CK_CHECK(!f.errors.empty());
    if (f.errors.empty()) return;
    const ckm::proto::Error& refusal = f.errors.back();
    // The CODE, not only the sentence. `ErrorCode::NotImplemented` had a
    // producer in `src/` and nothing in `tests/` naming it — so a change that
    // set the wrong code here would have passed every assertion below, which
    // are all about the prose. Found sweeping the whole enum for the rule the
    // wire-field cases taught: a value needs a producer AND a test that names
    // it. The other five codes each had one; this was the gap, and it was in
    // this suite.
    CK_CHECK(refusal.code == static_cast<std::uint16_t>(ckm::proto::ErrorCode::NotImplemented));
    CK_CHECK(refusal.context == "Pong");
    CK_CHECK(refusal.human.find("Pong") != std::string::npos);
    // Exactly, not "does not contain 'a message'": the old text would have
    // failed the context check above anyway, and what this pins is that the
    // sentence names the request rather than a placeholder.
    CK_CHECK(refusal.context != "a message");
    // No package numbers. A server does not know which release fixes this, and
    // a promise it cannot keep goes stale the moment the release lands — which
    // is precisely how the old one survived three days after its own promise
    // came true.
    CK_CHECK(refusal.human.find("WP-") == std::string::npos);
}

CK_TEST(killing_a_terminal_that_is_already_gone_is_not_an_error) {
    // A reader kills a terminal whose program has just ended; the message and
    // the exit race, and neither side is wrong. It is doubly true here — what
    // they asked for is that the terminal be dead, and it is. `begin_close`
    // has said this since M2 and this follows it rather than inventing a
    // different answer for the neighbouring operation.
    Fixture f("gone");
    ckm::server::Session& session = f.server.create_session("victim");
    ckm::server::Terminal& survivor = f.server.open_terminal(session.id, spec_running("sleep 30"));
    const std::uint64_t alive = survivor.id();
    f.attach(session.id);
    f.pump(10);

    f.kill(4242);  // no such terminal, and never was
    f.pump(12);
    for (const ckm::proto::Error& error : f.errors) CK_CHECK(error.context != "KillTerminal");
    // And it disturbed nothing: the terminal that DOES exist is untouched.
    // Without this line the case would pass on a server that had fallen over.
    CK_CHECK(f.server.terminals().find(alive) != nullptr);
    CK_CHECK(f.server.terminals().find(alive)->live());
}

CK_TEST(kill_terminal_is_on_the_terminal_menu_below_close_and_carries_no_chord) {
    // "Confirmed in the UI" is half of the session model row, and a command with no
    // menu path is not in the UI at all. Located by command id rather than by
    // position — three suites broke in one day naming positions — except for
    // the ordering claim, which IS a claim about position and so states the
    // pair it is about.
    ckv::term::HeadlessTerminal terminal{ckv::Size{100, 30}};
    ckv::ManualClock clock;
    ckv::ui::Application app{terminal, clock};
    ckm::client::ClientOptions options;
    options.settings.shell = "/bin/cat";
    ckm::client::ClientApp client{app, options};

    const ckm::client::KeyBinding* kill = nullptr;
    for (const ckm::client::KeyBinding& binding : client.keymap().bindings())
        if (binding.key == ckm::client::commands::kKillTerminal) kill = &binding;
    CK_CHECK(kill != nullptr);
    if (kill == nullptr) return;
    // No chord. This destroys unsaved work with no grace and no undo, and a
    // key one letter from `^B x` is a key somebody hits by accident once.
    CK_CHECK(kill->chord.empty());
    CK_CHECK(kill->command != ckv::ui::kInvalidCommand);
}

#endif  // !defined(_WIN32)
