// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The frame readout's words (the work queue WP-39): pure functions from numbers to
// the line a terminal window's frame footer shows, kept out of ClientApp so
// the unit boundaries — 1024 vs 1000, "999 KB" vs "0.98 MB", how 375‰ rounds
// — are pinned by a test once instead of argued at a review twice.
#pragma once

#include <cstdint>
#include <string>

#include "common/proto.hpp"

namespace ckm::client {

// Which readouts the reader has turned on (the View menu's three checkboxes,
// persisted as `[general] show-cpu` / `show-memory-rss` / `show-memory-real`).
struct StatsToggles {
    bool cpu = false;
    bool rss = false;
    bool real = false;
    bool any() const noexcept { return cpu || rss || real; }
};

// Bytes with an auto-scaled unit: 1024-based, promoted at 1000 so the number
// keeps at most three significant digits ("999 KB", then "1.0 MB" — never
// "1000 KB"), one decimal under 10 and none from there up. The unit ladder is
// the reader's (KB/MB/GB/TB), not the standards body's.
std::string format_bytes(std::uint64_t bytes);

// Tenths of a percent of one core, shown as a rounded whole percent: 373 is
// "37%", 375 is "38%", and 8370 is "837%" — more than one core is a value,
// not an overflow (the `top` convention, the work queue).
std::string format_cpu_permille(std::uint32_t permille);

// The whole line, e.g. "CPU 37% · RSS 210 MB · Real 96 MB", holding only the
// parts whose toggles are on. Empty when nothing is on, when the terminal's
// child is gone (the exit banner owns that moment), and for a Real segment
// the platform cannot answer (`has_real` clear) — nothing, rather than a zero
// pretending to be a measurement.
std::string stats_footer(const proto::TermStats& stats, const StatsToggles& toggles);

}  // namespace ckm::client
