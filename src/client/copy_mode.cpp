// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "client/copy_mode.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "cvision/core/text.hpp"
#include "cvision/scene/painter.hpp"

namespace ckm::client {
namespace {

// A line's text, without the blanks a terminal pads every row with. Those
// spaces are how a grid stores an empty cell, not something anybody typed,
// and a clipboard full of them is a clipboard nobody can paste.
std::string line_text(const CopyLine& line, int from, int to) {
    std::string out;
    for (int column = std::max(0, from); column < std::min<int>(to, static_cast<int>(line.size())); ++column)
        out += line[static_cast<std::size_t>(column)];
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has_upper(std::string_view text) {
    for (const char c : text)
        if (std::isupper(static_cast<unsigned char>(c)) != 0) return true;
    return false;
}

// Ordered corners, so every caller below reads top-left to bottom-right and
// none of them has to care which way the reader dragged.
std::pair<ckv::Point, ckv::Point> ordered(ckv::Point a, ckv::Point b) {
    if (a.y > b.y || (a.y == b.y && a.x > b.x)) std::swap(a, b);
    return {a, b};
}

}  // namespace

std::vector<CopyLine> compose_history(const ckv::core::TerminalSnapshot& snapshot) {
    std::vector<CopyLine> lines;
    const int width = std::max(1, snapshot.cells.width);
    const auto append = [&lines, width](const std::vector<ckv::Cell>& cells, std::size_t begin,
                                        std::size_t end) {
        for (std::size_t row = begin; row + static_cast<std::size_t>(width) <= end;
             row += static_cast<std::size_t>(width)) {
            CopyLine line;
            line.reserve(static_cast<std::size_t>(width));
            for (int column = 0; column < width; ++column) {
                const ckv::Cell& cell = cells[row + static_cast<std::size_t>(column)];
                // A continuation column belongs to the character before it and
                // must not be copied twice.
                line.push_back(cell.is_continuation() ? std::string{} : std::string(cell.grapheme()));
            }
            lines.push_back(std::move(line));
        }
    };
    append(snapshot.scrollback, 0, snapshot.scrollback.size());
    append(snapshot.cell_buffer, 0, snapshot.cell_buffer.size());
    if (lines.empty()) lines.push_back(CopyLine(static_cast<std::size_t>(width), " "));
    return lines;
}

std::string selected_text(const std::vector<CopyLine>& lines, SelectionMode mode, ckv::Point anchor,
                          ckv::Point cursor) {
    if (lines.empty() || mode == SelectionMode::None) return {};
    const auto [first, last] = ordered(anchor, cursor);
    const int top = std::clamp(first.y, 0, static_cast<int>(lines.size()) - 1);
    const int bottom = std::clamp(last.y, 0, static_cast<int>(lines.size()) - 1);

    std::string out;
    for (int row = top; row <= bottom; ++row) {
        const CopyLine& line = lines[static_cast<std::size_t>(row)];
        const int width = static_cast<int>(line.size());
        int from = 0;
        int to = width;
        if (mode == SelectionMode::Character) {
            if (row == first.y) from = first.x;
            // Inclusive of the cursor's own column: a reader who marked one
            // character expects one character, not none.
            if (row == last.y) to = std::min(width, last.x + 1);
        } else if (mode == SelectionMode::Rectangular) {
            from = std::min(anchor.x, cursor.x);
            to = std::min(width, std::max(anchor.x, cursor.x) + 1);
        }
        if (row > top) out += '\n';
        out += line_text(line, from, to);
    }
    return out;
}

std::string paste_bytes(std::string_view text, bool bracketed) {
    if (text.empty()) return {};
    if (!bracketed) return std::string(text);
    return "\x1b[200~" + std::string(text) + "\x1b[201~";
}

std::optional<ckv::Point> find_match(const std::vector<CopyLine>& lines, const std::string& query,
                                     ckv::Point from, bool forwards) {
    if (query.empty() || lines.empty()) return std::nullopt;
    // Smartcase: an all-lower-case query means "I do not care", and a capital
    // anywhere in it means the reader typed one on purpose.
    const bool sensitive = has_upper(query);
    const std::string needle = sensitive ? query : lowered(query);

    const int count = static_cast<int>(lines.size());
    for (int step = 1; step <= count; ++step) {
        const int row = forwards ? (from.y + step) % count : ((from.y - step) % count + count) % count;
        const std::string text =
            line_text(lines[static_cast<std::size_t>(row)], 0,
                      static_cast<int>(lines[static_cast<std::size_t>(row)].size()));
        const std::string haystack = sensitive ? text : lowered(text);
        const std::size_t hit = forwards ? haystack.find(needle) : haystack.rfind(needle);
        if (hit == std::string::npos) continue;
        // The column is a byte offset into the joined text; a line of ASCII —
        // which is what a search hits in practice — makes the two the same,
        // and a wide character before the match only shifts the cursor, never
        // the line it lands on.
        return ckv::Point{static_cast<int>(hit), row};
    }
    // The line the cursor is already on is searched last, so a repeated search
    // in a document with one match does not report "not found".
    const std::string text = line_text(lines[static_cast<std::size_t>(std::clamp(from.y, 0, count - 1))], 0,
                                       static_cast<int>(lines[static_cast<std::size_t>(
                                           std::clamp(from.y, 0, count - 1))].size()));
    const std::string haystack = sensitive ? text : lowered(text);
    if (haystack.find(needle) != std::string::npos)
        return ckv::Point{static_cast<int>(haystack.find(needle)), std::clamp(from.y, 0, count - 1)};
    return std::nullopt;
}

CopyModeView::CopyModeView(std::vector<CopyLine> lines, int cursor_row,
                           const ckv::ui::StandardRoles& roles)
    : lines_(std::move(lines)),
      frame_role_(roles.dialog_frame),
      text_role_(roles.static_text),
      selected_role_(roles.list_selected),
      status_role_(roles.hotkey) {
    if (lines_.empty()) lines_.push_back(CopyLine{});
    cursor_.y = std::clamp(cursor_row, 0, static_cast<int>(lines_.size()) - 1);
    anchor_ = cursor_;
    top_ = cursor_.y;
    set_focus_policy(ckv::ui::FocusPolicy::TabStop);
    set_help_context_key("ckmux.copy-mode");
}

int CopyModeView::visible_rows() const noexcept {
    // One row goes to the status line, which is where the position, the
    // selection mode and the search live.
    return std::max(1, bounds().height - 1);
}

void CopyModeView::on_resized() { scroll_into_view(); }

void CopyModeView::scroll_into_view() {
    const int rows = visible_rows();
    top_ = std::clamp(top_, 0, std::max(0, static_cast<int>(lines_.size()) - 1));
    if (cursor_.y < top_) top_ = cursor_.y;
    if (cursor_.y >= top_ + rows) top_ = cursor_.y - rows + 1;
    top_ = std::max(0, top_);
    invalidate();
}

void CopyModeView::move_cursor(int rows, int columns) {
    cursor_.y = std::clamp(cursor_.y + rows, 0, static_cast<int>(lines_.size()) - 1);
    const int width = static_cast<int>(lines_[static_cast<std::size_t>(cursor_.y)].size());
    cursor_.x = std::clamp(cursor_.x + columns, 0, std::max(0, width - 1));
    scroll_into_view();
}

void CopyModeView::begin_selection(SelectionMode mode) {
    // Pressing the same key again is how a reader takes it back — otherwise
    // the only way out of a selection they did not mean is to leave copy mode
    // and lose their place.
    if (mode_ == mode) {
        mode_ = SelectionMode::None;
    } else {
        mode_ = mode;
        anchor_ = cursor_;
    }
    invalidate();
}

std::string CopyModeView::status_text() const {
    std::string status = "COPY";
    // Where in the history the reader is, counted the way a terminal reader
    // thinks of it: 0 is the bottom, and negative is how far back.
    const int from_bottom = cursor_.y - (static_cast<int>(lines_.size()) - 1);
    status += "  " + std::to_string(from_bottom) + "/" + std::to_string(lines_.size());
    switch (mode_) {
        case SelectionMode::None: break;
        case SelectionMode::Character: status += "  [select]"; break;
        case SelectionMode::Line: status += "  [line]"; break;
        case SelectionMode::Rectangular: status += "  [block]"; break;
    }
    if (search_prompt_) status += std::string("  ") + (search_forwards_ ? "/" : "?") + *search_prompt_;
    else if (!query_.empty()) status += "  /" + query_;
    if (!notice_.empty()) status += "  " + notice_;
    return status;
}

void CopyModeView::draw(ckv::scene::Painter& painter) {
    const ckv::Rect box{0, 0, bounds().width, bounds().height};
    if (box.width <= 0 || box.height <= 0) return;
    const ckv::ui::Theme& theme = *context().theme;
    const ckv::Style text = theme.resolve(text_role_);
    const ckv::Style selected = theme.resolve(selected_role_);
    const ckv::Style status_style = theme.resolve(status_role_);
    const ckv::Style frame = theme.resolve(frame_role_);

    painter.fill(box, ckv::Cell::from_grapheme(" ", text));

    const auto [first, last] = ordered(anchor_, cursor_);
    const int rows = visible_rows();
    for (int row = 0; row < rows; ++row) {
        const int index = top_ + row;
        if (index >= static_cast<int>(lines_.size())) break;
        const CopyLine& line = lines_[static_cast<std::size_t>(index)];
        for (int column = 0; column < box.width && column < static_cast<int>(line.size()); ++column) {
            const std::string& grapheme = line[static_cast<std::size_t>(column)];
            if (grapheme.empty()) continue;  // the far half of a wide character
            bool inside = false;
            if (mode_ != SelectionMode::None && index >= first.y && index <= last.y) {
                if (mode_ == SelectionMode::Line) {
                    inside = true;
                } else if (mode_ == SelectionMode::Rectangular) {
                    inside = column >= std::min(anchor_.x, cursor_.x) &&
                             column <= std::max(anchor_.x, cursor_.x);
                } else {
                    const bool after_start = index > first.y || column >= first.x;
                    const bool before_end = index < last.y || column <= last.x;
                    inside = after_start && before_end;
                }
            }
            // The cursor is a cell, not a caret: this view holds the keyboard
            // and shows no terminal cursor of its own, so the reader has to be
            // able to see where they are.
            const bool at_cursor = index == cursor_.y && column == cursor_.x;
            painter.draw_text(ckv::Point{column, row}, grapheme,
                              (inside || at_cursor) ? selected : text);
        }
    }

    const std::string status = ckv::text::elide_to_width(status_text(), box.width);
    painter.fill(ckv::Rect{0, box.height - 1, box.width, 1}, ckv::Cell::from_grapheme(" ", frame));
    painter.draw_text(ckv::Point{0, box.height - 1}, status, status_style);
}

bool CopyModeView::search(bool forwards) {
    notice_.clear();
    const std::optional<ckv::Point> hit = find_match(lines_, query_, cursor_, forwards);
    if (!hit) {
        notice_ = "not found";
        invalidate();
        return false;
    }
    cursor_ = *hit;
    scroll_into_view();
    return true;
}

bool CopyModeView::on_key(const ckv::KeyEvent& event) {
    if (event.action == ckv::KeyAction::Release) return false;
    const ckv::KeyChord& chord = event.chord;
    const bool ctrl = has_modifier(chord.modifiers, ckv::Modifier::Ctrl);
    const std::string& character = chord.text;

    // While the prompt is open every key belongs to it: a reader typing a
    // search for "q" must not leave copy mode instead.
    if (search_prompt_) {
        if (chord.key == ckv::Key::Escape) {
            search_prompt_.reset();
            invalidate();
            return true;
        }
        if (chord.key == ckv::Key::Enter) {
            query_ = *search_prompt_;
            search_prompt_.reset();
            search(search_forwards_);
            return true;
        }
        if (chord.key == ckv::Key::Backspace) {
            if (!search_prompt_->empty()) search_prompt_->pop_back();
            invalidate();
            return true;
        }
        if (chord.key == ckv::Key::Char && !character.empty() && !ctrl) {
            *search_prompt_ += character;
            invalidate();
            return true;
        }
        return true;
    }

    notice_.clear();
    switch (chord.key) {
        case ckv::Key::Escape: finish(); return true;
        case ckv::Key::Enter: yank_and_exit(); return true;
        case ckv::Key::Up: move_cursor(-1, 0); return true;
        case ckv::Key::Down: move_cursor(1, 0); return true;
        case ckv::Key::Left: move_cursor(0, -1); return true;
        case ckv::Key::Right: move_cursor(0, 1); return true;
        case ckv::Key::PageUp: move_cursor(-visible_rows(), 0); return true;
        case ckv::Key::PageDown: move_cursor(visible_rows(), 0); return true;
        case ckv::Key::Home: cursor_.x = 0; scroll_into_view(); return true;
        case ckv::Key::End:
            cursor_.x = std::max(0, static_cast<int>(lines_[static_cast<std::size_t>(cursor_.y)].size()) - 1);
            scroll_into_view();
            return true;
        default: break;
    }
    if (chord.key != ckv::Key::Char || character.empty()) return true;

    if (ctrl) {
        if (character == "v") begin_selection(SelectionMode::Rectangular);
        else if (character == "b") move_cursor(-visible_rows(), 0);
        else if (character == "f") move_cursor(visible_rows(), 0);
        return true;
    }

    // vi keys, because the readers this is for already have them in their
    // fingers; the arrow keys do the same things for the readers who do not.
    if (character == "h") move_cursor(0, -1);
    else if (character == "l") move_cursor(0, 1);
    else if (character == "k") move_cursor(-1, 0);
    else if (character == "j") move_cursor(1, 0);
    else if (character == "0") { cursor_.x = 0; scroll_into_view(); }
    else if (character == "$")
        move_cursor(0, static_cast<int>(lines_[static_cast<std::size_t>(cursor_.y)].size()));
    else if (character == "g") { cursor_ = ckv::Point{0, 0}; scroll_into_view(); }
    else if (character == "G") { cursor_ = ckv::Point{0, static_cast<int>(lines_.size()) - 1}; scroll_into_view(); }
    else if (character == "v") begin_selection(SelectionMode::Character);
    else if (character == "V") begin_selection(SelectionMode::Line);
    else if (character == "y") yank_and_exit();
    else if (character == "q") finish();
    else if (character == "/" || character == "?") {
        search_forwards_ = character == "/";
        search_prompt_ = std::string{};
        invalidate();
    } else if (character == "n") search(search_forwards_);
    else if (character == "N") search(!search_forwards_);
    // Everything else is swallowed rather than passed on: copy mode holds the
    // keyboard, and a stray key reaching the child would type into whatever
    // program the reader is reading the output of.
    return true;
}

void CopyModeView::yank_and_exit() {
    std::string text = selected_text(lines_, mode_, anchor_, cursor_);
    // With nothing marked, yank takes the line the cursor is on. A reader who
    // pressed `y` meant to copy something, and refusing them the obvious
    // answer would make the key feel broken.
    if (text.empty() && mode_ == SelectionMode::None)
        text = selected_text(lines_, SelectionMode::Line, cursor_, cursor_);
    if (finished_) return;
    finished_ = true;
    // Both callbacks may detach this view, so nothing touches members after
    // the first one runs.
    if (on_copy) on_copy(std::move(text));
    if (on_exit) on_exit();
}

void CopyModeView::finish() {
    if (finished_) return;
    finished_ = true;
    if (on_exit) on_exit();
}

}  // namespace ckm::client
