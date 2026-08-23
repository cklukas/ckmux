// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/capability_grace.hpp"

namespace ckm::client {

bool first_terminal_may_open(bool host_capability_answered,
                             std::int64_t waited_nanos) noexcept {
    // An answer ends the wait whenever it arrives — including late, which is
    // the case the defect was about. Before the deadline the answer is worth
    // waiting for; after it, the fallback is worth more than a further wait.
    //
    // `>=` rather than `>`: at exactly the deadline the grace is spent, and a
    // predicate that waited one more tick would make the boundary depend on
    // the caller's polling interval rather than on the deadline.
    return host_capability_answered || waited_nanos >= kCapabilityGraceNanos;
}

}  // namespace ckm::client
