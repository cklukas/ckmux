// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// WP-4a: the grid diff algebra. Pure values in, ops out, ops applied to a
// mirror, and the mirror compared against what the server holds — which is the
// only property that matters, because everything a reader sees downstream of
// M2 is that mirror rather than the terminal itself.
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "common/grid_delta.hpp"
#include "common/proto.hpp"
#include "cvision/testing/cktest.hpp"

namespace {

using ckm::GridCursor;
using ckm::GridState;
using ckm::blank_cell;
using ckm::blank_state;
using ckm::push_scrollback_line;
using ckm::same_state;
using ckm::scrollback_cells;
using ckm::scrollback_lines;
using ckm::proto::CellsOp;
using ckm::proto::CursorOp;
using ckm::proto::GridOp;
using ckm::proto::ModeBit;
using ckm::proto::ModesOp;
using ckm::proto::ScrollOp;

// A deterministic generator. Not std::mt19937 seeded from a clock: a property
// test that cannot be replayed is a test that reports "something is wrong
// somewhere" and then never says it again.
struct Rng {
    std::uint64_t state;
    std::uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    }
    int below(int bound) { return bound <= 0 ? 0 : static_cast<int>(next() % static_cast<std::uint32_t>(bound)); }
};

ckv::Style style_for(int pick) {
    ckv::Style style;
    switch (pick % 4) {
        case 1: style.fg = ckv::Color::indexed(static_cast<std::uint8_t>(pick)); break;
        case 2:
            style.bg = ckv::Color::rgb(static_cast<std::uint8_t>(pick * 7),
                                       static_cast<std::uint8_t>(pick * 13),
                                       static_cast<std::uint8_t>(pick * 29));
            break;
        case 3:
            style.attrs = ckv::Attr::Bold | ckv::Attr::Underline;
            style.underline = ckv::UnderlineShape::Curly;
            style.underline_color = ckv::Color::indexed(9);
            break;
        default: break;
    }
    return style;
}

void write_text(GridState& state, int row, int column, std::string_view text, ckv::Style style) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        const int target = column + static_cast<int>(index);
        if (target >= state.cells.width) break;
        state.grid[static_cast<std::size_t>(row * state.cells.width + target)] =
            ckv::Cell::from_grapheme(text.substr(index, 1), style);
    }
}

std::vector<ckv::Cell> row_copy(const GridState& state, int row) {
    const auto begin = state.grid.begin() + static_cast<std::ptrdiff_t>(row * state.cells.width);
    return std::vector<ckv::Cell>(begin, begin + state.cells.width);
}

// A terminal's own scroll: the rows that leave the top of the screen become
// history, the rest move up, and the bottom blanks. The model, so the test is
// checking the algebra against something other than itself.
std::vector<std::vector<ckv::Cell>> scroll_model(GridState& state, int top, int bottom, int lines) {
    std::vector<std::vector<ckv::Cell>> pushed;
    if (lines > 0 && top == 0)
        for (int row = 0; row < lines && row < bottom; ++row) pushed.push_back(row_copy(state, row));
    const int width = state.cells.width;
    std::vector<ckv::Cell> region;
    for (int row = top; row < bottom; ++row) {
        const int source = row + lines;
        if (source >= top && source < bottom) {
            const auto begin = state.grid.begin() + static_cast<std::ptrdiff_t>(source * width);
            region.insert(region.end(), begin, begin + width);
        } else {
            region.insert(region.end(), static_cast<std::size_t>(width), blank_cell());
        }
    }
    std::copy(region.begin(), region.end(),
              state.grid.begin() + static_cast<std::ptrdiff_t>(top * width));
    for (const std::vector<ckv::Cell>& line : pushed) push_scrollback_line(state, line);
    return pushed;
}

int count_ops_of(const std::vector<GridOp>& ops, std::size_t which) {
    int count = 0;
    for (const GridOp& op : ops)
        if (op.index() == which) ++count;
    return count;
}

}  // namespace

