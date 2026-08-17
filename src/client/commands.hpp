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

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/keymap.hpp"
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
inline constexpr ckv::ui::CommandId kCopyMode = kBase + 13;
inline constexpr ckv::ui::CommandId kPaste = kBase + 14;

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
    // renders a KeyEvent. Empty for menu-only commands, and rewritten by a
    // reader's `bind`/`unbind` directives (plans/08) — which is why it owns
    // its storage rather than pointing into the default table.
    std::string chord;
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
    // What a config file calls this row. Empty for the one row that is not an
    // action — the digits carry a value rather than selecting a handler, so
    // nothing can be bound to them.
    std::optional<Action> action;
};

// The bindings, with a reader's rebinding applied. One object rather than a
// free table, because a rebinding has to reach every surface at once: what
// dispatches, what the footer advertises, what the which-key popup lists and
// what the help pages print all read this, so a chord cannot be shown in one
// place and honoured in another.
class Keymap {
public:
    // The built-in defaults of plans/06-ui-design.md.
    Keymap();

    // Applies `directives` in file order — a later `bind` of the same chord
    // wins, an `unbind` after a `bind` leaves neither. Anything that cannot
    // be honoured is appended to `problems` in the reader's own vocabulary
    // rather than dropped: a binding that silently does nothing is the defect
    // this project pins tests against (plans/12, the reachability rule).
    void apply(std::span<const BindDirective> directives, std::vector<std::string>& problems);

    // Every binding, in the order the which-key popup and help pages list
    // them. Declaration order is deliberate and is the display order.
    std::span<const KeyBinding> bindings() const noexcept { return bindings_; }

    // The binding for a chord, or nullptr. Digit chords are deliberately
    // absent: they carry a value rather than selecting a handler, so dispatch
    // resolves them before consulting this table (see ClientApp::resolve_prefix).
    const KeyBinding* find(std::string_view chord) const;

    // The footer hints for `context`, highest priority first. The caller turns
    // these into ckVision StatusLine items; this owns WHICH keys a context
    // advertises and in what order.
    std::vector<const KeyBinding*> footer(Context context) const;

private:
    KeyBinding* row_for(Action action);

    std::vector<KeyBinding> bindings_;
};

// How a key event is spelled for lookup and for display: "c", "T", "?",
// "C-b", "Esc". The one spelling authority lives in common/keymap.hpp so that
// a config file, the dispatcher and every surface agree; this is the overload
// for an event that has just arrived.
std::string chord_spelling(const ckv::KeyEvent& event);
using ckm::chord_spelling;

// 1-9 for a digit chord, otherwise 0.
int digit_chord(std::string_view chord);

// How the prefix itself is written wherever ckmux shows it ("^B").
std::string prefix_label(const ckv::KeyChord& prefix);

// "^B c" — how a binding is written in the footer and help.
std::string binding_label(const ckv::KeyChord& prefix, const KeyBinding& binding);

// The ckVision command an action is carried out by. The two vocabularies are
// deliberately separate: `Action` is what a reader writes in a file and what
// `check-config` validates without a user interface, `CommandId` is what
// ckVision dispatches. This is the one place they meet.
ckv::ui::CommandId command_for_action(Action action);

}  // namespace ckm::client
