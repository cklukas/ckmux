// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-38 at the wire (the work queue): `WatchStats` in, `TermStats` out, and the two
// properties the package stands on — stats flow only to a client that asked,
// and a server nobody asked does no sampling work at all. In-process `Server`
// on a `ManualClock` over a real socket, with real PTY children where a child
// is the claim: a spinner must read busy and an idle shell must read idle, and
// neither of those can be faked through the seam.
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

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
    directory /= "ckmux-st" + std::to_string(static_cast<unsigned long>(::getpid()));
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
    bool take(ckm::proto::Message& message) {
        std::string arrived;
        (void)stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) return false;
        return reader.next(message) == ckm::proto::DecodeError::None;
    }
};

bool pump(Server& server, WireClient& client, ckm::proto::Message& message, int passes = 8) {
    for (int pass = 0; pass < passes; ++pass) {
        if (!server.step()) return false;
        if (client.take(message)) return true;
    }
    return false;
}

// Steps the server `passes` times, collecting every `TermStats` that arrives
// and dropping everything else — a live attachment also carries TermOpened,
// layout and grid traffic, and this suite's claims are about stats alone.
int collect_stats(Server& server, WireClient& client, std::vector<ckm::proto::TermStats>& out,
                  int passes = 6) {
    int found = 0;
    for (int pass = 0; pass < passes; ++pass) {
        if (!server.step()) return found;
        ckm::proto::Message message;
        while (client.take(message)) {
            if (const auto* stats = std::get_if<ckm::proto::TermStats>(&message)) {
                out.push_back(*stats);
                ++found;
            }
        }
    }
    return found;
}

bool greet_and_attach(Server& server, WireClient& client) {
    ckm::proto::Hello hello;
    hello.build = "stats tests";
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

bool alive(const ckm::proto::TermStats& stats) {
    return (stats.flags & static_cast<std::uint8_t>(ckm::proto::TermStatsFlag::Alive)) != 0;
}

}  // namespace