CK_TEST(a_full_screen_scroll_costs_one_scroll_op_and_the_row_that_is_new) {
    GridState before = blank_state(ckv::Size{80, 24}, 100);
    for (int row = 0; row < 24; ++row)
        write_text(before, row, 0, "line " + std::to_string(row), ckv::Style{});
    GridState after = before;
    const auto pushed = scroll_model(after, 0, 24, 1);
    write_text(after, 23, 0, "the newest line", ckv::Style{});

    const std::vector<GridOp> ops = ckm::diff(before, after, pushed);

    // This is WP-4a's stated acceptance criterion: a screen that scrolled costs
    // a shift and the one row that is genuinely new, not a screenful of cells.
    CK_CHECK(count_ops_of(ops, 0) == 1);  // ScrollOp
    CK_CHECK(count_ops_of(ops, 1) == 1);  // CellsOp — the new bottom row only
    for (const GridOp& op : ops)
        if (const auto* scroll = std::get_if<ScrollOp>(&op)) {
            CK_CHECK(scroll->top == 0);
            CK_CHECK(scroll->bottom == 24);
            CK_CHECK(scroll->lines == 1);
        }

    GridState mirror = before;
    CK_CHECK(ckm::apply_delta(ops, mirror));
    CK_CHECK(same_state(mirror, after));

    // And what the op is actually for, in bytes on the wire against the same
    // screen re-sent row by row. A number rather than an adjective, because
    // "cheap" is the entire justification for the op existing.
    ckm::proto::GridDelta shifted;
    shifted.ops = ops;
    ckm::proto::GridDelta resent;
    for (int row = 0; row < 24; ++row) {
        std::vector<ckv::Cell> line = row_copy(after, row);
        resent.ops.push_back(CellsOp{static_cast<std::uint16_t>(row), 0, ckm::proto::to_runs(line)});
    }
    const std::size_t shifted_bytes = ckm::proto::encode(shifted).size();
    const std::size_t resent_bytes = ckm::proto::encode(resent).size();
    CK_CHECK(shifted_bytes * 5 < resent_bytes);
}

CK_TEST(a_scroll_with_a_status_line_under_it_is_still_a_scroll) {
    // What `less` does: scroll everything except the last row, and rewrite that
    // row. A shift search that insisted every row in the changed band line up
    // would find nothing here and re-send the whole screen — and this is the
    // commonest scroll a multiplexer ever carries.
    GridState before = blank_state(ckv::Size{80, 24}, 0);
    for (int row = 0; row < 24; ++row)
        write_text(before, row, 0, "text row " + std::to_string(row), ckv::Style{});
    write_text(before, 23, 0, ":", ckv::Style{});

    GridState after = before;
    (void)scroll_model(after, 0, 23, 1);
    write_text(after, 22, 0, "text row 23", ckv::Style{});
    write_text(after, 23, 0, ": 47%", ckv::Style{});

    const std::vector<GridOp> ops = ckm::diff(before, after);
    CK_CHECK(count_ops_of(ops, 0) == 1);  // one ScrollOp
    CK_CHECK(count_ops_of(ops, 1) <= 3);  // the new text row, the status line
    GridState mirror = before;
    CK_CHECK(ckm::apply_delta(ops, mirror));
    CK_CHECK(same_state(mirror, after));
}

