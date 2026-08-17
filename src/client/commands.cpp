// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "client/commands.hpp"

#include <algorithm>
#include <array>

#include "cvision/core/key.hpp"

namespace ckm::client {
namespace {

using ckv::ui::std_commands::kCascade;
using ckv::ui::std_commands::kMenu;
using ckv::ui::std_commands::kNextWindow;
using ckv::ui::std_commands::kPreviousWindow;
using ckv::ui::std_commands::kQuit;
using ckv::ui::std_commands::kTile;
using ckv::ui::std_commands::kWindowList;
using ckv::ui::std_commands::kZoom;

// The table. Order is display order in the which-key popup and the key
// reference, so it is grouped by what a reader is likely to want first:
// making terminals, moving between them, arranging them, then everything
// else. Footer priorities are relative only; the four highest are what a
// reader sees on a narrow terminal.
constexpr std::array<KeyBinding, 17> kBindings{{
    {"c", commands::kNewTerminal, "&New Terminal", "new term", 90},
    {"x", commands::kCloseTerminal, "&Close Terminal", "close term", 40},
    {"n", kNextWindow, "&Next Terminal", "next", 70},
    {"p", kPreviousWindow, "&Previous Terminal", "prev", 60},
    {"1-9", commands::kFocusByNumber, "Focus Terminal by Number", "focus n", 0},
    {"w", kWindowList, "&Window List…", "windows", 30},
    {"z", kZoom, "&Zoom", "zoom", 20},
    {"M", commands::kMoveResize, "&Move / Resize", "move/size", 0},
    {"t", kTile, "&Tile", "tile", 0},
    {"T", kCascade, "C&ascade", "cascade", 0},
    {"m", kMenu, "&Menu Bar", "menu", 95},
    {"d", commands::kDetach, "&Detach", "detach", 80},
    {"s", commands::kSessions, "&Sessions…", "sessions", 0},
    {"?", commands::kKeyReference, "&Keys…", "keys", 50},
    {"q", kQuit, "&Quit ckmux", "quit", 10},
    {"", commands::kSettings, "&Settings…", "settings", 0},
    {"", commands::kAbout, "&About ckmux…", "about", 0},
}};

}  // namespace

std::span<const KeyBinding> bindings() { return kBindings; }

const KeyBinding* find_binding(std::string_view chord) {
    if (chord.empty() || digit_chord(chord) != 0) return nullptr;
    for (const KeyBinding& binding : kBindings)
        if (binding.chord == chord) return &binding;
    return nullptr;
}

int digit_chord(std::string_view chord) {
    if (chord.size() != 1 || chord[0] < '1' || chord[0] > '9') return 0;
    return chord[0] - '0';
}

std::string chord_spelling(const ckv::KeyEvent& event) {
    const ckv::KeyChord& chord = event.chord;
    std::string prefix;
    if (has_modifier(chord.modifiers, ckv::Modifier::Ctrl)) prefix += "C-";
    if (has_modifier(chord.modifiers, ckv::Modifier::Alt)) prefix += "A-";
    if (chord.key == ckv::Key::Char) return prefix + chord.text;
    if (chord.key == ckv::Key::None) return {};
    return prefix + std::string(ckv::key_name(chord.key));
}

std::string prefix_label(const ckv::KeyChord& prefix) {
    // Ctrl chords are written the way a terminal reader expects to see them
    // ("^B"), not the way a GUI would ("Ctrl+B"): this label sits in a
    // one-line footer next to a dozen others, and every cell counts.
    if (prefix.key == ckv::Key::Char && has_modifier(prefix.modifiers, ckv::Modifier::Ctrl) &&
        prefix.text.size() == 1) {
        const char letter = prefix.text[0];
        const char upper = (letter >= 'a' && letter <= 'z')
                               ? static_cast<char>(letter - 'a' + 'A')
                               : letter;
        return std::string("^") + upper;
    }
    return ckv::format(prefix);
}

std::string binding_label(const ckv::KeyChord& prefix, const KeyBinding& binding) {
    if (binding.chord.empty()) return {};
    return prefix_label(prefix) + " " + std::string(binding.chord);
}

std::vector<const KeyBinding*> footer_bindings(Context context) {
    std::vector<const KeyBinding*> chosen;
    for (const KeyBinding& binding : kBindings) {
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

}  // namespace ckm::client
