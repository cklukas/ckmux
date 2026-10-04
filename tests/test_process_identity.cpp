// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/process_stats.hpp"
#include "server/terminals.hpp"

#include <limits>
#include <type_traits>
#include <utility>

#include "cvision/testing/cktest.hpp"

CK_TEST(terminal_and_sampler_preserve_the_upstream_full_width_identity) {
    using Id = ckv::core::ProcessId;
    static_assert(std::is_same_v<ckm::platform::ProcessId, Id>);
    static_assert(std::is_same_v<decltype(std::declval<const ckm::server::Terminal&>().process_id()), Id>);
    static_assert(std::is_same_v<decltype(ckm::platform::ProcessTable::Entry{}.pid), Id>);
    const Id root = std::numeric_limits<std::uint32_t>::max();
    const Id child = root - 4;
    const auto table = ckm::platform::ProcessTable::from_entries({
        {1, 0}, {root, 1}, {child, root}, {7, 1}});
    const auto tree = table.tree_of(root);
    CK_CHECK(root == 4'294'967'295LL);
    CK_CHECK(tree.size() == 2U);
    if (tree.size() == 2U) {
        CK_CHECK(tree[0] == root);
        CK_CHECK(tree[1] == child);
    }
    CK_CHECK(table.tree_of(-1).empty());
}
