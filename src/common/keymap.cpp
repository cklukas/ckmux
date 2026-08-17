// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "common/keymap.hpp"

#include <array>

namespace ckm {
namespace {

using ActionName = std::pair<std::string_view, Action>;
using ContextName = std::pair<std::string_view, KeyContext>;

// Kebab-case, because that is what the rest of the file is written in
// (`login-shell`, `kill-empty-session`) and a reader should not have to
// remember which half of their configuration uses which style.
constexpr std::array<ActionName, 17> kActions{{
    {"new-terminal", Action::NewTerminal},
    {"close-terminal", Action::CloseTerminal},
    {"next-terminal", Action::NextTerminal},
    {"previous-terminal", Action::PreviousTerminal},
    {"window-list", Action::WindowList},
    {"zoom", Action::Zoom},
    {"move-resize", Action::MoveResize},
    {"tile", Action::Tile},
    {"cascade", Action::Cascade},
    {"menu-bar", Action::MenuBar},
    {"detach", Action::Detach},
    {"sessions", Action::Sessions},
    {"key-reference", Action::KeyReference},
    {"settings", Action::Settings},
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
    if (text.size() == 1 && static_cast<unsigned char>(text.front()) > 0x20 &&
        static_cast<unsigned char>(text.front()) < 0x7F) {
        if (has_modifier(chord.modifiers, ckv::Modifier::Shift)) return std::nullopt;
        chord.key = ckv::Key::Char;
        chord.text = std::string(text);
        return chord;
    }
    return std::nullopt;
}

std::string chord_spelling(const ckv::KeyChord& chord) {
    if (chord.key == ckv::Key::None) return {};
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
