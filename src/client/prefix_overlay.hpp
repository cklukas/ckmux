// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The prefix capture surface and its which-key popup
// (the interface spec "The prefix & which-key popup").
//
// It exists for two reasons at once. The first is mechanical: once the prefix
// is armed, ckmux — not the focused child program — owns the very next key,
// and taking focus is how a ckVision application says that. The second is the
// product thesis: after a short pause it shows the reader every key that is
// now one press away, so the chord table is discoverable without being
// documented. The pause is what keeps that from becoming noise for someone
// who already knows the key they want.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "client/commands.hpp"
#include "cvision/ui/standard_roles.hpp"
#include "cvision/ui/theme.hpp"
#include "cvision/ui/view.hpp"

namespace ckm::client {

class PrefixOverlay final : public ckv::ui::View {
public:
    // `prefix` is only ever used for display here; matching it is the
    // terminal view's job (its parent-escape chord) and ClientApp's.
    //
    // The roles arrive already interned rather than being looked up by name
    // on attach. A library widget has no choice but the name lookup — it
    // cannot see the application's role struct — but this view can, and a
    // mistyped role name resolves to an invalid id that only fails when it
    // is finally painted.
    // `rows` is the key table as it stands right now, copied rather than
    // referenced: the popup can outlive the client that made it (the desktop
    // owns it, and a destroyed client leaves its keymap behind), and a
    // rebinding cannot change under an overlay that is already on screen.
    PrefixOverlay(ckv::KeyChord prefix, std::vector<std::pair<std::string, std::string>> rows,
                  const ckv::ui::StandardRoles& roles);

    // Runs with the chord spelling of the key that ended the prefix state, or
    // an empty string when it was cancelled (Esc, or a click elsewhere).
    // Fires exactly once.
    std::function<void(std::string)> on_chord;

    // Shows the full key table. Called from a timer so a reader who is
    // already typing a known chord never sees the popup at all.
    void expand();
    bool expanded() const noexcept { return expanded_; }

    // The rect this overlay wants inside `available`, sized to its content.
    // Collapsed, that is deliberately empty: the overlay is holding the
    // keyboard, not showing anything, and the footer is already saying so.
    ckv::Rect preferred_bounds(ckv::Rect available) const;

    void draw(ckv::scene::Painter& painter) override;
    bool on_key(const ckv::KeyEvent& event) override;

private:
    struct Row {
        std::string key;
        std::string hint;
    };
    const std::vector<Row>& rows() const noexcept { return rows_; }
    void finish(std::string chord);

    ckv::KeyChord prefix_;
    std::vector<Row> rows_;
    bool expanded_ = false;
    bool finished_ = false;
    ckv::ui::RoleId frame_role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId text_role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId key_role_ = ckv::ui::kInvalidRole;
};

}  // namespace ckm::client