CK_TEST(stats_flow_only_to_a_client_that_asked_and_the_sampler_is_idle_until_then) {
    const std::filesystem::path socket = private_socket("ask");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));

    // Three virtual seconds with nobody subscribed: no TermStats arrives, and
    // — the stronger claim — the sampler never ran at all. Messages could be
    // absent for a dozen reasons; a pass counter stuck at zero has one.
    std::vector<ckm::proto::TermStats> stats;
    for (int second = 0; second < 3; ++second) {
        clock.advance(1'000'000'000);
        CK_CHECK(collect_stats(server, client, stats, 3) == 0);
    }
    CK_CHECK(stats.empty());
    CK_CHECK(server.stats_passes() == 0U);

    // Ask, and the first report arrives within a sample period — memory
    // immediately, CPU zero because a rate needs an interval to differ over.
    client.say(ckm::proto::WatchStats{1});
    (void)collect_stats(server, client, stats, 8);
    CK_CHECK(!stats.empty());
    CK_CHECK(server.stats_passes() >= 1U);
    CK_CHECK(stats.front().term == terminal.id());
    CK_CHECK(alive(stats.front()));
    CK_CHECK(stats.front().rss_bytes > 0U);
    CK_CHECK(stats.front().cpu_permille == 0U);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(unsubscribing_stops_the_flow_while_the_connection_lives_on) {
    const std::filesystem::path socket = private_socket("off");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));
    (void)server.open_terminal(0, spec_running("sleep 30"));

    std::vector<ckm::proto::TermStats> stats;
    client.say(ckm::proto::WatchStats{1});
    (void)collect_stats(server, client, stats, 8);
    CK_CHECK(!stats.empty());

    client.say(ckm::proto::WatchStats{0});
    // A couple of passes for the unsubscribe to be read, then the claim: the
    // flow stops AND the sampler goes idle, over several virtual seconds.
    (void)collect_stats(server, client, stats, 4);
    const std::size_t passes_at_unsubscribe = server.stats_passes();
    std::vector<ckm::proto::TermStats> after;
    for (int second = 0; second < 3; ++second) {
        clock.advance(1'000'000'000);
        (void)collect_stats(server, client, after, 3);
    }
    CK_CHECK(after.empty());
    CK_CHECK(server.stats_passes() == passes_at_unsubscribe);

    // The connection itself is untouched: the same client still gets a Pong.
    ckm::proto::Ping ping;
    ping.nonce = 77;
    client.say(ping);
    ckm::proto::Message answer;
    bool ponged = false;
    for (int pass = 0; pass < 8 && !ponged; ++pass) {
        if (!pump(server, client, answer)) break;
        if (const auto* pong = std::get_if<ckm::proto::Pong>(&answer))
            ponged = pong->nonce == 77;
    }
    CK_CHECK(ponged);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(the_cadence_is_one_second_on_the_injected_clock) {
    const std::filesystem::path socket = private_socket("cadence");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));
    (void)server.open_terminal(0, spec_running("sleep 30"));

    std::vector<ckm::proto::TermStats> stats;
    client.say(ckm::proto::WatchStats{1});
    (void)collect_stats(server, client, stats, 8);
    CK_CHECK(stats.size() == 1U);  // the subscription's own first report

    // Ten quarter-second steps are 2.5 virtual seconds: the sampler owes
    // exactly two more passes (at 1.0 s and 2.0 s), not ten and not zero —
    // the cadence is the clock's, not the loop's.
    stats.clear();
    for (int quarter = 0; quarter < 10; ++quarter) {
        clock.advance(250'000'000);
        (void)collect_stats(server, client, stats, 2);
    }
    CK_CHECK(stats.size() == 2U);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_spinning_child_reads_busy_and_an_idle_shell_reads_idle) {
    const std::filesystem::path socket = private_socket("busy");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));
    ckm::server::Terminal& spinner =
        server.open_terminal(0, spec_running("while :; do :; done"));
    ckm::server::Terminal& idle = server.open_terminal(0, spec_running("sleep 30"));

    std::vector<ckm::proto::TermStats> stats;
    client.say(ckm::proto::WatchStats{1});
    (void)collect_stats(server, client, stats, 8);  // baselines established

    // The one place this suite spends real time, because CPU accumulation is
    // real however virtual the clock: give the spinner 400 ms of wall time,
    // advance the clock by the same 400 ms plus the sampler's second, and the
    // rate comes out in real permille of one core.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    clock.advance(1'000'000'000);
    stats.clear();
    (void)collect_stats(server, client, stats, 6);

    std::uint32_t spinner_permille = 0;
    std::uint32_t idle_permille = 0;
    bool saw_spinner = false;
    bool saw_idle = false;
    for (const ckm::proto::TermStats& entry : stats) {
        if (entry.term == spinner.id()) {
            spinner_permille = entry.cpu_permille;
            saw_spinner = true;
        }
        if (entry.term == idle.id()) {
            idle_permille = entry.cpu_permille;
            saw_idle = true;
        }
    }
    CK_CHECK(saw_spinner);
    CK_CHECK(saw_idle);
    // 400 ms of real spinning against 1 s of virtual interval is ~400‰ on an
    // unloaded machine; 100‰ is the floor a loaded CI still clears, and the
    // ceiling catches a rate computed against the wrong clock or unit.
    CK_CHECK(spinner_permille > 100U);
    CK_CHECK(spinner_permille < 16'000U);
    CK_CHECK(idle_permille < 100U);
    std::printf("  [rate] spinner %u permille, idle shell %u permille\n",
                spinner_permille, idle_permille);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_dead_child_is_announced_once_and_then_never_again) {
    const std::filesystem::path socket = private_socket("dead");
    forget(socket);
    ckm::Settings settings = test_settings();
    // Hold the window on exit, so the terminal STAYS while its child is gone —
    // the case the not-alive announcement exists for. Without hold the
    // terminal is removed and TermClosed already tells the whole story.
    settings.on_exit = ckm::ExitPolicy::Hold;
    ckv::ManualClock clock;
    Server server(Server::Options{socket, settings}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    CK_CHECK(greet_and_attach(server, client));
    ckm::server::Terminal& doomed = server.open_terminal(0, spec_running("exit 0"));

    std::vector<ckm::proto::TermStats> stats;
    client.say(ckm::proto::WatchStats{1});

    // Step with real waits until the exit has been observed and a not-alive
    // stats message arrived. The deadline guards the suite; reaching it fails.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool announced = false;
    while (!announced && std::chrono::steady_clock::now() < deadline) {
        clock.advance(1'000'000'000);
        (void)collect_stats(server, client, stats, 3);
        for (const ckm::proto::TermStats& entry : stats)
            if (entry.term == doomed.id() && !alive(entry)) announced = true;
    }
    CK_CHECK(announced);

    // Once. Three more sampled seconds bring no further word about it — a
    // reader's cleared readout must not be re-cleared every second forever.
    stats.clear();
    for (int second = 0; second < 3; ++second) {
        clock.advance(1'000'000'000);
        (void)collect_stats(server, client, stats, 3);
    }
    for (const ckm::proto::TermStats& entry : stats) CK_CHECK(entry.term != doomed.id());

    server.terminals().close_all();
    forget(socket);
}
