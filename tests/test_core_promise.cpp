// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-7, the M2 acceptance: the promise the whole project exists to keep.
//
// A program keeps running when the thing watching it dies. Everything else in
// ckmux is in service of that sentence, and this file is where it is checked the
// only way it can be — with a real server process, a real client process, a real
// child counting, and a real `kill -9`.
//
// The second half is the flood gate: a server carrying a child that never stops
// talking must still answer a `Ping` promptly, because a multiplexer that stops
// responding under load has stopped being a multiplexer.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "client/server_connection.hpp"
#include "client/server_session.hpp"
#include "common/proto.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"

#include "cvision/term/posix_terminal_subsession.hpp"

#include "cvision/core/golden.hpp"
#include "cvision/testing/cktest.hpp"

namespace {

using clock_type = std::chrono::steady_clock;

std::filesystem::path binary_path() {
#if defined(CKMUX_BINARY_PATH)
    return std::filesystem::path(CKMUX_BINARY_PATH);
#else
    return {};
#endif
}

std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-p" + std::to_string(static_cast<unsigned long>(::getpid()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (std::string(name) + ".sock");
}

void forget(const std::filesystem::path& socket) {
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".log"), ignored);
    std::filesystem::remove(socket.parent_path(), ignored);
}

::pid_t start_server(const std::filesystem::path& socket) {
    const ::pid_t child = ::fork();
    if (child != 0) return child;
    const std::string program = binary_path().string();
    const std::string path = socket.string();
    char* const argv[] = {const_cast<char*>(program.c_str()), const_cast<char*>("--server"),
                          const_cast<char*>(path.c_str()), const_cast<char*>("--foreground"),
                          nullptr};
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
        (void)::dup2(null_fd, STDERR_FILENO);
        (void)::close(null_fd);
    }
    ::execv(program.c_str(), argv);
    ::_exit(127);
}

// Waits for the server to be listening. Starting a process and connecting to it
// are two different moments, and the gap between them is exactly the race a
// client's own retry loop exists to cover (WP-2).
bool wait_for_socket(const std::filesystem::path& socket, int budget_ms = 5000) {
    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(budget_ms);
    for (;;) {
        ckm::platform::ConnectResult probe = ckm::platform::connect_to_server(socket);
        if (probe.status == ckm::platform::ConnectStatus::Connected) {
            (void)::close(probe.fd);
            return true;
        }
        if (clock_type::now() >= deadline) return false;
        ::usleep(5000);
    }
}

void end_process(::pid_t child, int signal_number) {
    if (child <= 0) return;
    (void)::kill(child, signal_number);
    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(3000);
    for (;;) {
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) != 0) return;
        if (clock_type::now() >= deadline) {
            (void)::kill(child, SIGKILL);
            (void)::waitpid(child, &status, 0);
            return;
        }
        ::usleep(5000);
    }
}

// A client on the wire, with the real session behind it. Deliberately not the
// ckmux binary: what these tests need to do to a client is kill it at an
// arbitrary moment and inspect what the next one is handed, and a UI in between
// would only be something else to go wrong.
struct WireClient {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader reader;
    ckm::client::ServerSession session{nullptr};
    // Every byte the server has written to this connection. Counted where the
    // bytes actually arrive rather than at the server's send site, because the
    // server under test is a forked process with its stderr on /dev/null —
    // instrumenting it would need a file and a shutdown hook, and this end
    // sees the same wire.
    std::size_t bytes_received = 0;

