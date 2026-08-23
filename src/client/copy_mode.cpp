// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/copy_mode.hpp"

#include <algorithm>
#include <limits>
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

// ASCII case folding, and deliberately only ASCII. `std::tolower` answers by
// locale, and a search whose results depend on the reader's `LANG` is not one
// feature but as many as there are locales (the engineering standard determinism). Bytes over
// 127 are left alone, which for UTF-8 text means a search over it is exact
// rather than wrong.
char folded(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool has_upper(std::string_view text) {
    for (const char c : text)
        if (c >= 'A' && c <= 'Z') return true;
    return false;
}

// One line as searchable text, plus the column each byte of it came from.
//
// A match is reported as a COLUMN, because that is what the cursor is indexed
// by and what a selection is defined in. A byte offset is the same number only
// while every grapheme is one byte wide, and the first box-drawing character in
// a line of program output ends that: the cursor landed inside the character
// before the match, and a selection started there copied half of it.
struct SearchableLine {
    std::string text;
    std::vector<int> columns;  // one entry per byte of `text`
};

SearchableLine searchable(const CopyLine& line, bool fold) {
    SearchableLine out;
    for (std::size_t column = 0; column < line.size(); ++column)
        for (const char byte : line[column]) {
            out.text.push_back(fold ? folded(byte) : byte);
            out.columns.push_back(static_cast<int>(column));
        }
    // The same trailing blanks `line_text` drops: a terminal pads every row out
    // to its width, and those spaces were never typed — a query ending in one
    // must not match the padding at the end of every line in the history.
    while (!out.text.empty() && out.text.back() == ' ') {
        out.text.pop_back();
        out.columns.pop_back();
    }
    return out;
}

// The first match starting in `[lowest, highest]` searching forwards, or the
// last one searching backwards. The window is what makes `from` exclusive
// within a line as well as between lines.
std::optional<int> match_in(const SearchableLine& line, const std::string& needle, bool forwards,
                            int lowest, int highest) {
    if (highest < lowest || needle.empty() || needle.size() > line.text.size())
        return std::nullopt;
    if (forwards) {
        for (std::size_t at = line.text.find(needle); at != std::string::npos;
             at = line.text.find(needle, at + 1)) {
            const int column = line.columns[at];
            if (column > highest) break;  // columns only grow along the line
            if (column >= lowest) return column;
        }
        return std::nullopt;
    }
    for (std::size_t at = line.text.rfind(needle); at != std::string::npos;) {
        const int column = line.columns[at];
        if (column < lowest) break;
        if (column <= highest) return column;
        if (at == 0) break;
        at = line.text.rfind(needle, at - 1);
    }
    return std::nullopt;
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
    const int width = snapshot.cells.width;
    // A terminal with no width is one nothing has sized yet — a mirror before
    // its first snapshot. Both buffers are rows of the terminal's OWN width, so
    // there is no second number to slice them at: a made-up width of 1 would
    // turn a history of ten thousand cells into ten thousand one-column lines
    // and open copy mode on a column of letters.
    if (width <= 0) return {CopyLine{std::string(" ")}};
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
    const int count = static_cast<int>(lines.size());
    // Where the search starts, clamped rather than trusted. This is a public
    // function over a document its caller composed, and a row outside that
    // document indexed the vector out of bounds — a crash bought by a cursor
    // that was merely stale, on a path with no other way to notice.
    const int row = std::clamp(from.y, 0, count - 1);
    const CopyLine& start_line = lines[static_cast<std::size_t>(row)];
    const int column = std::clamp(from.x, 0, static_cast<int>(start_line.size()));
    // Smartcase: an all-lower-case query means "I do not care", and a capital
    // anywhere in it means the reader typed one on purpose.
    const bool fold = !has_upper(query);
    std::string needle = query;
    if (fold)
        for (char& c : needle) c = folded(c);

    constexpr int kAnyColumn = std::numeric_limits<int>::max();
    const SearchableLine start = searchable(start_line, fold);
    // `from` is exclusive, and that has to hold WITHIN a line as well as
    // between them: a reader pressing `n` on a line with two matches in it was
    // sent a whole lap round the history to come back to the second one.
    if (const std::optional<int> hit = forwards
                                           ? match_in(start, needle, true, column + 1, kAnyColumn)
                                           : match_in(start, needle, false, 0, column - 1))
        return ckv::Point{*hit, row};
    // Every other line, in order, wrapping: a history is a loop to a reader
    // searching it.
    for (int step = 1; step < count; ++step) {
        const int at = forwards ? (row + step) % count : ((row - step) % count + count) % count;
        const SearchableLine line = searchable(lines[static_cast<std::size_t>(at)], fold);
        if (const std::optional<int> hit = match_in(line, needle, forwards, 0, kAnyColumn))
            return ckv::Point{*hit, at};
    }
    // The side of the starting line the search began past, searched last, so a
    // document with exactly one match in it does not answer "not found" to a
    // reader searching again from that match.
    if (const std::optional<int> hit = forwards ? match_in(start, needle, true, 0, column)
                                                : match_in(start, needle, false, column, kAnyColumn))
        return ckv::Point{*hit, row};
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
    if (finished_) return;
    std::string text = selected_text(lines_, mode_, anchor_, cursor_);
    // With nothing marked, yank takes the line the cursor is on. A reader who
    // pressed `y` meant to copy something, and refusing them the obvious
    // answer would make the key feel broken.
    if (text.empty() && mode_ == SelectionMode::None)
        text = selected_text(lines_, SelectionMode::Line, cursor_, cursor_);
    finished_ = true;
    // Both handlers are taken as values BEFORE either runs, because either may
    // destroy this view — `on_copy` reaches the client, which is free to leave
    // copy mode from inside it — and reading `on_exit` off a member afterwards
    // would be reading freed memory. Taking them also empties the members, so a
    // handler that comes back round here finds nothing left to call.
    std::function<void(std::string)> copy = std::move(on_copy);
    std::function<void()> exit = std::move(on_exit);
    if (copy) copy(std::move(text));
    if (exit) exit();
}

void CopyModeView::finish() {
    if (finished_) return;
    finished_ = true;
    // Moved out first, for the reason yank_and_exit states: leaving destroys
    // this view, and the member must not be touched once it may have.
    std::function<void()> exit = std::move(on_exit);
    if (exit) exit();
}

}  // namespace ckm::client
