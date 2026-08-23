// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/keymap.hpp"

#include <array>

namespace ckm {
namespace {

using ActionName = std::pair<std::string_view, Action>;
using ContextName = std::pair<std::string_view, KeyContext>;

// Kebab-case, because that is what the rest of the file is written in
// (`login-shell`, `kill-empty-session`) and a reader should not have to
// remember which half of their configuration uses which style.
constexpr std::array<ActionName, 35> kActions{{
    {"new-terminal", Action::NewTerminal},
    {"close-terminal", Action::CloseTerminal},
    {"kill-terminal", Action::KillTerminal},
    {"move-terminal", Action::MoveTerminal},
    {"rename-terminal", Action::RenameTerminal},
    {"print-output", Action::PrintOutput},
    {"printer-settings", Action::PrinterSettings},
    {"next-terminal", Action::NextTerminal},
    {"previous-terminal", Action::PreviousTerminal},
    {"window-list", Action::WindowList},
    {"zoom", Action::Zoom},
    {"minimize", Action::Minimize},
    {"move-resize", Action::MoveResize},
    // Named for the arrangement, not for the axis word: the two words are
    // used inconsistently across desktops, so `tile-horizontally` is the one
    // that lays full-WIDTH bands top to bottom and `tile-vertically` the one
    // that lays full-HEIGHT bands side by side. A file that said `tile`
    // before this landed is refused by name rather than silently given one of
    // the three — pre-1.0, ckmux fixes the design and tells the reader.
    {"tile-horizontally", Action::TileHorizontally},
    {"tile-vertically", Action::TileVertically},
    {"tile-grid", Action::TileGrid},
    {"cascade", Action::Cascade},
    {"menu-bar", Action::MenuBar},
    {"copy-mode", Action::CopyMode},
    {"paste", Action::Paste},
    {"detach", Action::Detach},
    {"sessions", Action::Sessions},
    {"new-session", Action::NewSession},
    {"rename-session", Action::RenameSession},
    {"kill-session", Action::KillSession},
    {"key-reference", Action::KeyReference},
    {"settings", Action::Settings},
    {"status-bar", Action::ToggleStatusBar},
    {"fit-desktop", Action::FitDesktop},
    {"show-cpu", Action::ShowCpuUsage},
    {"show-memory-rss", Action::ShowMemoryRss},
    {"show-memory-real", Action::ShowMemoryReal},
    {"about", Action::About},
    {"send-prefix", Action::SendPrefix},
    {"quit", Action::Quit},
}};

constexpr std::array<ContextName, 5> kContexts{{
    {"terminal", KeyContext::Terminal},
    {"desktop", KeyContext::Desktop},
    {"copy-mode", KeyContext::CopyMode},
    {"picker", KeyContext::Picker},
    {"move-resize", KeyContext::MoveResize},
}};

bool is_printable_ascii(char c) {
    return static_cast<unsigned char>(c) > 0x20 && static_cast<unsigned char>(c) < 0x7F;
}

// ASCII case, done by arithmetic rather than by <cctype>: `tolower` answers in
// the process's locale, and a chord table whose contents depend on the
// reader's locale is a table that is not the same table on two machines
// (the conventions — determinism is testable or it is not real).
char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char upper_ascii(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

template <typename Table>
std::string join_names(const Table& table) {
    std::string out;
    for (const auto& entry : table) {
        if (!out.empty()) out += ", ";
        out += entry.first;
    }
    return out;
}

}  // namespace

std::span<const std::pair<std::string_view, Action>> action_names() { return kActions; }
std::span<const std::pair<std::string_view, KeyContext>> key_context_names() { return kContexts; }

std::string_view action_name(Action action) {
    for (const auto& [name, value] : kActions)
        if (value == action) return name;
    return {};
}

std::string_view key_context_name(KeyContext context) {
    for (const auto& [name, value] : kContexts)
        if (value == context) return name;
    return {};
}

std::optional<Action> parse_action(std::string_view name) {
    for (const auto& [candidate, value] : kActions)
        if (candidate == name) return value;
    return std::nullopt;
}

std::optional<KeyContext> parse_key_context(std::string_view name) {
    for (const auto& [candidate, value] : kContexts)
        if (candidate == name) return value;
    return std::nullopt;
}

std::string action_name_list() { return join_names(kActions); }
std::string key_context_name_list() { return join_names(kContexts); }

std::optional<ckv::KeyChord> parse_chord(std::string_view text) {
    ckv::KeyChord chord;
    // Modifier prefixes, in any order and at most one of each. `M-` is Meta,
    // which on every keyboard a reader of this file has is the Alt key — the
    // two names are one modifier and are accepted as such.
    for (bool matched = true; matched;) {
        matched = false;
        if (text.size() < 2 || text[1] != '-') break;
        ckv::Modifier modifier = ckv::Modifier::None;
        switch (text.front()) {
            case 'C': modifier = ckv::Modifier::Ctrl; break;
            case 'S': modifier = ckv::Modifier::Shift; break;
            case 'A':
            case 'M': modifier = ckv::Modifier::Alt; break;
            default: break;
        }
        if (modifier == ckv::Modifier::None) break;
        // A repeated modifier is a typo, not an emphasis: `C-C-x` means
        // nothing, and accepting it would bind a chord nobody can press.
        if (has_modifier(chord.modifiers, modifier)) return std::nullopt;
        chord.modifiers = chord.modifiers | modifier;
        text.remove_prefix(2);
        matched = true;
    }
    if (text.empty()) return std::nullopt;

    // `^B` — the spelling the interface spec shows a reader everywhere a key appears,
    // and the one chord_spelling emits. Accepted alongside `C-b` because both
    // are spellings a reader may reasonably write, and because a file written
    // before this one existed still has to load. A lone `^` is not this rule:
    // it is a printable character and falls through to be a chord of its own.
    if (text.size() == 2 && text.front() == '^' && is_printable_ascii(text[1])) {
        // `A-^X` is not a spelling anybody writes; a chord that mixes the two
        // conventions is a typo, not a third convention.
        if (chord.modifiers != ckv::Modifier::None) return std::nullopt;
        chord.modifiers = ckv::Modifier::Ctrl;
        chord.key = ckv::Key::Char;
        chord.text = std::string(1, lower_ascii(text[1]));
        return chord;
    }

    // `Space` is a named key in this grammar and a printable character in
    // ckVision's key model. It is written as a name because a lone blank in a
    // configuration file is unreadable and, after trimming, unwritable.
    if (text == "Space") {
        chord.key = ckv::Key::Char;
        chord.text = " ";
        return chord;
    }
    if (const std::optional<ckv::Key> named = ckv::key_from_name(text)) {
        // Key::Char has no name of its own, so `key_from_name` cannot return
        // it; every hit here is a genuinely named key.
        chord.key = *named;
        return chord;
    }
    // One printable character standing for itself. Its case carries Shift, so
    // an explicit `S-` on it would be a second spelling of the same key.
    if (text.size() == 1 && is_printable_ascii(text.front())) {
        if (has_modifier(chord.modifiers, ckv::Modifier::Shift)) return std::nullopt;
        chord.key = ckv::Key::Char;
        chord.text = std::string(text);
        // With Ctrl, the case is NOT the shift, and this is the one place the
        // rule above stops holding. A terminal sends one C0 byte for Ctrl+b
        // and for Ctrl+Shift+b alike — the shift is not observable — so `C-B`
        // and `C-b` cannot be two bindings. They are one, folded here and
        // spelled `^B`. Before this, `C-B` parsed into a chord no reader could
        // ever press.
        if (has_modifier(chord.modifiers, ckv::Modifier::Ctrl))
            chord.text[0] = lower_ascii(chord.text[0]);
        return chord;
    }
    return std::nullopt;
}

std::string chord_spelling(const ckv::KeyChord& chord) {
    if (chord.key == ckv::Key::None) return {};
    // Ctrl on a printable character is written `^B`: the spelling the interface spec
    // shows in the footer, the menu accelerators, the which-key popup and the
    // help pages. It is written here rather than at each of those surfaces
    // because this function is the single spelling authority — a chord
    // dispatches under exactly the string it returns, so a second caret rule
    // anywhere else is a key the reader is shown and cannot press.
    //
    // Only when Ctrl is the whole modifier set: `A-^X` is not a spelling
    // anybody writes, so every other chord keeps its `C-`/`A-`/`S-` prefix.
    // `C-Space` is likewise left alone — Space is a named key in this grammar,
    // and `^ ` would be an invisible one.
    if (chord.key == ckv::Key::Char && chord.modifiers == ckv::Modifier::Ctrl &&
        chord.text.size() == 1 && chord.text != " ")
        return std::string("^") + upper_ascii(chord.text[0]);

    std::string spelling;
    if (has_modifier(chord.modifiers, ckv::Modifier::Ctrl)) spelling += "C-";
    if (has_modifier(chord.modifiers, ckv::Modifier::Alt)) spelling += "A-";
    if (chord.key == ckv::Key::Char) {
        // Shift is not written here: on a printable character the case is the
        // shift, and a terminal reports the shifted character rather than the
        // unshifted one plus a flag.
        return spelling + (chord.text == " " ? std::string("Space") : chord.text);
    }
    if (has_modifier(chord.modifiers, ckv::Modifier::Shift)) spelling += "S-";
    return spelling + std::string(ckv::key_name(chord.key));
}

}  // namespace ckm
