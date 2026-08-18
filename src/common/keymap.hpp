// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The vocabulary a key binding is written in: what an action is called, what
// a context is called, and how a chord is spelled (plans/08-configuration.md).
//
// It lives in `common` rather than beside the client's keymap table because
// three readers need it and only one of them has a user interface. The client
// dispatches with it; `check-config` validates a file with it without opening
// a window; and the config parser has to reject `bind termianl d detach` at
// the line it appears on, which it cannot do with strings it does not
// understand. Everything here is names and spellings — nothing knows what a
// command does, and nothing includes ckVision's `ui` layer.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cvision/core/key.hpp"

namespace ckm {

// What a binding may be bound to. These are the registry names
// (plans/06-ui-design.md), and the list is deliberately exactly the set of
// actions ckmux can carry out today: an action that parsed and then did
// nothing would be the Window List defect again — a live-looking binding with
// no handler behind it (plans/12, the reachability rule). The list grows with
// the packages that add the actions, never ahead of them.
enum class Action : unsigned char {
    NewTerminal,
    CloseTerminal,
    MoveTerminal,
    NextTerminal,
    PreviousTerminal,
    WindowList,
    Zoom,
    MoveResize,
    Tile,
    Cascade,
    MenuBar,
    CopyMode,
    Paste,
    Detach,
    Sessions,
    NewSession,
    RenameSession,
    KillSession,
    KeyReference,
    Settings,
    About,
    SendPrefix,
    Quit,
};

// Where a binding applies. `terminal` is the key *after* the prefix, which is
// every binding ckmux has today; the others bind direct keys in surfaces that
// arrive with their packages. They are named here so a reader who writes one
// ahead of time is told it is not honoured yet rather than told it is a typo —
// and told, rather than silently obeyed-and-ignored.
enum class KeyContext : unsigned char {
    Terminal,
    Desktop,
    CopyMode,
    Picker,
    MoveResize,
};

// Every action name, in the order the key reference lists them. Used to
// answer "what are the valid options?" when a file names something else.
std::span<const std::pair<std::string_view, Action>> action_names();
std::span<const std::pair<std::string_view, KeyContext>> key_context_names();

std::string_view action_name(Action action);
std::string_view key_context_name(KeyContext context);
std::optional<Action> parse_action(std::string_view name);
std::optional<KeyContext> parse_key_context(std::string_view name);

// A comma-separated list of every valid spelling, for a message that has to
// tell a reader what they could have written instead.
std::string action_name_list();
std::string key_context_name_list();

// The chord grammar of plans/08: `C-x` (Ctrl), `S-x` (Shift), `A-x` / `M-x`
// (Alt), `F1`–`F12`, named keys (`Enter`, `Esc`, `Space`, `Up`, …), or one
// printable character standing for itself.
//
// Shift is only ever written for a named key. On a printable character the
// case *is* the shift — `T` is Shift+t, and `S-t` would be a second way to
// spell the same key, which is one way too many for a table that has to look
// bindings up by their spelling.
std::optional<ckv::KeyChord> parse_chord(std::string_view text);

// The inverse, and the single spelling authority: a chord dispatches under
// exactly the string this returns, and every surface that shows a key shows
// this string. Two spellings would mean a binding that dispatches and a
// binding the reader was shown could differ.
std::string chord_spelling(const ckv::KeyChord& chord);

// One `bind` or `unbind` line, in file order. `action` is empty for `unbind`,
// which says only that the chord stops doing whatever it did.
struct BindDirective {
    KeyContext context = KeyContext::Terminal;
    std::string chord;
    std::optional<Action> action;

    friend bool operator==(const BindDirective&, const BindDirective&) = default;
};

}  // namespace ckm