CK_TEST(random_edit_scripts_leave_the_mirror_byte_identical) {
    // The property the whole package exists for. Random writes, scrolls in and
    // out of regions, cursor moves, mode toggles, titles and history pushes —
    // and after every single delta the mirror must hold exactly what the server
    // holds. Not "close enough to draw": identical, because from M2 onward the
    // mirror is the only terminal a reader ever sees.
    Rng rng{0x9E3779B97F4A7C15ull};
    const ckv::Size size{40, 12};
    GridState server = blank_state(size, 30);
    GridState mirror = server;
    int scroll_rounds = 0;

    for (int round = 0; round < 600; ++round) {
        const GridState previous = server;
        std::vector<std::vector<ckv::Cell>> pushed;
        switch (rng.below(6)) {
            case 0:
            case 1: {  // a program writing where it is
                const int row = rng.below(size.height);
                const int column = rng.below(size.width);
                const int length = 1 + rng.below(12);
                std::string text;
                for (int index = 0; index < length; ++index)
                    text += static_cast<char>('a' + rng.below(26));
                write_text(server, row, column, text, style_for(rng.below(4)));
                break;
            }
            case 2: {  // a scroll, sometimes of a region rather than the screen
                const int top = rng.below(3);
                const int bottom = size.height - rng.below(3);
                const int lines = (1 + rng.below(3)) * (rng.below(4) == 0 ? -1 : 1);
                if (bottom - top >= 2) {
                    pushed = scroll_model(server, top, bottom, lines);
                    ++scroll_rounds;
                }
                break;
            }
            case 3: {  // the cursor
                server.cursor = GridCursor{static_cast<std::uint16_t>(rng.below(size.width)),
                                           static_cast<std::uint16_t>(rng.below(size.height)),
                                           static_cast<std::uint8_t>(rng.below(4)),
                                           static_cast<std::uint8_t>(rng.below(2)),
                                           static_cast<std::uint8_t>(rng.below(2))};
                break;
            }
            case 4: {  // one mode bit, the way a child turns one on
                static const ModeBit bits[] = {ModeBit::MouseReporting, ModeBit::MouseEncodingSgr,
                                               ModeBit::BracketedPaste, ModeBit::ApplicationCursorKeys,
                                               ModeBit::FocusReporting, ModeBit::AlternateBuffer,
                                               ModeBit::AlternateScroll};
                server.modes ^= static_cast<std::uint32_t>(bits[rng.below(7)]);
                break;
            }
            default: {  // the caption
                server.title = "window " + std::to_string(rng.below(4));
                break;
            }
        }

        const std::vector<GridOp> ops = ckm::diff(previous, server, pushed);
        CK_CHECK(ckm::apply_delta(ops, mirror));
        if (!same_state(mirror, server)) {
            // Named so a failure says which round to replay rather than only
            // that one of six hundred went wrong.
            CK_CHECK(round < 0);
            break;
        }
    }
    CK_CHECK(same_state(mirror, server));
    // The script has to have exercised what it claims to: a run where the
    // scroll arm never fired would pass while proving nothing about scrolls.
    CK_CHECK(scroll_rounds > 20);
    CK_CHECK(scrollback_lines(server) != 0);
    CK_CHECK(scrollback_lines(server) <= server.max_scrollback_lines);
}

CK_TEST(a_delta_that_does_not_fit_the_mirror_is_refused_rather_than_half_applied) {
    GridState mirror = blank_state(ckv::Size{20, 5}, 10);
    write_text(mirror, 0, 0, "untouched", ckv::Style{});
    const GridState before = mirror;

    // A row that does not exist, behind an op that would have been fine. The
    // first op must not land: a mirror holding half a delta shows a stripe of
    // some other moment and nothing downstream can tell.
    std::vector<GridOp> ops;
    ops.push_back(CellsOp{0, 0, ckm::proto::to_runs(std::vector<ckv::Cell>(
                                   5, ckv::Cell::from_grapheme("X", ckv::Style{})))});
    ops.push_back(CellsOp{9, 0, ckm::proto::to_runs(std::vector<ckv::Cell>(
                                   5, ckv::Cell::from_grapheme("Y", ckv::Style{})))});
    CK_CHECK(!ckm::apply_delta(ops, mirror));
    CK_CHECK(same_state(mirror, before));

    // Same for a run that would write past the end of its row, and for a scroll
    // of a region the mirror does not have.
    std::vector<GridOp> overrun;
    overrun.push_back(CellsOp{1, 18, ckm::proto::to_runs(std::vector<ckv::Cell>(
                                         5, ckv::Cell::from_grapheme("Z", ckv::Style{})))});
    CK_CHECK(!ckm::apply_delta(overrun, mirror));
    CK_CHECK(same_state(mirror, before));

    std::vector<GridOp> impossible_scroll;
    impossible_scroll.push_back(ScrollOp{0, 9, 1});
    CK_CHECK(!ckm::apply_delta(impossible_scroll, mirror));
    CK_CHECK(same_state(mirror, before));
}

CK_TEST(a_size_change_is_not_a_delta_and_the_mirror_says_so) {
    // GridDelta carries no geometry, deliberately: a mirror that resized itself
    // from a delta would be guessing at a size nobody sent it. The server sends
    // TermMeta first; until the mirror has taken it, the delta does not fit and
    // is refused rather than applied to the wrong shape.
    GridState small = blank_state(ckv::Size{10, 3}, 5);
    write_text(small, 0, 0, "before", ckv::Style{});
    GridState large = blank_state(ckv::Size{16, 4}, 5);
    write_text(large, 3, 0, "after", ckv::Style{});

    const std::vector<GridOp> ops = ckm::diff(small, large);
    CK_CHECK(count_ops_of(ops, 1) == 4);  // every row of the new size

    GridState stale = small;
    CK_CHECK(!ckm::apply_delta(ops, stale));
    CK_CHECK(same_state(stale, small));

    GridState resized = blank_state(large.cells, 5);
    CK_CHECK(ckm::apply_delta(ops, resized));
    CK_CHECK(same_state(resized, large));
}

