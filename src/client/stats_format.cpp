// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/stats_format.hpp"

#include <array>
#include <cstdio>

namespace ckm::client {

std::string format_bytes(std::uint64_t bytes) {
    static constexpr std::array<const char*, 6> kUnits{"B", "KB", "MB", "GB", "TB", "PB"};
    // Promote at 1000, not 1024, so the number never spends four digits saying
    // what "1.0 MB" says in three — "999 KB" is the last of its unit, and
    // 1000..1023 KB round up through the division below.
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }
    char text[32];
    if (unit == 0) {
        std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
    } else if (value < 9.95) {
        // One decimal under ten ("1.2 GB"); snprintf's own rounding decides
        // the boundary, which is why the test pins 9.95 and not "about ten".
        std::snprintf(text, sizeof text, "%.1f %s", value, kUnits[unit]);
    } else {
        std::snprintf(text, sizeof text, "%.0f %s", value, kUnits[unit]);
    }
    return text;
}

std::string format_cpu_permille(std::uint32_t permille) {
    char text[16];
    std::snprintf(text, sizeof text, "%u%%", (permille + 5u) / 10u);
    return text;
}

std::string stats_footer(const proto::TermStats& stats, const StatsToggles& toggles) {
    const bool alive =
        (stats.flags & static_cast<std::uint8_t>(proto::TermStatsFlag::Alive)) != 0;
    if (!alive || !toggles.any()) return {};
    const bool has_real =
        (stats.flags & static_cast<std::uint8_t>(proto::TermStatsFlag::HasReal)) != 0;
    std::string line;
    const auto append = [&line](std::string part) {
        if (!line.empty()) line += " · ";
        line += std::move(part);
    };
    if (toggles.cpu) append("CPU " + format_cpu_permille(stats.cpu_permille));
    if (toggles.rss) append("RSS " + format_bytes(stats.rss_bytes));
    if (toggles.real && has_real) append("Real " + format_bytes(stats.real_bytes));
    return line;
}

}  // namespace ckm::client
