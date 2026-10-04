// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/stats.hpp"

#include <limits>

namespace ckm {
namespace {

std::uint32_t cpu_rate(std::uint64_t cpu, std::uint64_t wall) {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    const std::uint64_t whole = cpu / wall;
    if (whole > maximum / 1000u) return maximum;
    // Multiply the remainder by 1000 through binary long division, without
    // ever forming an overflowing uint64 product. Ten bits, independent of
    // counter size; exact on every target, including MSVC without uint128.
    const std::uint64_t initial = cpu % wall;
    std::uint64_t remainder = 0;
    std::uint32_t fraction = 0;
    for (unsigned bit = 512; bit != 0; bit >>= 1u) {
        fraction *= 2u;
        if (remainder >= wall - remainder) {
            remainder -= wall - remainder;
            ++fraction;
        } else {
            remainder += remainder;
        }
        if ((1000u & bit) != 0) {
            if (remainder >= wall - initial) {
                remainder -= wall - initial;
                ++fraction;
            } else {
                remainder += initial;
            }
        }
    }
    const std::uint64_t result = whole * 1000u + fraction;
    return result > maximum ? maximum : static_cast<std::uint32_t>(result);
}

}  // namespace

proto::TermStats make_term_stats(const ckv::core::ProcessResources& sample,
                                CpuBaseline& baseline, std::int64_t now,
                                std::uint64_t terminal,
                                std::optional<std::uint64_t> platform_cost) {
    using State = ckv::core::ProcessResourceState;
    proto::TermStats stats;
    stats.term = terminal;
    switch (sample.state) {
        case State::Unsupported: stats.state = proto::TermStatsState::Unsupported; break;
        case State::Available: stats.state = proto::TermStatsState::Available; break;
        case State::Partial: stats.state = proto::TermStatsState::Partial; break;
        case State::Gone: stats.state = proto::TermStatsState::Gone; break;
        case State::Failed: stats.state = proto::TermStatsState::Failed; break;
    }
    stats.cpu_scope = sample.cpu_scope == ckv::core::ProcessCpuScope::OwnedJobLifetime
                          ? proto::TermStatsScope::OwnedJobLifetime
                          : proto::TermStatsScope::LiveProcessTree;
    stats.live_processes = sample.live_processes;
    stats.unreadable_processes = sample.unreadable_processes;
    stats.system_error = sample.system_error;
    if (sample.state == State::Available && sample.live_processes == 0 &&
        sample.unreadable_processes == 0) {
        // An observed empty owned job is finished, not a live measured zero.
        stats.state = proto::TermStatsState::Gone;
        baseline.primed = false;
        return stats;
    }
    if (sample.state != State::Available && sample.state != State::Partial) {
        baseline.primed = false;
        return stats;
    }
    if (sample.live_processes != 0 || sample.unreadable_processes != 0)
        stats.flags |= static_cast<std::uint8_t>(proto::TermStatsFlag::Alive);
    if (sample.rss_bytes) {
        stats.rss_bytes = *sample.rss_bytes;
        stats.flags |= static_cast<std::uint8_t>(proto::TermStatsFlag::HasRss);
    }
    const auto real = sample.private_rss_bytes ? sample.private_rss_bytes : platform_cost;
    if (real) {
        stats.real_bytes = *real;
        stats.real_kind = sample.private_rss_bytes ? proto::TermStatsMemory::PrivateResident
                                                  : proto::TermStatsMemory::PlatformCost;
        stats.flags |= static_cast<std::uint8_t>(proto::TermStatsFlag::HasReal);
    }
    if (sample.cpu_time_nanos) {
        if (baseline.primed && baseline.scope == sample.cpu_scope && now > baseline.at_nanos &&
            *sample.cpu_time_nanos >= baseline.cpu_nanos) {
            // Unsigned subtraction also handles clocks spanning signed zero
            // without signed overflow, provided the ordering above holds.
            const auto wall = static_cast<std::uint64_t>(now) -
                              static_cast<std::uint64_t>(baseline.at_nanos);
            stats.cpu_permille = cpu_rate(*sample.cpu_time_nanos - baseline.cpu_nanos, wall);
            stats.flags |= static_cast<std::uint8_t>(proto::TermStatsFlag::HasCpu);
        }
        baseline.cpu_nanos = *sample.cpu_time_nanos;
        baseline.at_nanos = now;
        baseline.scope = sample.cpu_scope;
        baseline.primed = true;
    } else {
        baseline.primed = false;
    }
    return stats;
}

}  // namespace ckm
