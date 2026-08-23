// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The client's side of the wire (the testing plan §6). The client decodes the server→
// client half of the catalogue and then does something with it that the server
// never does: it keeps a mirror. So this driver goes one step past the codec
// and drives the two functions that write into that mirror —
// `ckm::apply_delta` for a `GridDelta`, and `ckm::decode_grid` for the grid
// inside an attach snapshot.
//
// The claim under test is the one the protocol spec and WP-4a state as absolute: a
// delta is applied whole or not at all. A mirror holding half a delta shows a
// program's screen with a stripe of some other moment in it, and nothing
// downstream can tell that it happened.
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

#include "common/grid_delta.hpp"
#include "common/proto.hpp"
#include "cvision/core/geometry.hpp"
#include "fuzz_common.hpp"

namespace proto = ckm::proto;

namespace {

// The shape the mirror STARTS in — a known one on purpose, so that every run
// asks the same question of the ops it is given. Where it ends up is another
// matter: a delta may carry a `ResizeOp`, and then the geometry is the peer's
// to state, which is why what is asserted afterwards is coherence rather than
// these two numbers.
constexpr int kMirrorColumns = 80;
constexpr int kMirrorRows = 24;
constexpr std::size_t kMirrorScrollbackLines = 64;

void drive_grid_delta(const proto::GridDelta& delta) {
    ckm::GridState mirror = ckm::blank_state(ckv::Size{kMirrorColumns, kMirrorRows},
                                             kMirrorScrollbackLines);
    const ckm::GridState before = mirror;

    if (!ckm::apply_delta(delta.ops, mirror)) {
        // Refused means untouched. This is the half of "all or nothing" that
        // a test can only assert by holding the state from before.
        ckm::fuzz::require(ckm::same_state(mirror, before));
        return;
    }

    // Accepted means still coherent — which is not the same as "still 80×24".
    // A delta may carry a `ResizeOp`, so the geometry after one is the peer's
    // to state; what may never happen is a mirror whose parts disagree. So:
    // a size that is a size at all, inside the bound both ends refuse the same
    // number at, a grid holding exactly that many cells, and a history inside
    // the reader's limit and measured in rows of the current width.
    ckm::fuzz::require(mirror.cells.width > 0 && mirror.cells.height > 0);
    ckm::fuzz::require(mirror.cells.width <= proto::kMaxGridColumns);
    ckm::fuzz::require(mirror.cells.height <= proto::kMaxGridRows);
    ckm::fuzz::require(mirror.grid.size() == static_cast<std::size_t>(mirror.cells.width) *
                                                 static_cast<std::size_t>(mirror.cells.height));
    ckm::fuzz::require(ckm::scrollback_lines(mirror) <= kMirrorScrollbackLines);
    ckm::fuzz::require(ckm::scrollback_cells(mirror).size() ==
                       ckm::scrollback_lines(mirror) *
                           static_cast<std::size_t>(mirror.cells.width));

    // Applying the same delta to the mirror it already produced must still be
    // an all-or-nothing decision — there is no op whose second application is
    // a crash rather than an answer.
    ckm::GridState twice = mirror;
    const ckm::GridState before_twice = twice;
    if (!ckm::apply_delta(delta.ops, twice)) ckm::fuzz::require(ckm::same_state(twice, before_twice));
}

void drive_snapshot(const proto::Attached& attached) {
    for (const proto::TerminalState& state : attached.snapshot.terminals) {
        // The geometry is handed over exactly as the peer stated it, however
        // absurd: `decode_grid` refuses a size past the protocol's bound
        // itself now, so the driver has nothing to protect the allocator from
        // and no reason to ask a smaller question than the client asks.
        const ckv::Size cells{state.columns, state.rows};
        ckm::GridState mirror = ckm::blank_state(ckv::Size{0, 0}, kMirrorScrollbackLines);
        if (!ckm::decode_grid(state.grid, cells, mirror)) continue;
        // A grid that decoded is a grid of exactly the size it was decoded
        // for: run lengths that summed to something else are a refusal, never
        // a short grid that later indexes past its end.
        ckm::fuzz::require(mirror.cells == cells);
        ckm::fuzz::require(mirror.cells.width >= 0 && mirror.cells.height >= 0);
        ckm::fuzz::require(mirror.cells.width <= proto::kMaxGridColumns);
        ckm::fuzz::require(mirror.cells.height <= proto::kMaxGridRows);
        ckm::fuzz::require(mirror.grid.size() == static_cast<std::size_t>(mirror.cells.width) *
                                                     static_cast<std::size_t>(mirror.cells.height));
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);

    proto::FrameReader reader;
    if (!reader.append(input)) return 0;

    for (;;) {
        proto::Message message;
        const proto::DecodeError error = reader.next(message);
        if (error != proto::DecodeError::None) break;  // incomplete, or the connection is over

        if (!ckm::fuzz::is_server_to_client(proto::type_of(message))) continue;
        ckm::fuzz::require_survives_its_encoder(message);

        if (const auto* delta = std::get_if<proto::GridDelta>(&message)) {
            drive_grid_delta(*delta);
            continue;
        }
        if (const auto* attached = std::get_if<proto::Attached>(&message)) {
            drive_snapshot(*attached);
            continue;
        }
    }
    return 0;
}
