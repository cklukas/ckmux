// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/client_app.hpp"
#include "common/stats.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"
#include "scratch_directory.hpp"

#include <chrono>
#include <string>
#include <vector>

#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
using Stats = ckm::proto::TermStats;
struct Reader {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader frames;
    bool connect(const std::filesystem::path& endpoint) {
        auto connected = ckm::platform::connect_to_server(endpoint);
        if (connected.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = connected.take_stream();
        return true;
    }
    void say(const ckm::proto::Message& message) {
        const auto bytes = ckm::proto::encode(message);
        (void)stream.send(bytes);
    }
    bool take(ckm::proto::Message& message) {
        std::string received;
        (void)stream.receive(received);
        if (!received.empty()) CK_CHECK(frames.append(received));
        return frames.next(message) == ckm::proto::DecodeError::None;
    }
    void stats(std::vector<Stats>& output) {
        ckm::proto::Message message;
        while (take(message)) if (const auto* value = std::get_if<Stats>(&message)) output.push_back(*value);
    }
};

bool attach(ckm::server::Server& server, ckv::ManualClock& clock, Reader& reader) {
    reader.say(ckm::proto::Hello{});
    bool greeted = false;
    for (int pass = 0; pass < 100 && !greeted; ++pass) {
        clock.advance(40'000'000);
        if (!server.step()) return false;
        ckm::proto::Message message;
        while (reader.take(message)) if (std::holds_alternative<ckm::proto::HelloAck>(message)) greeted = true;
        ::Sleep(1);
    }
    if (!greeted) return false;
    ckm::proto::Attach request;
    request.mode = static_cast<std::uint8_t>(ckm::proto::AttachMode::Join);
    request.columns = 80;
    request.rows = 20;
    reader.say(request);
    for (int pass = 0; pass < 100; ++pass) {
        clock.advance(40'000'000);
        if (!server.step()) return false;
        ckm::proto::Message message;
        while (reader.take(message)) if (std::holds_alternative<ckm::proto::Attached>(message)) return true;
        ::Sleep(1);
    }
    return false;
}
}