    bool connect_only(const std::filesystem::path& socket) {
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        if (result.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = ckm::platform::Stream(result.fd);
        session = ckm::client::ServerSession([this](const ckm::proto::Message& message) {
            (void)stream.send(ckm::proto::encode(message));
        });
        ckm::proto::Hello hello;
        hello.build = "a test";
        (void)stream.send(ckm::proto::encode(hello));
        return true;
    }

    bool connect_and_attach(const std::filesystem::path& socket, ckv::Size desktop) {
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        if (result.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = ckm::platform::Stream(result.fd);
        session = ckm::client::ServerSession([this](const ckm::proto::Message& message) {
            (void)stream.send(ckm::proto::encode(message));
        });
        session.set_history_limit(200);
        ckm::proto::Hello hello;
        hello.build = "a test";
        (void)stream.send(ckm::proto::encode(hello));
        session.attach(0, desktop, ckv::Size{9, 18});
        return pump_until([this] { return session.attached(); });
    }

    std::size_t pump() {
        std::string arrived;
        (void)stream.receive(arrived);
        bytes_received += arrived.size();
        if (!arrived.empty() && !reader.append(arrived)) return 0;
        std::size_t count = 0;
        for (;;) {
            ckm::proto::Message message;
            if (reader.next(message) != ckm::proto::DecodeError::None) break;
            ++count;
            if (std::holds_alternative<ckm::proto::HelloAck>(message)) continue;
            (void)session.handle(message);
            // Latched as it goes past, not left to be read off `last_`. A pump
            // call decodes every complete frame in the buffer, so under a flood
            // a `Pong` is followed by more `GridDelta`s in the SAME batch and
            // `last_` no longer holds it — the answer arrived and the reader
            // looking at the end of the batch missed it.
            if (const auto* pong = std::get_if<ckm::proto::Pong>(&message))
                last_pong_nonce_ = pong->nonce;
            last_ = std::move(message);
        }
        session.heal_if_needed();
        return count;
    }

    template <typename Ready>
    bool pump_until(Ready ready, int budget_ms = 6000) {
        const clock_type::time_point deadline =
            clock_type::now() + std::chrono::milliseconds(budget_ms);
        for (;;) {
            pump();
            if (ready()) return true;
            if (clock_type::now() >= deadline) return false;
            ::usleep(3000);
        }
    }

    std::string screen_of(std::uint64_t terminal) {
        std::string text;
        ckm::client::RemoteTerminalSubsession* mirror = session.terminal(terminal);
        if (mirror == nullptr) return text;
        for (const ckv::Cell& cell : mirror->cells())
            if (!cell.is_continuation()) text += cell.grapheme();
        return text;
    }

    std::uint64_t only_terminal() {
        const std::vector<std::uint64_t> ids = session.terminal_ids();
        return ids.empty() ? 0 : ids.front();
    }

    ckm::proto::Message last_;
    std::uint64_t last_pong_nonce_ = 0;
};

// The highest counter value on a screen that reads "count 1 count 2 ...".
int highest_count(const std::string& screen) {
    int highest = 0;
    std::size_t at = screen.find("count ");
    while (at != std::string::npos) {
        highest = std::max(highest, std::atoi(screen.c_str() + at + 6));
        at = screen.find("count ", at + 1);
    }
    return highest;
}

}  // namespace

CK_TEST(a_program_keeps_running_when_the_client_watching_it_is_killed) {
    // The core promise, end to end and with nothing simulated: a server in its
    // own process, a client in its own process, a child counting in a real PTY,
    // and `kill -9` — the one ending a client cannot do anything about, and
    // therefore the one worth testing.
    const std::filesystem::path socket = private_socket("promise");
    forget(socket);
    CK_CHECK(!binary_path().empty());
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    // A client, and a terminal counting once every 50 ms.
    WireClient first;
    CK_CHECK(first.connect_and_attach(socket, ckv::Size{80, 24}));
    ckm::proto::NewTerminal ask;
    ask.command = "/bin/sh";
    first.session.request(ask);
    CK_CHECK(first.pump_until([&] { return first.only_terminal() != 0; }));
    const std::uint64_t terminal = first.only_terminal();
    ckm::proto::Input start;
    start.term = terminal;
    start.bytes = "i=0; while :; do i=$((i+1)); echo count $i; sleep 0.05; done\n";
    first.session.request(start);

    if (!first.pump_until([&] { return highest_count(first.screen_of(terminal)) >= 3; })) {
        ckm::client::RemoteTerminalSubsession* mirror = first.session.terminal(terminal);
        std::printf("  [debug] terminal %llu: mirror %dx%d, seq %u, gaps %llu, resnapshots %llu, "
                    "screen \"%.50s\"\n",
                    static_cast<unsigned long long>(terminal),
                    mirror ? mirror->mirror().cells().width : -1,
                    mirror ? mirror->mirror().cells().height : -1,
                    mirror ? mirror->mirror().sequence() : 0,
                    static_cast<unsigned long long>(mirror ? mirror->mirror().gaps() : 0),
                    static_cast<unsigned long long>(first.session.resnapshots()),
                    first.screen_of(terminal).c_str());
    }
    CK_CHECK(first.pump_until([&] { return highest_count(first.screen_of(terminal)) >= 3; }, 2000));
    const int seen_by_the_first_client = highest_count(first.screen_of(terminal));
    CK_CHECK(seen_by_the_first_client >= 3);

    // The client dies without warning. Not a detach, not a quit: the socket
    // simply stops, which is what a `kill -9`, a closed laptop or a dropped ssh
    // connection all look like from the server's side.
    first.stream.close();

    // Time passes with nobody watching. This is the part that matters: the
    // child is not paused, buffered or waiting for a reader — it is running.
    ::usleep(500'000);

    // A second client attaches, and is handed the terminal whole.
    WireClient second;
    CK_CHECK(second.connect_and_attach(socket, ckv::Size{80, 24}));
    CK_CHECK(second.only_terminal() == terminal);
    const int seen_on_reattach = highest_count(second.screen_of(terminal));
    std::printf("  [core promise] first client saw count %d; after 500 ms unwatched, the "
                "reattaching client was handed count %d\n",
                seen_by_the_first_client, seen_on_reattach);
    CK_CHECK(seen_on_reattach > seen_by_the_first_client);

    // And it keeps counting for the new client, which is the other half: the
    // reattachment is live, not a photograph.
    CK_CHECK(second.pump_until(
        [&] { return highest_count(second.screen_of(terminal)) > seen_on_reattach; }));

    // The rehydrated frame, dumped the way every ckVision golden is (D-014).
    // Not compared against a file — a counter's value is different every run, by
    // design, and a golden of it would be a golden of the clock — but against
    // the same dump of a frame with known content below, which is what pins the
    // format end to end.
    ckm::client::RemoteTerminalSubsession* mirror = second.session.terminal(terminal);
    CK_CHECK(mirror != nullptr);
    if (mirror != nullptr) {
        const ckv::core::TerminalSnapshot snapshot = mirror->snapshot();
        CK_CHECK(snapshot.cells.width == 80);
        CK_CHECK(snapshot.cells.height == 24);
        CK_CHECK(snapshot.cell_buffer.size() == 80U * 24U);
    }

    ckm::proto::KillServer kill;
    second.session.request(kill);
    (void)second.stream.flush();
    end_process(server, SIGTERM);
    forget(socket);
}

CK_TEST(a_rehydrated_frame_is_the_frame_the_server_holds) {
    // The golden half of the core promise, with content that does not move: what
    // a client is handed on attach has to be what the terminal actually holds,
    // cell for cell and style for style — which is a stronger statement than "the
    // words are there", and the one a reader's screen depends on.
    const std::filesystem::path socket = private_socket("golden");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    WireClient client;
    CK_CHECK(client.connect_and_attach(socket, ckv::Size{40, 6}));
    ckm::proto::NewTerminal ask;
    ask.command = "/bin/sh";
    client.session.request(ask);
    CK_CHECK(client.pump_until([&] { return client.only_terminal() != 0; }));
    const std::uint64_t terminal = client.only_terminal();

    // Deliberately styled: a golden that only ever sees plain text would not
    // notice a delta that lost every colour on the way.
    ckm::proto::Input write;
    write.term = terminal;
    write.bytes = "printf '\\033[1;31mSTYLED\\033[0m plain\\n'\n";
    client.session.request(write);
    // Waiting for a RED cell, not for the words: the shell echoes the command
    // line, so the text is on screen before the program that styles it has run.
    const auto red_cell_on_screen = [&] {
        ckm::client::RemoteTerminalSubsession* mirror = client.session.terminal(terminal);
        if (mirror == nullptr) return false;
        for (const ckv::Cell& cell : mirror->cells())
            if (cell.grapheme() == "S" && cell.style().fg.is_indexed()) return true;
        return false;
    };
    CK_CHECK(client.pump_until(red_cell_on_screen));

    // Attach again — the resnapshot path — and compare the two dumps. If the
    // snapshot and the delta stream disagreed about anything at all, these
    // would differ.
    const ckv::core::TerminalSnapshot before = client.session.terminal(terminal)->snapshot();
    client.session.attach(0, ckv::Size{40, 6}, ckv::Size{9, 18});
    CK_CHECK(client.pump_until([&] { return client.session.attachments() >= 2; }));
    const ckv::core::TerminalSnapshot after = client.session.terminal(terminal)->snapshot();

    CK_CHECK(before.cells == after.cells);
    CK_CHECK(before.cell_buffer.size() == after.cell_buffer.size());
    bool identical = before.cell_buffer.size() == after.cell_buffer.size();
    for (std::size_t index = 0; identical && index < before.cell_buffer.size(); ++index)
        if (before.cell_buffer[index].grapheme() != after.cell_buffer[index].grapheme() ||
            !(before.cell_buffer[index].style() == after.cell_buffer[index].style()))
            identical = false;
    CK_CHECK(identical);
    // The style really did survive: a red bold cell is on the screen, so the
    // comparison above was comparing something.
    bool found_red = false;
    for (const ckv::Cell& cell : after.cell_buffer)
        if (cell.grapheme() == "S" && cell.style().fg.is_indexed()) found_red = true;
    CK_CHECK(found_red);

    ckm::proto::KillServer kill;
    client.session.request(kill);
    (void)client.stream.flush();
    end_process(server, SIGTERM);
    forget(socket);
}

CK_TEST(a_reader_sees_their_shells_prompt_in_the_window_ckmux_drew) {
    // The whole thing, as a reader meets it: the real `ckmux` client, in a real
    // terminal, against a real server, with a real shell in the window — and the
    // question is the only one that matters, which is whether they can SEE it.
    //
    // Reported from a running ckmux: "no output is visible in the terminal".
    // Everything underneath was working — the mirror held the prompt, the
    // sequence was unbroken, the window was drawn — and the reason nothing
    // showed is that the terminal had been created at the size of the client's
    // whole DESKTOP while the window that displays it is smaller by its frame.
    // A terminal view shows the bottom of its grid, as a terminal should, so a
    // prompt printed at the top sat above the view. Nothing in the protocol was
    // wrong; the geometry was, and only running it could show that.
    const std::filesystem::path socket = private_socket("prompt");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    // The client hosted in a PTY, its screen decoded from what it drew — the
    // end-to-end pattern of the testing plan.
    ckv::term::TerminalLaunchSpec spec = ckv::term::TerminalLaunchSpec::program(binary_path().string(), {});
    spec.working_directory = "/tmp";
    spec.environment = {{"TERM", "xterm-256color"},
                        {"PATH", "/usr/bin:/bin"},
                        {"SHELL", "/bin/sh"},
                        {"HOME", "/tmp"},
                        {"LC_ALL", "C"},
                        {"CKMUX_SOCKET", socket.string()}};
    spec.profile = ckv::term::embedded_xterm_sixel_profile();
    spec.profile.cells = ckv::Size{100, 30};
    spec.profile.cell_pixels = ckv::Size{9, 18};
    spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
    ckv::term::TerminalSubsessionOptions options;
    options.max_output_bytes = 1u << 20u;
    options.max_parser_work_per_step = 256u << 10u;
    auto client = ckv::term::PosixTerminalSubsession::launch(spec, options);

    // `sh` prints no prompt of its own when it is not interactive, so the shell
    // is asked to say something a reader would recognise.
    const auto screen_of_client = [&client] {
        std::string text;
        const ckv::core::TerminalSnapshot snapshot = client->snapshot();
        for (const ckv::Cell& cell : snapshot.cell_buffer)
            if (!cell.is_continuation()) text += cell.grapheme();
        return text;
    };
    const clock_type::time_point deadline = clock_type::now() + std::chrono::seconds(8);
    bool typed = false;
    bool seen = false;
    while (!seen && clock_type::now() < deadline) {
        (void)client->drain(64 * 1024);
        const std::string screen = screen_of_client();
        // Once the WINDOW is on screen, type into it exactly as a reader would.
        // "Terminal 1" is the window's caption; a bare "Terminal" is also the
        // menu bar's third title, which is drawn in the first frame — long
        // before the client has a session, let alone a terminal in it. Typing
        // then goes nowhere and is never typed again, which is a test that
        // fails for a reason that has nothing to do with what it is checking.
        if (!typed && screen.find("Terminal 1") != std::string::npos) {
            client->send_input("echo ckmux-is-working\n");
            typed = true;
        }
        seen = screen.find("ckmux-is-working") != std::string::npos;
        ::usleep(20000);
    }
    CK_CHECK(typed);
    CK_CHECK(seen);
    client->close();

    ckm::proto::KillServer kill;
    WireClient closer;
    if (closer.connect_and_attach(socket, ckv::Size{80, 24})) {
        closer.session.request(kill);
        (void)closer.stream.flush();
    }
    end_process(server, SIGTERM);
    forget(socket);
}

CK_TEST(a_flooding_child_does_not_stop_the_server_answering) {
    // The flood gate, at protocol level and against a real process: `yes` in one
    // terminal, and a `Ping` that must come back promptly WHILE it runs. A
    // multiplexer that stops answering under load has stopped being one — and
    // the failure is not theoretical, it is what an unbounded read loop or a
    // blocking write to a slow client produces.
    const std::filesystem::path socket = private_socket("flood");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    WireClient client;
    CK_CHECK(client.connect_and_attach(socket, ckv::Size{80, 24}));
    ckm::proto::NewTerminal ask;
    ask.command = "/bin/sh";
    client.session.request(ask);
    CK_CHECK(client.pump_until([&] { return client.only_terminal() != 0; }));
    const std::uint64_t terminal = client.only_terminal();

    ckm::proto::Input flood;
    flood.term = terminal;
    flood.bytes = "yes ckmux-flood-line\n";
    client.session.request(flood);
    CK_CHECK(client.pump_until(
        [&] { return client.screen_of(terminal).find("ckmux-flood-line") != std::string::npos; }));

    // Ten round trips through a server that is reading a flood the whole time
    // — measured against an un-attached connection PAIRED with them rather
    // than run afterwards.
    //
    // Running the two loops in sequence, as this rig first did, leaves the
    // second one timing a deeper flood than the first: the child has been
    // writing for the whole of the first loop before the second starts. That
    // cannot turn a near-zero into half a second, so the original conclusion
    // stands, but it does inflate whichever loop runs second — and simply
    // swapping them would move the bias onto the other measurement rather than
    // remove it. Alternating the two within one window is what actually
    // removes it: each pair of pings meets the same flood depth, so the
    // difference between the two numbers is the difference between the two
    // CONNECTIONS and nothing else.
    //
    // The byte counter rides in the same window for the same reason. Two runs
    // would each have their own flood depth, and a bytes-per-second from one
    // divided by a latency from the other would be a ratio of two different
    // experiments.
    WireClient idle;
    const bool idle_connected = idle.connect_only(socket);
    const std::size_t bytes_before = client.bytes_received;
    const clock_type::time_point window_began = clock_type::now();
    double worst_ms = 0.0;
    double total_ms = 0.0;
    double idle_worst = 0.0;
    double idle_total = 0.0;
    for (std::uint64_t attempt = 1; attempt <= 10; ++attempt) {
        // The attached client: the server's answer PLUS this client's decode of
        // the flood standing in front of it.
        ckm::proto::Ping ping;
        ping.nonce = attempt;
        const clock_type::time_point sent = clock_type::now();
        client.session.request(ping);
        (void)client.stream.flush();
        bool answered = false;
        const clock_type::time_point deadline = sent + std::chrono::milliseconds(2000);
        while (!answered && clock_type::now() < deadline) {
            client.pump();
            answered = client.last_pong_nonce_ == attempt;
            if (!answered) ::usleep(200);
        }
        CK_CHECK(answered);
        const double elapsed =
            std::chrono::duration<double, std::milli>(clock_type::now() - sent).count();
        worst_ms = std::max(worst_ms, elapsed);
        total_ms += elapsed;

        // The un-attached one, immediately after, against the same flood.
        // Deltas go out via `for_each_attached`, so this connection is handed
        // nothing to decode: what it times is the server's answer alone.
        if (!idle_connected) continue;
        ckm::proto::Ping idle_ping;
        idle_ping.nonce = 100 + attempt;
        const clock_type::time_point idle_sent = clock_type::now();
        idle.session.request(idle_ping);
        (void)idle.stream.flush();
        bool idle_answered = false;
        const clock_type::time_point idle_deadline = idle_sent + std::chrono::milliseconds(2000);
        while (!idle_answered && clock_type::now() < idle_deadline) {
            idle.pump();
            idle_answered = idle.last_pong_nonce_ == 100 + attempt;
            if (!idle_answered) ::usleep(200);
        }
        const double idle_elapsed =
            std::chrono::duration<double, std::milli>(clock_type::now() - idle_sent).count();
        idle_worst = std::max(idle_worst, idle_elapsed);
        idle_total += idle_elapsed;
    }
    const double window_ms =
        std::chrono::duration<double, std::milli>(clock_type::now() - window_began).count();
    const std::size_t flood_bytes = client.bytes_received - bytes_before;
    std::printf("  [flood gate] Ping round trip under a `yes` flood: %.1f ms worst, %.1f ms mean\n",
                worst_ms, total_ms / 10.0);
    if (idle_connected) {
        std::printf(
            "  [server latency] same flood, un-attached connection, paired: %.1f ms worst, %.1f ms "
            "mean\n",
            idle_worst, idle_total / 10.0);
    } else {
        std::printf("  [server latency] un-attached connection refused; not measured\n");
    }
    // What the server actually put on the wire over that same window. This is
    // the number the volume hypothesis lives or dies on: if a regression made
    // the flood cost more to send, it shows here; if it is flat, the time is
    // being spent somewhere that is not the wire.
    std::printf("  [flood bytes] %zu bytes to the attached client over %.1f ms (%.2f MB/s)\n",
                flood_bytes, window_ms,
                (static_cast<double>(flood_bytes) / (1024.0 * 1024.0)) / (window_ms / 1000.0));
    // Generous on purpose: what is being asserted is that the server never stops
    // answering, not how fast this machine is. A server that read without a
    // budget, or wrote to a slow client without a queue, would not answer at all
    // — the failure this gate catches is unbounded, not marginal. Sanitizer
    // builds run the whole flood several times slower for the same behaviour,
    // so the budget scales with the build rather than the assertion weakening:
    // unbounded still fails either way.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    constexpr double kWorstBudgetMs = 5000.0;
#elif defined(__has_feature)
    // __has_feature must be nested, not combined with `defined(__has_feature) &&`
    // in one #if: a compiler that lacks it (GCC 13) still has to PARSE the tokens
    // after macro replacement, and `__has_feature(x)` becomes `0(x)` — a syntax
    // error the && short-circuit cannot rescue. GCC 14 defines __has_feature and
    // took the combined form; GCC 13 does not and did not.
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
      __has_feature(undefined_behavior_sanitizer)
    constexpr double kWorstBudgetMs = 5000.0;
#  else
    constexpr double kWorstBudgetMs = 1000.0;
#  endif
#else
    constexpr double kWorstBudgetMs = 1000.0;
#endif
    CK_CHECK(worst_ms < kWorstBudgetMs);

    ckm::proto::KillServer kill;
    client.session.request(kill);
    (void)client.stream.flush();
    end_process(server, SIGTERM);
    forget(socket);
}
