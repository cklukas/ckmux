// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Copy mode: reading a terminal's history, selecting out of it, and searching
// it (the interface spec "Copy mode & clipboard", WP-17).
//
// It sits ABOVE the `TerminalSubsession` seam, over a snapshot and nothing
// else, which is why it is written the same way against M1's in-process
// terminals as against M2's mirrors: a snapshot is a value, and where it came
// from is not this file's business. Nothing here knows about a socket, and
// nothing changes when the terminal moves behind one.
//
// The history is **frozen** on entry. A program that keeps printing while its
// reader is reading does not move the text out from under the cursor, and a
// selection made at line 4 000 is still the same line when it is copied. That
// is a decision, not a simplification: the alternative is a document that
// re-indexes itself between the moment a reader marks something and the
// moment they yank it.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cvision/core/terminal_subsession.hpp"
#include "cvision/ui/standard_roles.hpp"
#include "cvision/ui/view.hpp"

namespace ckm::client {

// What a selection covers. The three modes exist because the three shapes a
// reader wants are genuinely different: a run of prose, a set of whole lines,
// and a column out of a table that a line-wise copy would ruin.
enum class SelectionMode : unsigned char { None, Character, Line, Rectangular };

// One line of history, a grapheme per column. Column-indexed rather than
// stored as text, because a rectangular selection is defined in columns and a
// wide character occupies two of them — joining to a string first would put
// the right edge of the block in a different place on every line that
// contains one. A continuation column holds an empty string.
using CopyLine = std::vector<std::string>;

// The scrollback followed by the screen, which is the document a reader means
// by "what this terminal has said".
std::vector<CopyLine> compose_history(const ckv::core::TerminalSnapshot& snapshot);

// The text a selection yields. Exposed because it is the part worth testing
// without a view: given a document and two corners, what lands on the
// clipboard. Trailing blanks go — a terminal line is a fixed number of cells,
// and the spaces to the right of the last character were never typed.
std::string selected_text(const std::vector<CopyLine>& lines, SelectionMode mode,
                          ckv::Point anchor, ckv::Point cursor);

// The bytes a paste puts on the wire. Bracketed only if the program asked
// for it (DEC mode 2004): wrapping text a program did not ask to have wrapped
// puts `[200~` on its screen, and not wrapping it for one that did lets an
// editor treat a pasted paragraph as a sequence of commands.
//
// A function rather than a step inside the paste, because this is the rule
// worth pinning and the delivery is not: `send_input` on a real subsession
// goes down a pty, where the only way to observe it is to wait for a child
// process to echo it back.
std::string paste_bytes(std::string_view text, bool bracketed);

// Where the next match is, or nothing. `from` is exclusive, so repeating a
// search moves. Case-insensitive while the query is all lower case
// (a lower-case query means "I do not care"; typing a capital means "I do").
std::optional<ckv::Point> find_match(const std::vector<CopyLine>& lines, const std::string& query,
                                     ckv::Point from, bool forwards);

class CopyModeView final : public ckv::ui::View {
public:
    CopyModeView(std::vector<CopyLine> lines, int cursor_row, const ckv::ui::StandardRoles& roles);

    // Yanked text, on its way to the clipboard targets. Runs before on_exit.
    std::function<void(std::string)> on_copy;
    // Copy mode is over, by `q`, `Esc`, or a completed yank.
    std::function<void()> on_exit;

    // --- what the window caption and the footer show, and what a test asserts
    ckv::Point cursor() const noexcept { return cursor_; }
    SelectionMode selection_mode() const noexcept { return mode_; }
    // 0 at the oldest line of history. Also the "-1024/10000" in the caption.
    int scroll_position() const noexcept { return cursor_.y; }
    int history_size() const noexcept { return static_cast<int>(lines_.size()); }
    bool searching() const noexcept { return search_prompt_.has_value(); }
    const std::string& query() const noexcept { return query_; }
    // The status line's own text, so a test asserts on what a reader sees
    // rather than on a private field.
    std::string status_text() const;

    void draw(ckv::scene::Painter& painter) override;
    bool on_key(const ckv::KeyEvent& event) override;
    void on_resized() override;

private:
    int visible_rows() const noexcept;
    void move_cursor(int rows, int columns);
    void scroll_into_view();
    void begin_selection(SelectionMode mode);
    void yank_and_exit();
    void finish();
    // Runs `query_` from just past the cursor and moves there if it hits.
    bool search(bool forwards);

    std::vector<CopyLine> lines_;
    ckv::Point cursor_{};
    ckv::Point anchor_{};
    SelectionMode mode_ = SelectionMode::None;
    int top_ = 0;  // the first line of history on screen
    // The half-typed search, present only while the prompt is open. Separate
    // from `query_` so that abandoning a search with Esc leaves the previous
    // one intact for `n` — which is what a reader who changed their mind
    // meant.
    std::optional<std::string> search_prompt_;
    bool search_forwards_ = true;
    std::string query_;
    // Said once, under the status line, when a search runs off the end. Not a
    // dialog: a failed search is not an error, it is an answer.
    std::string notice_;
    bool finished_ = false;
    ckv::ui::RoleId frame_role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId text_role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId selected_role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId status_role_ = ckv::ui::kInvalidRole;
};

}  // namespace ckm::client