CK_TEST(native_server_stats_preserve_owned_descendants_after_root_exit_and_fan_out_once) {
    ckmtest::ScratchDirectory scratch("native-resource-wire");
    const auto endpoint = std::filesystem::path("resource-" + std::to_string(::GetCurrentProcessId()));
    ckm::Settings settings;
    settings.shell = CKMUX_TEST_CHILD_PATH;
    settings.login_shell = false;
    settings.on_exit = ckm::ExitPolicy::Hold;
    ckv::ManualClock clock;
    ckm::server::Server server({endpoint, settings}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);
    Reader first;
    Reader second;
    CK_CHECK(first.connect(endpoint));
    CK_CHECK(attach(server, clock, first));
    CK_CHECK(second.connect(endpoint));
    CK_CHECK(attach(server, clock, second));
    first.say(ckm::proto::WatchStats{1});
    second.say(ckm::proto::WatchStats{1});
    for (int pass = 0; pass < 10; ++pass) { clock.advance(40'000'000); CK_CHECK(server.step()); ::Sleep(1); }
    ckm::server::TerminalSpec spec;
    spec.command = "--job-root";
    spec.working_directory = scratch.path().string();
    auto& terminal = server.open_terminal(0, spec);
    const auto id = terminal.id();
    std::vector<Stats> left;
    std::vector<Stats> right;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (terminal.process_id() != -1 && std::chrono::steady_clock::now() < deadline) {
        clock.advance(40'000'000);
        CK_CHECK(server.step());
        first.stats(left);
        second.stats(right);
        ::Sleep(2);
    }
    CK_CHECK(terminal.process_id() == -1);
    left.clear();
    right.clear();
    // Drain packets already in flight before comparing the same future ticks.
    for (int pass = 0; pass < 10; ++pass) { first.stats(left); second.stats(right); ::Sleep(1); }
    left.clear(); right.clear();
    for (int pass = 0; pass < 3; ++pass) {
        clock.advance(1'000'000'000);
        CK_CHECK(server.step());
        for (int read = 0; read < 10; ++read) { first.stats(left); second.stats(right); ::Sleep(1); }
    }
    CK_CHECK(left.size() == 3U);
    CK_CHECK(left == right);
    unsigned measured_intervals = 0;
    for (const auto& stats : left) {
        CK_CHECK(stats.term == id);
        CK_CHECK(stats.state == ckm::proto::TermStatsState::Available);
        CK_CHECK(stats.cpu_scope == ckm::proto::TermStatsScope::OwnedJobLifetime);
        CK_CHECK(stats.live_processes == 1);
        CK_CHECK(stats.rss_bytes >= 4 * 1024 * 1024);
        CK_CHECK(stats.real_kind == ckm::proto::TermStatsMemory::PrivateResident);
        if ((stats.flags & static_cast<std::uint8_t>(ckm::proto::TermStatsFlag::HasCpu)) != 0)
            ++measured_intervals;
    }
    // A root that exits before its first sampling pass has no first CPU
    // interval. The following two intervals must be real measurements.
    CK_CHECK(measured_intervals >= 2);
    terminal.close();
}

CK_TEST(native_held_exit_banner_preserves_a_clear_that_preceded_the_display_tick) {
    ckmtest::ScratchDirectory scratch("native-held-clear-order");
    const auto endpoint = std::filesystem::path("held-clear-" + std::to_string(::GetCurrentProcessId()));
    ckm::Settings settings;
    settings.shell = CKMUX_TEST_CHILD_PATH;
    settings.login_shell = false;
    settings.on_exit = ckm::ExitPolicy::Hold;
    settings.max_fps = 1;
    ckv::ManualClock clock;
    ckm::server::Server server({endpoint, settings}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);
    Reader reader;
    CK_CHECK(reader.connect(endpoint));
    CK_CHECK(attach(server, clock, reader));
    CK_CHECK(clock.now_nanos() < 1'000'000'000);
    ckm::server::TerminalSpec spec;
    spec.command = "--exit-zero";
    spec.working_directory = scratch.path().string();
    auto& terminal = server.open_terminal(0, spec);
    CK_CHECK(terminal.process_id() > 0);
    CK_CHECK(terminal.session().state() != ckv::core::TerminalSubsessionState::Failed);
    const auto id = terminal.id();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (terminal.process_id() > 0 && std::chrono::steady_clock::now() < deadline) {
        CK_CHECK(server.step());
        ::Sleep(1);
    }
    CK_CHECK(terminal.process_id() == -1);
    CK_CHECK(!terminal.exit_announced());
    reader.say(ckm::proto::WatchStats{1});
    std::vector<Stats> stats;
    while (stats.empty() && std::chrono::steady_clock::now() < deadline) {
        CK_CHECK(server.step());
        reader.stats(stats);
        ::Sleep(1);
    }
    CK_CHECK(stats.size() == 1U);
    if (stats.size() == 1U) {
        CK_CHECK(stats.front().term == id);
        CK_CHECK(stats.front().state == ckm::proto::TermStatsState::Gone);
    }
    CK_CHECK(!terminal.exit_announced());
    stats.clear();
    clock.advance(1'000'000'000);
    CK_CHECK(server.step());
    for (int pass = 0; pass < 10; ++pass) { reader.stats(stats); ::Sleep(1); }
    CK_CHECK(terminal.exit_announced());
    CK_CHECK(server.terminals().find(id) != nullptr);
    CK_CHECK(stats.empty());
    terminal.close();
}

CK_TEST(native_local_footer_samples_the_job_after_root_exit) {
    ckmtest::ScratchDirectory scratch("native-resource-footer");
    ckv::term::HeadlessTerminal host({100, 30});
    ckv::ManualClock clock;
    ckv::ui::Application app(host, clock);
    ckm::client::ClientOptions options;
    options.settings.show_cpu = true;
    options.settings.show_memory_rss = true;
    options.settings.show_memory_real = true;
    options.settings.on_exit = ckm::ExitPolicy::Hold;
    ckv::term::TerminalSubsession* owned = nullptr;
    options.terminal_source = [&](ckm::client::TerminalRequest request) -> ckv::term::TerminalSubsession& {
        request.launch = ckv::core::TerminalLaunchSpec::program(CKMUX_TEST_CHILD_PATH, {"--job-root"});
        request.launch.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
        request.launch.profile.cells = {80, 20};
        request.launch.working_directory = scratch.path().string();
        auto child = ckv::term::launch_terminal_subsession(request.launch, request.options);
        owned = child.get();
        return app.adopt_terminal_subsession(std::move(child));
    };
    ckm::client::ClientApp client(app, std::move(options));
    CK_CHECK(owned != nullptr);
    if (!owned) return;
    CK_CHECK(owned->process_id() > 0);
    CK_CHECK(owned->state() != ckv::core::TerminalSubsessionState::Failed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (owned->process_id() != -1 && std::chrono::steady_clock::now() < deadline) {
        clock.advance(40'000'000);
        app.step(0);
        ::Sleep(2);
    }
    CK_CHECK(owned->process_id() == -1);
    CK_CHECK(owned->state() == ckv::core::TerminalSubsessionState::Exited);
    CK_CHECK(owned->process_resources().live_processes == 1);
    clock.advance(1'000'000'000);
    app.step(0);
    CK_CHECK(client.desktop().windows().size() == 1U);
    if (client.desktop().windows().empty()) return;
    const auto footer = std::string(client.desktop().windows()[0]->footer());
    CK_CHECK(footer.find("RSS ") != std::string::npos);
    CK_CHECK(footer.find("Private RSS ") != std::string::npos);
    CK_CHECK(footer.find("Stats unavailable") == std::string::npos);
    owned->close();
}