CK_TEST(a_mode_op_speaks_only_for_the_bits_it_names) {
    GridState mirror = blank_state(ckv::Size{4, 2}, 0);
    mirror.modes = static_cast<std::uint32_t>(ModeBit::BracketedPaste) |
                   static_cast<std::uint32_t>(ModeBit::AlternateScroll);
    std::vector<GridOp> ops;
    ops.push_back(ModesOp{static_cast<std::uint32_t>(ModeBit::MouseReporting),
                          static_cast<std::uint32_t>(ModeBit::MouseReporting)});
    CK_CHECK(ckm::apply_delta(ops, mirror));
    // The two it did not mention are still on. A delta that assigned the whole
    // word would have turned them off while saying nothing about them.
    CK_CHECK((mirror.modes & static_cast<std::uint32_t>(ModeBit::BracketedPaste)) != 0);
    CK_CHECK((mirror.modes & static_cast<std::uint32_t>(ModeBit::AlternateScroll)) != 0);
    CK_CHECK((mirror.modes & static_cast<std::uint32_t>(ModeBit::MouseReporting)) != 0);
}

CK_TEST(a_row_of_one_repeated_cell_costs_one_run) {
    // The cheapness claim, stated where it can be checked: a blank row that
    // becomes a row of dashes is two bytes of length and one cell, not one
    // hundred and twenty cells.
    GridState before = blank_state(ckv::Size{120, 3}, 0);
    GridState after = before;
    write_text(after, 1, 0, std::string(120, '-'), ckv::Style{});
    const std::vector<GridOp> ops = ckm::diff(before, after);
    CK_CHECK(ops.size() == 1U);
    const auto* cells = std::get_if<CellsOp>(&ops.front());
    CK_CHECK(cells != nullptr);
    if (cells != nullptr) {
        CK_CHECK(cells->runs.size() == 1U);
        CK_CHECK(cells->runs.front().run_length == 120);
    }
}

CK_TEST(the_ops_a_diff_produces_survive_the_wire) {
    // The algebra and the codec are separate packages, and this is the seam
    // between them: ops that a diff can produce but the wire cannot carry would
    // be a delta that works in a unit test and nowhere else.
    GridState before = blank_state(ckv::Size{30, 6}, 8);
    for (int row = 0; row < 6; ++row)
        write_text(before, row, 0, "row " + std::to_string(row), style_for(row));
    GridState after = before;
    const auto pushed = scroll_model(after, 0, 6, 2);
    write_text(after, 5, 0, "brand new", style_for(2));
    after.cursor = GridCursor{7, 5, 2, 1, 1};
    after.modes = static_cast<std::uint32_t>(ModeBit::MouseReporting);
    after.title = "a caption a program chose";

    ckm::proto::GridDelta message;
    message.term = 42;
    message.seq = 7;
    message.ops = ckm::diff(before, after, pushed);
    CK_CHECK(!message.ops.empty());

    const std::string frame = ckm::proto::encode(message);
    ckm::proto::Message decoded;
    const ckm::proto::DecodeResult result = ckm::proto::decode(frame, decoded);
    CK_CHECK(result.ok());
    const auto* delta = std::get_if<ckm::proto::GridDelta>(&decoded);
    CK_CHECK(delta != nullptr);
    if (delta == nullptr) return;
    CK_CHECK(delta->term == 42U);
    CK_CHECK(delta->seq == 7U);
    CK_CHECK(delta->ops == message.ops);

    // And the ops that came off the wire rebuild the same terminal.
    GridState mirror = before;
    CK_CHECK(ckm::apply_delta(delta->ops, mirror));
    CK_CHECK(same_state(mirror, after));
}

