// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The ckmux keymap registry: one table that drives every surface which
// mentions a key (the interface spec "Keymap registry"). Runtime dispatch,
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

// What ckmux calls its own commands. A command's identity is the string it is
// declared under; the `CommandId` that stands for it is assigned by the
// registry at runtime and is meaningful only to that registry (ckVision's
// D-013). Nobody picks a number, so ckmux and the library cannot collide over
// one — which is what the previous `base + index` scheme could not promise.
//
// The `ckmux.` prefix is this application's own half of that flat space. The
// library's commands live under `ckv.`, and ckmux references those by their
// own keys (`ckv::ui::std_command_keys`) rather than declaring synonyms: a
// terminal window is a window, and Next Terminal IS the standard next-window
// command wearing a ckmux label.
//
// These are also the durable names. A `bind` line in a config file, a menu
// definition and — at M3 — a CLI subcommand naming a command all key on the
// string, because a runtime-assigned id means nothing outside the process
// that assigned it.
inline constexpr std::string_view kNewTerminal = "ckmux.terminal.new";
inline constexpr std::string_view kCloseTerminal = "ckmux.terminal.close";
// The session model's `kill-terminal`: SIGKILL now, no asking, no grace. A separate
// command from Close rather than a checkbox inside it, because the two answer
// different questions — Close asks a program to finish and negotiates about
// what happens if it will not; this one is what a reader reaches for when that
// negotiation has already failed.
inline constexpr std::string_view kKillTerminal = "ckmux.terminal.kill";
inline constexpr std::string_view kMoveTerminal = "ckmux.terminal.move-session";
// Pinning one terminal's caption to a name the reader chose. Separate from
// `kRenameSession` because a terminal and a session are different things to
// name, and separate from anything the child does with OSC 0/2: this is the
// override, and what the child says goes on being recorded underneath it.
inline constexpr std::string_view kRenameTerminal = "ckmux.terminal.rename";
// What a program printed, and the settings that decide whether it is kept.
// Two commands rather than one: looking at captures and changing the policy
// are different intents, and a reader who wants the second should not have to
// pass through a list of the first.
inline constexpr std::string_view kPrintOutput = "ckmux.terminal.print-output";
inline constexpr std::string_view kPrinterSettings = "ckmux.terminal.printer-settings";
inline constexpr std::string_view kMoveResize = "ckmux.window.move-resize";
// Collapsing ckmux's own bottom chrome: the footer goes and the window bar
// drops to the last row (WP-35). ckmux's rather than ckVision's because the
// footer is ckmux's — the bar only REPORTS its ▼ toggle and hides nothing
// itself, which is what makes the same widget usable by an application whose
// chrome is something else entirely.
inline constexpr std::string_view kToggleStatusBar = "ckmux.window.status-bar";
// Fitting the SESSION's desktop to this reader's screen (WP-40). Named for
// what it changes rather than for what it looks at: `ckmux.session.*` because
// the desktop belongs to the session and every client watching it is affected,
// not `ckmux.window.*` where a reader's own chrome lives.
inline constexpr std::string_view kFitDesktop = "ckmux.session.fit-desktop";
// The View menu's three readouts (WP-39): checkable items that put what the
// processes under each terminal cost on every terminal window's frame footer.
// Three commands because they are three independent checkboxes, persisted as
// three `[general]` keys the same way the Settings dialog's checkboxes are.
inline constexpr std::string_view kShowCpuUsage = "ckmux.view.show-cpu";
inline constexpr std::string_view kShowMemoryRss = "ckmux.view.show-memory-rss";
inline constexpr std::string_view kShowMemoryReal = "ckmux.view.show-memory-real";
inline constexpr std::string_view kDetach = "ckmux.session.detach";
inline constexpr std::string_view kSessions = "ckmux.session.list";
inline constexpr std::string_view kNewSession = "ckmux.session.new";
inline constexpr std::string_view kRenameSession = "ckmux.session.rename";
inline constexpr std::string_view kKillSession = "ckmux.session.kill";
inline constexpr std::string_view kKeyReference = "ckmux.help.keys";
// The whole listing, as against `kKeyReference`'s page for whatever surface
// has the reader's attention (WP-14). Deliberately without a row in the
// keymap table: it is the page that lists the chords, and giving it one of
// its own would be a chord whose only purpose is to describe the others.
inline constexpr std::string_view kAllKeys = "ckmux.help.all-keys";
inline constexpr std::string_view kAbout = "ckmux.help.about";
inline constexpr std::string_view kSendPrefix = "ckmux.terminal.send-prefix";
// Arming the prefix from OUTSIDE a terminal. A terminal view hands the prefix
// key to the client and everything else to its child, so this exists for the
// moments when there is no terminal to press it in — a client with no session,
// or one whose windows are all closed.
inline constexpr std::string_view kArmPrefix = "ckmux.prefix";
// There is no theme command. The colour theme is a stored setting, chosen and
// kept in Settings ▸ General… — three commands that changed it for one session
// and left the file saying otherwise were a menu that could not be trusted.
// Display-only: the digit keys are dispatched by value rather than through a
// single handler, but the reader still has to be told they exist.
inline constexpr std::string_view kFocusByNumber = "ckmux.terminal.focus-by-number";
inline constexpr std::string_view kSettings = "ckmux.settings";
inline constexpr std::string_view kCopyMode = "ckmux.terminal.copy-mode";
inline constexpr std::string_view kPaste = "ckmux.terminal.paste";

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
    // reader's `bind`/`unbind` directives (the configuration spec) — which is why it owns
    // its storage rather than pointing into the default table.
    std::string chord;
    // What this row commands, as its durable name — one of `commands::`
    // above, or one of ckVision's own `std_command_keys`.
    std::string_view key;
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
    // The id this row's key currently stands for, filled in by `resolve()`
    // once a registry has assigned one — last in the aggregate because the
    // table is written positionally and this is the one member no author of a
    // row supplies. `kInvalidCommand` until resolution, and still that
    // afterwards if the command was never declared, which is a state
    // tests/test_command_reachability.cpp fails on rather than lets ship.
    ckv::ui::CommandId command = ckv::ui::kInvalidCommand;
    // The chord this row had before any of the reader's directives were
    // applied — filled in by `Keymap`'s constructor from the table itself, so
    // no row states it twice. It exists for one question a surface has to be
    // able to ask: is this chord the reader's or ours? The help pages MARK a
    // rebinding rather than quietly showing it (the interface spec), which they cannot
    // do without knowing what it replaced.
    //
    // Also updated by `set_default_chord`, because a default that depends on
    // a setting is still a default: `^B ^B` for send-prefix is not a reader
    // rebinding anything, and marking it as one would be a lie about their
    // own file.
    std::string default_chord = {};
};

