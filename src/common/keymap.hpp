// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The vocabulary a key binding is written in: what an action is called, what
// a context is called, and how a chord is spelled (the configuration spec).
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
// (the interface spec), and the list is deliberately exactly the set of
// actions ckmux can carry out today: an action that parsed and then did
// nothing would be the Window List defect again — a live-looking binding with
// no handler behind it (the work queue, the reachability rule). The list grows with
// the packages that add the actions, never ahead of them.
enum class Action : unsigned char {
    NewTerminal,
    CloseTerminal,
    // Appended at the END of the enum rather than beside CloseTerminal, so no
    // existing action's value moves: these are serialised nowhere today, and
    // the day one is, a reordering would silently rebind every reader's
    // keymap. Cheaper to keep the rule than to remember when it starts to
    // matter.
    MoveTerminal,
    // Naming a terminal is not the same as moving it, and it is a different
    // verb from renaming a SESSION: one pins a window's caption, the other
    // labels the thing the session picker lists. Two names because they are
    // two commands, and a reader writes whichever they meant.
    RenameTerminal,
    // What a program in this terminal printed, and what to do with it. A
    // command rather than only a frame button, because a reader whose window
    // is small enough to have dropped the button still has to be able to reach
    // their captures (the interface spec rule 2: everything is in the menu).
    PrintOutput,
    PrinterSettings,
    NextTerminal,
    PreviousTerminal,
    WindowList,
    Zoom,
    // Putting a terminal away, which is ckVision's own `ckv.window.minimize`
    // wearing a ckmux chord (WP-34) — the same verb the window's own `_`
    // control offers, reached from the keyboard and the menu for the window
    // the reader is already IN and therefore cannot easily aim at.
    Minimize,
    MoveResize,
    // Three named tilings and no unqualified `tile`. ckVision's own
    // `ckv.window.tile` produces exactly what tile_vertically produces
    // (U4-b, checked against `Desktop::tile()`), so a ckmux surface offering
    // both would be offering one arrangement twice under two names — and a
    // reader who bound one of them would have no way to tell which they got.
    TileHorizontally,
    TileVertically,
    TileGrid,
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
    // Hiding the footer, at which point the window bar is on the last row
    // (WP-35). A row of chrome the reader can spend on their program
    // instead — and a command as well as the bar's own ▼, because the bar
    // is not always on screen and the way back must not be either.
    ToggleStatusBar,
    // Making the SESSION's desktop this screen's size (WP-40). A reader's own
    // window changing size never does this — the session's coordinate space is
    // shared with whoever else is watching, and reflowing it SIGWINCHes every
    // child in the session, so it is asked for rather than inferred.
    FitDesktop,
    // The View menu's three readouts (WP-39): what the processes under each
    // terminal cost, written onto every terminal window's frame footer. Three
    // actions because they are three independent checkboxes, each persisted
    // as its own `[general]` key — and rebindable like everything else,
    // though none carries a default chord: a display toggle is not a
    // muscle-memory operation, and the prefix table is a scarce resource.
    ShowCpuUsage,
    ShowMemoryRss,
    ShowMemoryReal,
    About,
    SendPrefix,
    Quit,
    // The session model's `kill-terminal`. Last, per the note at the top of the enum.
    KillTerminal,
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

// The chord grammar of the configuration spec: `^X` or `C-x` (Ctrl), `S-x` (Shift),
// `A-x` / `M-x` (Alt), `F1`–`F12`, named keys (`Enter`, `Esc`, `Space`,
// `Up`, …), or one printable character standing for itself.
//
// Shift is only ever written for a named key. On a printable character the
// case *is* the shift — `T` is Shift+t, and `S-t` would be a second way to
// spell the same key, which is one way too many for a table that has to look
// bindings up by their spelling. Ctrl is the exception: a terminal sends one
// byte for Ctrl+b and Ctrl+Shift+b alike, so `C-B` folds to `C-b` rather than
// parsing into a chord nobody can press.
//
// `^X` and `C-x` are both accepted and are the same chord. Two spellings in,
// one spelling out — see chord_spelling.
std::optional<ckv::KeyChord> parse_chord(std::string_view text);

// The inverse, and the single spelling authority: a chord dispatches under
// exactly the string this returns, and every surface that shows a key shows
// this string. Two spellings would mean a binding that dispatches and a
// binding the reader was shown could differ.
//
// Ctrl on a printable character comes out as `^B` — what the interface spec writes, and
// what a terminal's reader expects to see in a one-line footer. Everything
// else keeps `C-`/`A-`/`S-`. Nothing outside this function is allowed a caret
// rule of its own.
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
