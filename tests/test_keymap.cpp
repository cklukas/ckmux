// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The vocabulary a binding is written in (the configuration spec), and the
// one property that makes a keymap trustworthy: a chord dispatches under
// exactly the spelling the reader was shown. Two spellings — one for lookup,
// one for display — is how a footer comes to advertise a key that does
// nothing.
#include "common/keymap.hpp"

#include <set>
#include <string>

#include "cvision/testing/cktest.hpp"

using ckm::Action;
using ckm::chord_spelling;
using ckm::KeyContext;
using ckm::parse_action;
using ckm::parse_chord;
using ckm::parse_key_context;

CK_TEST(a_chord_survives_the_round_trip_it_is_looked_up_by) {
    const std::string kSpellings[] = {"c",     "T",   "?",     "^B", "^A",         "^]",
                                      "A-x",   "F1",  "F12",   "Enter", "Esc",     "Space",
                                      "Up",    "C-A-Delete",   "S-Tab", "C-Space", "^"};
    for (const std::string& text : kSpellings) {
        const std::optional<ckv::KeyChord> chord = parse_chord(text);
        CK_CHECK(chord.has_value());
        if (chord) CK_CHECK(chord_spelling(*chord) == text);
    }
}

CK_TEST(a_ctrl_chord_is_shown_the_way_a_terminal_reader_writes_it) {
    // The interface spec writes the prefix `^B` in every place a key appears — the
    // footer, the menu accelerators, the which-key popup, the help pages. The
    // spelling authority writes it, so all of them do; when it did not, the
    // Send Prefix menu item advertised "^B C-b" for one key.
    const std::optional<ckv::KeyChord> prefix = parse_chord("C-b");
    CK_CHECK(prefix.has_value());
    if (prefix) CK_CHECK(chord_spelling(*prefix) == "^B");
    // Both spellings in, one out — so a reader's older file and this one
    // cannot become two entries for one key.
    CK_CHECK(parse_chord("^B") == parse_chord("C-b"));
    CK_CHECK(parse_chord("^b") == parse_chord("C-b"));
}

CK_TEST(with_ctrl_the_case_of_a_letter_is_not_its_shift) {
    // The one place the "case IS the shift" rule stops holding. A terminal
    // sends one byte (0x02) for Ctrl+b and for Ctrl+Shift+b alike — the shift
    // is not observable — so `C-B` and `C-b` cannot be two bindings. Before
    // this, `C-B` parsed into a chord no reader could ever press.
    CK_CHECK(parse_chord("C-B") == parse_chord("C-b"));
    const std::optional<ckv::KeyChord> upper = parse_chord("C-B");
    CK_CHECK(upper.has_value());
    if (upper) {
        CK_CHECK(upper->text == "b");
        CK_CHECK(chord_spelling(*upper) == "^B");
    }
    // Without Ctrl the rule is unchanged: `t` and `T` stay two bindings,
    // because the default table uses both (Tile and Cascade).
    CK_CHECK(parse_chord("t") != parse_chord("T"));
}

CK_TEST(the_caret_is_for_ctrl_alone_and_never_hides_a_key) {
    // `A-^X` mixes two conventions; that is a typo, not a third convention.
    CK_CHECK(!parse_chord("A-^x").has_value());
    // A lone '^' is a printable character, so it stays a chord of its own —
    // the caret rule must not swallow the key it is written with.
    const std::optional<ckv::KeyChord> caret = parse_chord("^");
    CK_CHECK(caret.has_value());
    if (caret) {
        CK_CHECK(caret->modifiers == ckv::Modifier::None);
        CK_CHECK(chord_spelling(*caret) == "^");
    }
    // Space is a named key in this grammar, so Ctrl+Space stays `C-Space`
    // rather than becoming an invisible `^ `.
    const std::optional<ckv::KeyChord> ctrl_space = parse_chord("C-Space");
    CK_CHECK(ctrl_space.has_value());
    if (ctrl_space) CK_CHECK(chord_spelling(*ctrl_space) == "C-Space");
    // And a caret with more than one character after it is not a chord.
    CK_CHECK(!parse_chord("^Bc").has_value());
    CK_CHECK(!parse_chord("^ ").has_value());
}

CK_TEST(meta_and_alt_are_one_modifier_written_two_ways) {
    const std::optional<ckv::KeyChord> alt = parse_chord("A-g");
    const std::optional<ckv::KeyChord> meta = parse_chord("M-g");
    CK_CHECK(alt.has_value() && meta.has_value());
    CK_CHECK(alt == meta);
    // And they settle on one spelling, so a table cannot hold the same key
    // under two names.
    if (meta) CK_CHECK(chord_spelling(*meta) == "A-g");
}

CK_TEST(a_chord_that_cannot_be_pressed_is_not_a_chord) {
    const std::string kNotChords[] = {"", "C-", "C-C-x", "Ctrl+b", "nosuchkey", "F13", "ab", "S-t"};
    for (const std::string& text : kNotChords) CK_CHECK(!parse_chord(text).has_value());
}

CK_TEST(the_case_of_a_letter_is_its_shift) {
    // `T` and `t` are different bindings — the default table uses both for
    // Cascade and Tile — so Shift must not be a second way to say the same
    // thing. It is refused on a printable character and accepted on a named
    // key, where the character cannot carry it.
    CK_CHECK(!parse_chord("S-t").has_value());
    const std::optional<ckv::KeyChord> shifted = parse_chord("S-Up");
    CK_CHECK(shifted.has_value());
    if (shifted) CK_CHECK(has_modifier(shifted->modifiers, ckv::Modifier::Shift));
}

CK_TEST(every_action_and_context_name_is_unique_and_round_trips) {
    std::set<std::string_view> seen;
    for (const auto& [name, action] : ckm::action_names()) {
        CK_CHECK(seen.insert(name).second);
        CK_CHECK(parse_action(name) == action);
        CK_CHECK(ckm::action_name(action) == name);
    }
    CK_CHECK(!parse_action("no-such-action").has_value());
    CK_CHECK(!parse_action("").has_value());

    std::set<std::string_view> contexts;
    for (const auto& [name, context] : ckm::key_context_names()) {
        CK_CHECK(contexts.insert(name).second);
        CK_CHECK(parse_key_context(name) == context);
        CK_CHECK(ckm::key_context_name(context) == name);
    }
    CK_CHECK(!parse_key_context("termianl").has_value());
}

CK_TEST(the_name_lists_a_message_offers_are_the_names_that_work) {
    // A rejection message ends with "try one of …", and a reader who copies a
    // word out of it must get a binding that works.
    const std::string actions = ckm::action_name_list();
    for (const auto& [name, action] : ckm::action_names()) {
        (void)action;
        CK_CHECK(actions.find(name) != std::string::npos);
    }
    const std::string contexts = ckm::key_context_name_list();
    CK_CHECK(contexts.find("terminal") != std::string::npos);
    CK_CHECK(contexts.find("copy-mode") != std::string::npos);
}