// The bindings, with a reader's rebinding applied. One object rather than a
// free table, because a rebinding has to reach every surface at once: what
// dispatches, what the footer advertises, what the which-key popup lists and
// what the help pages print all read this, so a chord cannot be shown in one
// place and honoured in another.
class Keymap {
public:
    // The built-in defaults of the interface spec.
    Keymap();

    // Applies `directives` in file order — a later `bind` of the same chord
    // wins, an `unbind` after a `bind` leaves neither. Anything that cannot
    // be honoured is appended to `problems` in the reader's own vocabulary
    // rather than dropped: a binding that silently does nothing is the defect
    // this project pins tests against (the work queue, the reachability rule).
    void apply(std::span<const BindDirective> directives, std::vector<std::string>& problems);

    // Fills one row's default chord before a reader's directives apply — for
    // the row whose sensible default depends on another setting rather than
    // on this table: send-prefix mirrors the prefix itself, and the prefix is
    // the reader's to choose. Runs under apply()'s own collision rule
    // (whatever held the chord loses it), and a later `bind` moves it like
    // any other default.
    void set_default_chord(Action action, std::string chord);

    // Fills in each row's `command` from the registry that declared it.
    // Called once, after the application has declared everything, because an
    // id is assigned at declaration time and is meaningful only to the
    // registry that assigned it: a table of ids built any earlier would be a
    // table of numbers that mean nothing yet.
    void resolve(const ckv::ui::CommandRegistry& registry);

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
// "^B", "Esc". The one spelling authority lives in common/keymap.hpp so that
// a config file, the dispatcher and every surface agree; this is the overload
// for an event that has just arrived.
std::string chord_spelling(const ckv::KeyEvent& event);
using ckm::chord_spelling;

// 1-9 for a digit chord, otherwise 0.
int digit_chord(std::string_view chord);

// How the prefix itself is written wherever ckmux shows it ("^B") — which is
// now just chord_spelling, kept as a name because the call sites read better
// for it. It carried a second caret rule of its own until the one authority
// learned to write the caret; there is nothing left here to disagree with.
std::string prefix_label(const ckv::KeyChord& prefix);

// "^B c" — how a binding is written in the footer and help.
std::string binding_label(const ckv::KeyChord& prefix, const KeyBinding& binding);

// The command an action is carried out by, as its durable name. The two
// vocabularies are deliberately separate: `Action` is the short word a reader
// writes in a config file and `check-config` validates without a user
// interface, while a command key is what the registry resolves to an id. This
// is the one place they meet.
std::string_view command_for_action(Action action);

}  // namespace ckm::client
