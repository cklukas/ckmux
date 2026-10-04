// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include "common/proto.hpp"
#include "cvision/core/process_resources.hpp"

namespace ckm {

// Reader-independent rate history. A clock reading of zero is a real sample.
struct CpuBaseline {
    std::uint64_t cpu_nanos = 0;
    std::int64_t at_nanos = 0;
    ckv::core::ProcessCpuScope scope = ckv::core::ProcessCpuScope::ProcessLifetime;
    bool primed = false;
};

// Pure conversion; unavailable fields never become measured zero. Platform
// cost is POSIX footprint/PSS, not Windows private resident working-set pages.
proto::TermStats make_term_stats(const ckv::core::ProcessResources& sample,
                                CpuBaseline& baseline, std::int64_t now,
                                std::uint64_t terminal = 0,
                                std::optional<std::uint64_t> platform_cost = std::nullopt);

}  // namespace ckm
