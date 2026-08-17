// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The ckmux keymap registry: one table that drives every surface which
// mentions a key (plans/06-ui-design.md "Keymap registry"). Runtime dispatch,
// the footer hints, the which-key popup, and the help pages all read this
// table, so a rebinding can never leave one of them telling the reader
// something the others do not do.
//
// It complements — never duplicates — ckVision's CommandRegistry, which owns
// what a command IS (id, title, handler, enablement). This table owns how a
// command is REACHED through the ckmux prefix, and how it is advertised.
// Commands whose only route is a menu still appear here with an empty chord,
// because the help pages list them too.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cvision/core/event.hpp"
#include "cvision/ui/command.hpp"

namespace ckm::client {

namespace commands {

// ckVision reserves every id below kFirstApplicationCommandId for its own
// standard set (Quit, Zoom, NextWindow, Tile, WindowList, Menu, Help, ...).
// ckmux references those directly rather than defining private synonyms, and
// numbers only its own commands here.
inline constexpr ckv::ui::CommandId kBase = ckv::ui::kFirstApplicationCommandId;

inline constexpr ckv::ui::CommandId kNewTerminal = kBase + 0;
inline constexpr ckv::ui::CommandId kCloseTerminal = kBase + 1;
inline constexpr ckv::ui::CommandId kMoveResize = kBase + 2;
inline constexpr ckv::ui::CommandId kDetach = kBase + 3;
inline constexpr ckv::ui::CommandId kSessions = kBase + 4;
inline constexpr ckv::ui::CommandId kKeyReference = kBase + 5;
inline constexpr ckv::ui::CommandId kAbout = kBase + 6;
inline constexpr ckv::ui::CommandId kSendPrefix = kBase + 7;
inline constexpr ckv::ui::CommandId kThemeDark = kBase + 8;
inline constexpr ckv::ui::CommandId kThemeLight = kBase + 9;
inline constexpr ckv::ui::CommandId kThemeMono = kBase + 10;
// Display-only: the digit keys are dispatched by value rather than through a
// single handler, but the reader still has to be told they exist.
inline constexpr ckv::ui::CommandId kFocusByNumber = kBase + 11;
inline constexpr ckv::ui::CommandId kSettings = kBase + 12;

}  // namespace commands

// Which hint set the footer shows. The context is derived from what currently
// holds focus, never set by hand at a call site, so it cannot drift out of
// step with what the keys actually do right now.
enum class Context {
    Terminal,  // a terminal window has focus: keys belong to the child program
    Desktop,   // no terminal has focus (an empty desktop, or ckmux chrome)
    Prefix,    // the prefix is armed and ckmux owns the very next key
};

struct KeyBinding {
    // The key pressed AFTER the prefix, spelled the way chord_spelling()
    // renders a KeyEvent. Empty for menu-only commands.
    std::string_view chord;
    ckv::ui::CommandId command;
    // The title, '&' marking its menu mnemonic. For a ckmux-owned command
    // this is what gets registered in ckVision's CommandRegistry, so menus
    // and this table cannot disagree. For one of ckVision's reserved
    // standard commands the registered title is fixed by the library and
    // this becomes the surface label (CommandPresentation) instead — the
    // sanctioned way to say "Next Terminal" where the framework says
    // "Next Window".
    std::string_view title;
    // The short form the footer and which-key popup show. Kept separate from
    // the title because a footer has cells to spare, not words.
    std::string_view hint;
    // 0 never appears in the footer. Higher survives a narrower terminal —
    // ckVision's StatusLine drops low-priority items first.
    int footer_priority;
};

// Every ckmux binding, in the order the which-key popup and help pages list
// them. Declaration order is deliberate and is the display order.
std::span<const KeyBinding> bindings();

// The binding for a chord, or nullptr. Digit chords are deliberately absent:
// they carry a value rather than selecting a handler, so dispatch resolves
// them before consulting this table (see ClientApp::dispatch_prefix_key).
const KeyBinding* find_binding(std::string_view chord);

// How a key event is spelled for lookup and for display: "c", "T", "?",
// "C-b", "Esc". One spelling function for both, so a binding that dispatches
// is a binding the reader was shown.
std::string chord_spelling(const ckv::KeyEvent& event);

// 1-9 for a digit chord, otherwise 0.
int digit_chord(std::string_view chord);

// How the prefix itself is written wherever ckmux shows it ("^B").
std::string prefix_label(const ckv::KeyChord& prefix);

// "^B c" — how a binding is written in the footer and help.
std::string binding_label(const ckv::KeyChord& prefix, const KeyBinding& binding);

// The footer hints for `context`, highest priority first. The caller turns
// these into ckVision StatusLine items; this function owns WHICH keys a
// context advertises and in what order.
std::vector<const KeyBinding*> footer_bindings(Context context);

}  // namespace ckm::client
