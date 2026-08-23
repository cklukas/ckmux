// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/prefix_overlay.hpp"

#include <algorithm>
#include <utility>

#include "cvision/core/text.hpp"
#include "cvision/scene/painter.hpp"

namespace ckm::client {
namespace {

constexpr int kColumns = 2;
constexpr int kColumnGap = 3;

}  // namespace

PrefixOverlay::PrefixOverlay(ckv::KeyChord prefix,
                             std::vector<std::pair<std::string, std::string>> rows,
                             const ckv::ui::StandardRoles& roles)
    : prefix_(std::move(prefix)),
      frame_role_(roles.dialog_frame),
      text_role_(roles.static_text),
      key_role_(roles.hotkey) {
    rows_.reserve(rows.size());
    for (std::pair<std::string, std::string>& row : rows)
        rows_.push_back(Row{std::move(row.first), std::move(row.second)});
    // Focusable because holding the keyboard is this view's whole purpose.
    set_focus_policy(ckv::ui::FocusPolicy::TabStop);
    set_help_context_key("ckmux.prefix");
}

ckv::Rect PrefixOverlay::preferred_bounds(ckv::Rect available) const {
    // Collapsed the overlay shows nothing at all: an empty rect paints no
    // cells, so the desktop underneath is untouched while ckmux quietly holds
    // the next keystroke.
    if (!expanded_) return ckv::Rect{};

    const std::vector<Row>& table = rows();
    int key_width = 0;
    int hint_width = 0;
    for (const Row& row : table) {
        key_width = std::max(key_width, ckv::text::text_width(row.key));
        hint_width = std::max(hint_width, ckv::text::text_width(row.hint));
    }
    const int cell_width = key_width + 1 + hint_width;
    const int rows_per_column =
        (static_cast<int>(table.size()) + kColumns - 1) / std::max(1, kColumns);

    // Two frame columns, one padding column each side, then the grid.
    const int width = std::min(available.width, 4 + kColumns * cell_width + (kColumns - 1) * kColumnGap);
    const int height = std::min(available.height, rows_per_column + 3);  // frame + title row
    const int x = available.x + std::max(0, (available.width - width) / 2);
    const int y = available.y + std::max(0, (available.height - height) / 2);
    return ckv::Rect{x, y, width, height};
}

void PrefixOverlay::expand() {
    if (expanded_ || finished_) return;
    expanded_ = true;
    invalidate();
}

void PrefixOverlay::draw(ckv::scene::Painter& painter) {
    if (!expanded_) return;
    const ckv::Rect box{0, 0, bounds().width, bounds().height};
    if (box.width <= 2 || box.height <= 2) return;

    const ckv::ui::Theme& theme = *context().theme;
    const ckv::Style frame = theme.resolve(frame_role_);
    const ckv::Style text = theme.resolve(text_role_);
    const ckv::Style key_role_style = theme.resolve(key_role_);
    // A key keeps the panel's own background and contributes only its accent
    // foreground, the same composition rule ckVision's own chrome controls
    // follow.
    const ckv::Style key{key_role_style.fg, frame.bg, frame.attrs | key_role_style.attrs};

    painter.fill(box, ckv::Cell::from_grapheme(" ", frame));
    painter.draw_box(box, ckv::scene::LineStyle::Single, frame);
    const std::string title = " " + prefix_label(prefix_) + " … ";
    painter.draw_text(ckv::Point{2, 0}, title, key);

    const std::vector<Row>& table = rows();
    int key_width = 0;
    for (const Row& row : table) key_width = std::max(key_width, ckv::text::text_width(row.key));
    const int rows_per_column =
        (static_cast<int>(table.size()) + kColumns - 1) / std::max(1, kColumns);
    const int cell_width = (box.width - 4 - (kColumns - 1) * kColumnGap) / std::max(1, kColumns);

    for (std::size_t index = 0; index < table.size(); ++index) {
        const int column = static_cast<int>(index) / std::max(1, rows_per_column);
        const int row_index = static_cast<int>(index) % std::max(1, rows_per_column);
        const int x = 2 + column * (cell_width + kColumnGap);
        const int y = 2 + row_index;
        if (y >= box.height - 1 || x >= box.width - 1) continue;
        painter.draw_text(ckv::Point{x, y}, table[index].key, key);
        const int hint_x = x + key_width + 1;
        if (hint_x < box.width - 1)
            painter.draw_text(ckv::Point{hint_x, y},
                              ckv::text::elide_to_width(table[index].hint, box.width - 1 - hint_x), text);
    }
}

bool PrefixOverlay::on_key(const ckv::KeyEvent& event) {
    if (event.action == ckv::KeyAction::Release) return false;
    // Every key ends the prefix state — that is what "the next key" means.
    // Which key it was is ClientApp's decision to interpret; an unknown one
    // simply cancels rather than reaching the child program, because the
    // reader's intent was to address ckmux.
    std::string chord = chord_spelling(event);
    if (event.chord.key == ckv::Key::Escape) chord.clear();
    finish(std::move(chord));
    return true;
}

void PrefixOverlay::finish(std::string chord) {
    if (finished_) return;
    finished_ = true;
    // Taken as a value before it runs, not called through the member: the
    // callback's whole job is to end this overlay, and a `std::function` that
    // destroys the object holding it is destroying itself mid-call. Moving it
    // out first also empties the member, so a second chord finds nothing to
    // call rather than a handler that has already had its say.
    std::function<void(std::string)> chosen = std::move(on_chord);
    if (chosen) chosen(std::move(chord));
}

}  // namespace ckm::client
