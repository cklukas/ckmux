// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "client/commands.hpp"

#include <algorithm>
#include <array>

#include "cvision/core/key.hpp"

namespace ckm::client {
namespace {

// ckVision's own commands, referenced by the keys the library declares them
// under rather than by private synonyms: a terminal window is a window, and
// "Next Terminal" IS the standard next-window command wearing a ckmux label.
using ckv::ui::std_command_keys::kCascade;
using ckv::ui::std_command_keys::kMenu;
using ckv::ui::std_command_keys::kNextWindow;
using ckv::ui::std_command_keys::kPreviousWindow;
using ckv::ui::std_command_keys::kQuit;
// `ckv.window.tile` is deliberately absent. It is a standard command other
// ckVision applications bind, and the library keeps it — but it produces the
// same arrangement as tile_vertically (U4-b), so listing both in ckmux's own
// menu and help would offer one behaviour twice under two names.
using ckv::ui::std_command_keys::kTileGrid;
using ckv::ui::std_command_keys::kTileHorizontally;
using ckv::ui::std_command_keys::kTileVertically;
using ckv::ui::std_command_keys::kWindowList;
using ckv::ui::std_command_keys::kZoom;

// The defaults. Order is display order in the which-key popup and the key
// reference, so it is grouped by what a reader is likely to want first:
// making terminals, moving between them, arranging them, then everything
// else. Footer priorities are relative only; the four highest are what a
// reader sees on a narrow terminal.
//
// A reader's `bind` lines rewrite the chords in this table (plans/08); they
// never add or remove a row, because a row is a command that exists and a
// binding is only how it is reached. A command with no chord is still listed:
// the help pages and the menus name it.
const std::array<KeyBinding, 26>& default_bindings() {
    static const std::array<KeyBinding, 26> kBindings{{
        {"c", commands::kNewTerminal, "&New Terminal", "new term", 90, Action::NewTerminal},
        {"x", commands::kCloseTerminal, "&Close Terminal", "close term", 40, Action::CloseTerminal},
        {".", commands::kMoveTerminal, "&Move to Session…", "move to session", 0,
         Action::MoveTerminal},
        // Mnemonics are per menu, and New already holds the Terminal menu's N
        // — so Next answers to its x, the way Cascade answers to its a.
        {"n", kNextWindow, "Ne&xt Terminal", "next", 70, Action::NextTerminal},
        {"p", kPreviousWindow, "&Previous Terminal", "prev", 60, Action::PreviousTerminal},
        {"1-9", commands::kFocusByNumber, "Focus Terminal by Number", "focus n", 0, std::nullopt},
        {"w", kWindowList, "&Window List…", "windows", 30, Action::WindowList},
        {"z", kZoom, "&Zoom", "zoom", 20, Action::Zoom},
        {"M", commands::kMoveResize, "&Move / Resize", "move/size", 0, Action::MoveResize},
        // Each tiling answers to the first letter of its own name, which is
        // the whole rule: h, v, g. `t` used to be the unqualified Tile and is
        // now nothing — `^B v` produces exactly the arrangement `^B t` always
        // did, under the name that says which one it is. Keeping `t` as a
        // fourth chord for the same behaviour would put the one thing this
        // package removes back into the reader's hands.
        {"h", kTileHorizontally, "Tile &Horizontally", "tile rows", 0, Action::TileHorizontally},
        {"v", kTileVertically, "Tile &Vertically", "tile columns", 0, Action::TileVertically},
        {"g", kTileGrid, "Tile &Grid", "tile grid", 0, Action::TileGrid},
        {"T", kCascade, "C&ascade", "cascade", 0, Action::Cascade},
        {"m", kMenu, "&Menu Bar", "menu", 95, Action::MenuBar},
        {"[", commands::kCopyMode, "Cop&y Mode", "copy", 45, Action::CopyMode},
        {"]", commands::kPaste, "Past&e", "paste", 25, Action::Paste},
        // The chord is filled in at startup with the prefix's own spelling
        // (Keymap::set_default_chord): ^B ^B sends a literal prefix to the
        // program, and the table cannot know what the reader made the prefix.
        {"", commands::kSendPrefix, "&Send Prefix to Program", "send prefix", 0, Action::SendPrefix},
        {"d", commands::kDetach, "&Detach", "detach", 80, Action::Detach},
        {"s", commands::kSessions, "&Sessions…", "sessions", 0, Action::Sessions},
        {"S", commands::kNewSession, "&New Session…", "new session", 0, Action::NewSession},
        {"R", commands::kRenameSession, "&Rename Session…", "rename", 0, Action::RenameSession},
        {"K", commands::kKillSession, "&End Session…", "end session", 0, Action::KillSession},
        {"?", commands::kKeyReference, "&Keys…", "keys", 50, Action::KeyReference},
        {"q", kQuit, "&Quit ckmux", "quit", 10, Action::Quit},
        {"", commands::kSettings, "&Settings…", "settings", 0, Action::Settings},
        {"", commands::kAbout, "&About ckmux…", "about", 0, Action::About},
    }};
    return kBindings;
}

}  // namespace

std::string_view command_for_action(Action action) {
    switch (action) {
        case Action::NewTerminal: return commands::kNewTerminal;
        case Action::CloseTerminal: return commands::kCloseTerminal;
        case Action::MoveTerminal: return commands::kMoveTerminal;
        case Action::NextTerminal: return kNextWindow;
        case Action::PreviousTerminal: return kPreviousWindow;
        case Action::WindowList: return kWindowList;
        case Action::Zoom: return kZoom;
        case Action::MoveResize: return commands::kMoveResize;
        case Action::TileHorizontally: return kTileHorizontally;
        case Action::TileVertically: return kTileVertically;
        case Action::TileGrid: return kTileGrid;
        case Action::Cascade: return kCascade;
        case Action::MenuBar: return kMenu;
        case Action::CopyMode: return commands::kCopyMode;
        case Action::Paste: return commands::kPaste;
        case Action::Detach: return commands::kDetach;
        case Action::Sessions: return commands::kSessions;
        case Action::NewSession: return commands::kNewSession;
        case Action::RenameSession: return commands::kRenameSession;
        case Action::KillSession: return commands::kKillSession;
        case Action::KeyReference: return commands::kKeyReference;
        case Action::Settings: return commands::kSettings;
        case Action::About: return commands::kAbout;
        case Action::SendPrefix: return commands::kSendPrefix;
        case Action::Quit: return kQuit;
    }
    return {};
}

void Keymap::resolve(const ckv::ui::CommandRegistry& registry) {
    for (KeyBinding& binding : bindings_) {
        const std::optional<ckv::ui::CommandId> id = registry.id_for(binding.key);
        // Left invalid where nothing declared the key. Not asserted: this
        // runs before the reachability test does, and a table row pointing at
        // an undeclared command is precisely what that test exists to report
        // by name rather than to crash on.
        binding.command = id.value_or(ckv::ui::kInvalidCommand);
    }
}

Keymap::Keymap() : bindings_(default_bindings().begin(), default_bindings().end()) {}

KeyBinding* Keymap::row_for(Action action) {
    for (KeyBinding& binding : bindings_)
        if (binding.action == action) return &binding;
    return nullptr;
}

void Keymap::apply(std::span<const BindDirective> directives, std::vector<std::string>& problems) {
    for (const BindDirective& directive : directives) {
        // Only the terminal context has keys today. The rest are refused out
        // loud: a reader who binds a copy-mode key before copy mode exists is
        // told so, rather than left with a binding that looks live in their
        // file and does nothing in the program.
        if (directive.context != KeyContext::Terminal) {
            problems.push_back("bind " + std::string(key_context_name(directive.context)) + " " +
                               directive.chord + ": the " +
                               std::string(key_context_name(directive.context)) +
                               " context has no keys yet — only 'terminal' bindings are honoured today");
            continue;
        }
        // A digit is dispatched by its value, so it can be neither bound nor
        // unbound: ^B 4 means "the fourth terminal", and there is no handler
        // for it to point at.
        if (digit_chord(directive.chord) != 0) {
            problems.push_back("bind terminal " + directive.chord +
                               ": the digits 1-9 focus a terminal by number and cannot be rebound");
            continue;
        }

        // Whatever held the chord loses it, whether this line rebinds it or
        // unbinds it. Doing this first is what makes `bind terminal c detach`
        // move the chord rather than leave two rows claiming it.
        for (KeyBinding& binding : bindings_)
            if (binding.chord == directive.chord) binding.chord.clear();
        if (!directive.action) continue;  // unbind: the chord now reaches nothing

        KeyBinding* const row = row_for(*directive.action);
        if (row == nullptr) {
            // Unreachable through the config parser, which validates action
            // names against the same table this is built from. Kept because
            // the two lists are only equal by construction, and a silent
            // no-op here would be the exact defect this file argues against.
            problems.push_back("bind terminal " + directive.chord + " " +
                               std::string(action_name(*directive.action)) +
                               ": ckmux has no such command");
            continue;
        }
        row->chord = directive.chord;
    }
}

void Keymap::set_default_chord(Action action, std::string chord) {
    KeyBinding* const row = row_for(action);
    if (row == nullptr || chord.empty()) return;
    for (KeyBinding& binding : bindings_)
        if (binding.chord == chord) binding.chord.clear();
    row->chord = std::move(chord);
}

const KeyBinding* Keymap::find(std::string_view chord) const {
    if (chord.empty() || digit_chord(chord) != 0) return nullptr;
    for (const KeyBinding& binding : bindings_)
        if (binding.chord == chord) return &binding;
    return nullptr;
}

std::vector<const KeyBinding*> Keymap::footer(Context context) const {
    std::vector<const KeyBinding*> chosen;
    for (const KeyBinding& binding : bindings_) {
        if (binding.chord.empty()) continue;
        switch (context) {
            case Context::Terminal:
            case Context::Desktop:
                // Both non-prefix contexts advertise the same prefix keys —
                // the prefix is what makes them reachable, and it is armed
                // the same way from either. What differs is the leading
                // label the caller prepends, not the key set.
                if (binding.footer_priority <= 0) continue;
                break;
            case Context::Prefix:
                // Prefix-pending shows everything that is now one key away,
                // including the keys with no footer priority of their own:
                // this is the moment the reader is choosing among them.
                break;
        }
        chosen.push_back(&binding);
    }
    if (context != Context::Prefix)
        std::stable_sort(chosen.begin(), chosen.end(),
                         [](const KeyBinding* a, const KeyBinding* b) {
                             return a->footer_priority > b->footer_priority;
                         });
    return chosen;
}

int digit_chord(std::string_view chord) {
    if (chord.size() != 1 || chord[0] < '1' || chord[0] > '9') return 0;
    return chord[0] - '0';
}

std::string chord_spelling(const ckv::KeyEvent& event) { return ckm::chord_spelling(event.chord); }

std::string prefix_label(const ckv::KeyChord& prefix) {
    // Nothing of its own any more. This used to carry a second caret rule —
    // "^B" here, "C-b" from chord_spelling — which is exactly how a footer
    // comes to advertise a key spelled one way and dispatch a key spelled
    // another. chord_spelling is the single spelling authority (plans/06,
    // common/keymap.hpp) and now writes the caret itself, so this is one name
    // for one function rather than a second opinion.
    return ckm::chord_spelling(prefix);
}

std::string binding_label(const ckv::KeyChord& prefix, const KeyBinding& binding) {
    if (binding.chord.empty()) return {};
    return ckm::chord_spelling(prefix) + " " + binding.chord;
}

}  // namespace ckm::client