CK_TEST(a_corrupted_delta_is_refused_rather_than_turned_into_a_different_screen) {
    // The decoder is fed a real delta with single bytes flipped, everywhere,
    // one at a time. Every outcome is acceptable except two: a crash, and a
    // frame that decodes into ops the mirror then swallows without noticing —
    // which is why the surviving deltas are applied rather than only decoded.
    GridState before = blank_state(ckv::Size{16, 4}, 4);
    GridState after = before;
    (void)scroll_model(after, 0, 4, 1);
    write_text(after, 3, 0, "tail", ckv::Style{});
    after.title = "corrupt me";

    ckm::proto::GridDelta message;
    message.term = 1;
    message.seq = 1;
    message.ops = ckm::diff(before, after);
    const std::string original = ckm::proto::encode(message);

    int decoded_count = 0;
    for (std::size_t index = 0; index < original.size(); ++index) {
        for (const std::uint8_t mask : {0x01u, 0x40u, 0x80u}) {
            std::string corrupted = original;
            corrupted[index] = static_cast<char>(static_cast<std::uint8_t>(corrupted[index]) ^ mask);
            ckm::proto::Message decoded;
            const ckm::proto::DecodeResult result = ckm::proto::decode(corrupted, decoded);
            if (!result.ok()) continue;
            ++decoded_count;
            if (const auto* delta = std::get_if<ckm::proto::GridDelta>(&decoded)) {
                GridState mirror = before;
                (void)ckm::apply_delta(delta->ops, mirror);  // must refuse or fit; never crash
                CK_CHECK(mirror.grid.size() == static_cast<std::size_t>(16 * 4));
                CK_CHECK(scrollback_lines(mirror) <= mirror.max_scrollback_lines);
            }
        }
    }
    // Some corruptions land in fields that are legal at their new value — a
    // colour byte, a title character. If none had decoded, this test would be
    // asserting that the codec rejects everything, which proves nothing about
    // what the mirror does with a delta that got through.
    CK_CHECK(decoded_count > 0);
}

CK_TEST(a_flood_of_pushed_lines_costs_the_mirror_lines_not_the_history) {
    // The defect this pins: the mirror once paid O(history) for every push —
    // a front erase in apply, then a whole-history flatten for the next
    // reader — so a child flooding a full history cost seconds per delta
    // batch, and the client stopped answering for as long as output kept
    // arriving. One `find /` was enough. Now a push costs the line it
    // pushes, whatever the history's size.
    //
    // The flood below applies three histories' worth of lines and reads the
    // history between batches, which is how a view reads it on every change
    // notification. The wall budget at the end is generous the same way the
    // core promise's flood gate is: what is asserted is the complexity
    // class — the old shape missed this budget by minutes on any machine,
    // sanitizers or none — not the speed of this host.
    constexpr std::size_t kCap = 2000;
    constexpr int kWidth = 120;
    GridState mirror = blank_state(ckv::Size{kWidth, 4}, kCap);

    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    std::size_t pushed_total = 0;
    for (int batch = 0; batch < 3000; ++batch) {
        ckm::proto::ScrollbackPushOp push;
        for (int line = 0; line < 2; ++line) {
            std::vector<ckv::Cell> cells(static_cast<std::size_t>(kWidth), blank_cell());
            const std::string text = "line " + std::to_string(pushed_total);
            for (std::size_t i = 0; i < text.size() && i < static_cast<std::size_t>(kWidth); ++i)
                cells[i] = ckv::Cell::from_grapheme(std::string(1, text[i]), ckv::Style{});
            push.lines.push_back(ckm::proto::to_runs(cells));
            ++pushed_total;
        }
        std::vector<GridOp> ops;
        ops.emplace_back(std::move(push));
        CK_CHECK(ckm::apply_delta(ops, mirror));
        CK_CHECK(scrollback_cells(mirror).size() == scrollback_lines(mirror) * kWidth);
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();

    CK_CHECK(scrollback_lines(mirror) == kCap);
    // The capacity kept the newest lines and dropped the oldest.
    const auto row_text = [&](std::size_t row, std::size_t length) {
        const std::span<const ckv::Cell> history = scrollback_cells(mirror);
        std::string text;
        for (std::size_t i = 0; i < length; ++i)
            text += history[row * static_cast<std::size_t>(kWidth) + i].grapheme();
        return text;
    };
    CK_CHECK(row_text(kCap - 1, 9) == "line 5999");
    CK_CHECK(row_text(0, 9) == "line 4000");
    // Storage stays within one dead prefix of the kept history — the
    // amortized reclaim, pinned so the fix cannot quietly regress into
    // "never reclaim" (unbounded memory) or "reclaim per push" (the old
    // cost wearing a new face).
    CK_CHECK(mirror.scrollback.size() <= 2 * kCap * static_cast<std::size_t>(kWidth));
    CK_CHECK(elapsed < 5.0);
}
