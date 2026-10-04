// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/stats.hpp"
#include "client/stats_format.hpp"
#include "client/client_app.hpp"
#include "client/server_session.hpp"

#include <limits>

#include "cvision/testing/cktest.hpp"
#include "cvision/term/headless_terminal.hpp"

namespace {
using ckv::core::ProcessResources;
using ckv::core::ProcessResourceState;
using ckm::proto::TermStatsFlag;
bool has(const ckm::proto::TermStats& stats, TermStatsFlag flag) {
    return (stats.flags & static_cast<std::uint8_t>(flag)) != 0;
}
ProcessResources job() {
    ProcessResources sample;
    sample.state = ProcessResourceState::Available;
    sample.cpu_scope = ckv::core::ProcessCpuScope::OwnedJobLifetime;
    sample.live_processes = 2;
    sample.cpu_time_nanos = 0;
    sample.rss_bytes = 0;
    sample.private_rss_bytes = 0;
    return sample;
}
}

CK_TEST(resource_zero_is_measured_but_first_cpu_interval_is_unavailable) {
    ckm::CpuBaseline baseline;
    const auto first = ckm::make_term_stats(job(), baseline, 0, 23);
    CK_CHECK(first.term == 23);
    CK_CHECK(has(first, TermStatsFlag::HasRss));
    CK_CHECK(has(first, TermStatsFlag::HasReal));
    CK_CHECK(!has(first, TermStatsFlag::HasCpu));
    CK_CHECK(first.cpu_scope == ckm::proto::TermStatsScope::OwnedJobLifetime);
    CK_CHECK(first.real_kind == ckm::proto::TermStatsMemory::PrivateResident);
    CK_CHECK(ckm::client::stats_footer(first, {true, true, true}) ==
             "CPU ? · RSS 0 B · Private RSS 0 B");
    const auto second = ckm::make_term_stats(job(), baseline, 1'000'000'000);
    CK_CHECK(has(second, TermStatsFlag::HasCpu));
    CK_CHECK(second.cpu_permille == 0);
}

CK_TEST(resource_partial_and_refused_fields_never_become_zero_measurements) {
    ckm::CpuBaseline baseline;
    auto sample = job();
    sample.state = ProcessResourceState::Partial;
    sample.unreadable_processes = 3;
    sample.system_error = 5;
    sample.rss_bytes.reset();
    sample.private_rss_bytes.reset();
    (void)ckm::make_term_stats(sample, baseline, 0);
    sample.cpu_time_nanos = 375'000'000;
    const auto partial = ckm::make_term_stats(sample, baseline, 1'000'000'000);
    CK_CHECK(has(partial, TermStatsFlag::HasCpu));
    CK_CHECK(!has(partial, TermStatsFlag::HasRss));
    CK_CHECK(!has(partial, TermStatsFlag::HasReal));
    CK_CHECK(partial.unreadable_processes == 3);
    CK_CHECK(partial.system_error == 5);
    CK_CHECK(ckm::client::stats_footer(partial, {true, true, true}) == "CPU 38% · RSS ? · partial");
    sample.cpu_time_nanos.reset();
    const auto missing = ckm::make_term_stats(sample, baseline, 2'000'000'000);
    CK_CHECK(!has(missing, TermStatsFlag::HasCpu));
    CK_CHECK(!baseline.primed);
    sample.state = ProcessResourceState::Failed;
    const auto failed = ckm::make_term_stats(sample, baseline, 3'000'000'000);
    CK_CHECK(failed.state == ckm::proto::TermStatsState::Failed);
    CK_CHECK(ckm::client::stats_footer(failed, {true, true, true}) == "Stats unavailable (error 5)");
    sample.state = ProcessResourceState::Unsupported;
    sample.system_error = 0;
    CK_CHECK(ckm::client::stats_footer(ckm::make_term_stats(sample, baseline, 4'000'000'000),
                                    {true, true, true}) == "Stats unavailable");
}

CK_TEST(resource_rate_resets_on_failed_query_rollback_scope_change_and_equal_time) {
    ckm::CpuBaseline baseline;
    auto sample = job();
    (void)ckm::make_term_stats(sample, baseline, 0);
    sample.cpu_time_nanos = 3'000'000'000;
    CK_CHECK(ckm::make_term_stats(sample, baseline, 1'000'000'000).cpu_permille == 3000);
    sample.state = ProcessResourceState::Failed;
    (void)ckm::make_term_stats(sample, baseline, 2'000'000'000);
    sample.state = ProcessResourceState::Available;
    sample.cpu_time_nanos = 9'000'000'000;
    CK_CHECK(!has(ckm::make_term_stats(sample, baseline, 3'000'000'000), TermStatsFlag::HasCpu));
    sample.cpu_time_nanos = 1;
    CK_CHECK(!has(ckm::make_term_stats(sample, baseline, 4'000'000'000), TermStatsFlag::HasCpu));
    sample.cpu_scope = ckv::core::ProcessCpuScope::ProcessLifetime;
    CK_CHECK(!has(ckm::make_term_stats(sample, baseline, 5'000'000'000), TermStatsFlag::HasCpu));
    CK_CHECK(!has(ckm::make_term_stats(sample, baseline, 5'000'000'000), TermStatsFlag::HasCpu));
}

CK_TEST(resource_rate_is_exact_and_saturating_without_counter_or_clock_overflow) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (std::uint64_t wall = 1; wall < 60; ++wall) {
        for (std::uint64_t cpu = 0; cpu < 100; ++cpu) {
            ckm::CpuBaseline baseline;
            auto sample = job();
            (void)ckm::make_term_stats(sample, baseline, 0);
            sample.cpu_time_nanos = cpu;
            const auto stats = ckm::make_term_stats(sample, baseline, static_cast<std::int64_t>(wall));
            CK_CHECK(stats.cpu_permille == cpu * 1000 / wall);
        }
    }
    ckm::CpuBaseline baseline;
    auto sample = job();
    (void)ckm::make_term_stats(sample, baseline, std::numeric_limits<std::int64_t>::min());
    sample.cpu_time_nanos = maximum;
    CK_CHECK(ckm::make_term_stats(sample, baseline, std::numeric_limits<std::int64_t>::max()).cpu_permille == 1000);
    baseline = {};
    sample.cpu_time_nanos = 0;
    (void)ckm::make_term_stats(sample, baseline, 0);
    sample.cpu_time_nanos = maximum;
    const auto saturated = ckm::make_term_stats(sample, baseline, 1);
    CK_CHECK(saturated.cpu_permille == std::numeric_limits<std::uint32_t>::max());
    CK_CHECK(ckm::client::format_cpu_permille(saturated.cpu_permille) == "429496730%");
}

CK_TEST(resource_job_descendants_and_empty_job_are_distinct_from_failed_queries) {
    ckm::CpuBaseline baseline;
    auto sample = job();
    sample.live_processes = 1; // Root identity may already be -1; job owns this child.
    sample.rss_bytes = 4096;
    const auto live = ckm::make_term_stats(sample, baseline, 0);
    CK_CHECK(has(live, TermStatsFlag::Alive));
    CK_CHECK(live.rss_bytes == 4096);
    sample.live_processes = 0;
    const auto empty = ckm::make_term_stats(sample, baseline, 1);
    CK_CHECK(empty.state == ckm::proto::TermStatsState::Gone);
    CK_CHECK(ckm::client::stats_footer(empty, {true, true, true}).empty());
    sample.state = ProcessResourceState::Partial;
    sample.unreadable_processes = 1;
    const auto unreadable = ckm::make_term_stats(sample, baseline, 2);
    CK_CHECK(unreadable.state == ckm::proto::TermStatsState::Partial);
    CK_CHECK(has(unreadable, TermStatsFlag::Alive));
}

CK_TEST(resource_wire_carries_availability_and_refuses_invalid_enumerations) {
    ckm::CpuBaseline baseline;
    auto sample = job();
    sample.state = ProcessResourceState::Partial;
    sample.unreadable_processes = 7;
    sample.system_error = 5;
    sample.rss_bytes.reset();
    const auto original = ckm::make_term_stats(sample, baseline, 0, 81);
    const auto bytes = ckm::proto::encode(original);
    ckm::proto::Message decoded;
    CK_CHECK(ckm::proto::decode(bytes, decoded).ok());
    CK_CHECK(std::get<ckm::proto::TermStats>(decoded) == original);
    for (const std::size_t field : {37U, 38U, 39U}) {
        auto bad = bytes;
        bad[field] = static_cast<char>(255);
        CK_CHECK(!ckm::proto::decode(bad, decoded).ok());
    }
    CK_CHECK(ckm::proto::kProtocolVersion == 4);
}

CK_TEST(resource_wire_mirror_keeps_its_report_when_the_local_stats_timer_runs) {
    ckm::client::ServerSession session([](const ckm::proto::Message&) {});
    ckm::proto::Attached attached;
    attached.session = 1;
    ckm::proto::TerminalState state;
    state.term = 81;
    state.columns = 80;
    state.rows = 20;
    state.rect = {0, 0, 80, 20};
    state.grid = ckm::proto::to_runs(std::vector<ckv::Cell>(1600));
    attached.snapshot.terminals.push_back(state);
    CK_CHECK(session.handle(attached));
    auto* mirror = session.terminal(81);
    CK_CHECK(mirror != nullptr);
    if (!mirror) return;
    ckv::term::HeadlessTerminal host({100, 30});
    ckv::ManualClock clock;
    ckv::ui::Application app(host, clock);
    ckm::client::ClientOptions options;
    options.settings.show_memory_rss = true;
    options.terminal_source = [&](ckm::client::TerminalRequest) -> ckv::term::TerminalSubsession& {
        return *mirror;
    };
    ckm::client::ClientApp client(app, std::move(options));
    session.on_stats = [&](ckm::client::RemoteTerminalSubsession& terminal, const ckm::proto::TermStats& stats) {
        client.receive_terminal_stats(terminal, stats);
    };
    ckm::CpuBaseline baseline;
    auto sample = job();
    sample.rss_bytes = 8 * 1024 * 1024;
    const auto expected = ckm::make_term_stats(sample, baseline, 0, 81);
    ckm::proto::Message decoded;
    CK_CHECK(ckm::proto::decode(ckm::proto::encode(expected), decoded).ok());
    CK_CHECK(session.handle(decoded));
    app.step(0);
    CK_CHECK(client.desktop().windows().size() == 1U);
    if (client.desktop().windows().empty()) return;
    const auto footer = std::string(client.desktop().windows()[0]->footer());
    CK_CHECK(footer == "RSS 8.0 MB");
    for (int tick = 0; tick < 3; ++tick) { clock.advance(1'000'000'000); app.step(0); }
    CK_CHECK(client.desktop().windows()[0]->footer() == footer);
}
