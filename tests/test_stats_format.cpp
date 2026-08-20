// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The frame readout's words (WP-39), pinned where they are cheapest to argue:
// 1024 vs 1000, where "999 KB" becomes "1.0 MB", how 375‰ rounds, and which
// segments a footer holds under which toggles. Pure functions, no terminal.
#include "client/stats_format.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

using ckm::client::format_bytes;
using ckm::client::format_cpu_permille;
using ckm::client::stats_footer;
using ckm::client::StatsToggles;
using ckm::proto::TermStats;
using ckm::proto::TermStatsFlag;

constexpr std::uint64_t KB = 1024;
constexpr std::uint64_t MB = 1024 * KB;
constexpr std::uint64_t GB = 1024 * MB;
constexpr std::uint64_t TB = 1024 * GB;

std::uint8_t flags(bool alive, bool has_real) {
    std::uint8_t value = 0;
    if (alive) value |= static_cast<std::uint8_t>(TermStatsFlag::Alive);
    if (has_real) value |= static_cast<std::uint8_t>(TermStatsFlag::HasReal);
    return value;
}

}  // namespace

CK_TEST(bytes_take_the_unit_that_keeps_three_digits) {
    CK_CHECK(format_bytes(0) == "0 B");
    CK_CHECK(format_bytes(512) == "512 B");
    CK_CHECK(format_bytes(999) == "999 B");
    // 1000 bytes is not yet a kilobyte and not worth four digits: promoted.
    CK_CHECK(format_bytes(1000) == "1.0 KB");
    CK_CHECK(format_bytes(1536) == "1.5 KB");
    CK_CHECK(format_bytes(999 * KB) == "999 KB");
    // The gap 1000..1023 KB, where 1024-based units and 3-digit width fight:
    // promoted and rounded, never "1000 KB".
    CK_CHECK(format_bytes(1000 * KB) == "1.0 MB");
    CK_CHECK(format_bytes(210 * MB) == "210 MB");
    CK_CHECK(format_bytes(96 * MB) == "96 MB");
    // One decimal under ten, none from there up, with snprintf's rounding at
    // the seam: 9.94 stays one-decimal, 9.96 rounds into the integer form.
    CK_CHECK(format_bytes(static_cast<std::uint64_t>(9.94 * static_cast<double>(GB))) == "9.9 GB");
    CK_CHECK(format_bytes(static_cast<std::uint64_t>(9.96 * static_cast<double>(GB))) == "10 GB");
    CK_CHECK(format_bytes(3 * TB) == "3.0 TB");
    CK_CHECK(format_bytes(1200 * GB) == "1.2 TB");
}

CK_TEST(cpu_permille_rounds_to_a_whole_percent_and_carries_many_cores) {
    CK_CHECK(format_cpu_permille(0) == "0%");
    CK_CHECK(format_cpu_permille(4) == "0%");
    CK_CHECK(format_cpu_permille(5) == "1%");
    CK_CHECK(format_cpu_permille(373) == "37%");
    CK_CHECK(format_cpu_permille(375) == "38%");
    CK_CHECK(format_cpu_permille(1000) == "100%");
    // Eight busy cores are a value, not an overflow (the top convention).
    CK_CHECK(format_cpu_permille(8370) == "837%");
}

CK_TEST(the_footer_holds_exactly_what_is_toggled_on) {
    TermStats stats;
    stats.cpu_permille = 373;
    stats.rss_bytes = 210 * MB;
    stats.real_bytes = 96 * MB;
    stats.flags = flags(true, true);

    CK_CHECK(stats_footer(stats, {true, true, true}) == "CPU 37% · RSS 210 MB · Real 96 MB");
    CK_CHECK(stats_footer(stats, {true, false, false}) == "CPU 37%");
    CK_CHECK(stats_footer(stats, {false, true, false}) == "RSS 210 MB");
    CK_CHECK(stats_footer(stats, {false, false, true}) == "Real 96 MB");
    CK_CHECK(stats_footer(stats, {false, true, true}) == "RSS 210 MB · Real 96 MB");
    CK_CHECK(stats_footer(stats, {false, false, false}).empty());
}

CK_TEST(what_cannot_be_said_is_left_out_rather_than_zeroed) {
    TermStats stats;
    stats.cpu_permille = 373;
    stats.rss_bytes = 210 * MB;
    stats.real_bytes = 0;
    // The platform cannot answer Real: the segment is absent even when its
    // toggle is on — nothing, rather than a zero pretending to be measured.
    stats.flags = flags(true, false);
    CK_CHECK(stats_footer(stats, {true, true, true}) == "CPU 37% · RSS 210 MB");

    // A dead child's footer is empty whatever is toggled: the exit banner
    // owns that moment, and a frozen last number would contradict it.
    stats.flags = flags(false, true);
    CK_CHECK(stats_footer(stats, {true, true, true}).empty());
}
