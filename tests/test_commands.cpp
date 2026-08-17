// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The keymap table is ckmux's single source of truth for every surface that
// mentions a key (plans/06-ui-design.md). These are the invariants that make
// that claim true rather than aspirational: no two bindings answer to the
// same key, every binding is displayable, and the footer's own ordering is
// the priority order it promises.
#include <algorithm>
#include <set>
#include <string>

#include "client/commands.hpp"
#include "cvision/testing/cktest.hpp"

using ckm::client::binding_label;
using ckm::client::chord_spelling;
using ckm::client::Context;
using ckm::client::digit_chord;
using ckm::client::KeyBinding;
using ckm::client::Keymap;
using ckm::client::prefix_label;

namespace {
const ckv::KeyChord kPrefix{ckv::Key::Char, ckv::Modifier::Ctrl, "b"};

// The defaults, which is what every test here that does not rebind is about.
const Keymap& defaults() {
    static const Keymap keymap;
    return keymap;
}
}  // namespace

CK_TEST(every_binding_is_uniquely_reachable_and_displayable) {
    std::set<std::string> chords;
    for (const KeyBinding& binding : defaults().bindings()) {
        CK_CHECK(!binding.title.empty());
        CK_CHECK(!binding.hint.empty());
        // A row names its command; the id it stands for is a registry's to
        // assign, and this table has not met one.
        CK_CHECK(!binding.key.empty());
        CK_CHECK(binding.command == ckv::ui::kInvalidCommand);
        if (binding.chord.empty()) continue;  // menu-only, still listed in help
        CK_CHECK(chords.insert(binding.chord).second);
    }
    CK_CHECK(!chords.empty());
}

CK_TEST(a_chord_resolves_to_the_binding_the_reader_was_shown) {
    const KeyBinding* const new_terminal = defaults().find("c");
    CK_CHECK(new_terminal != nullptr);
    CK_CHECK(new_terminal->key == ckm::client::commands::kNewTerminal);
    CK_CHECK(binding_label(kPrefix, *new_terminal) == "^B c");

    // Case matters: tile and cascade differ only by Shift, and confusing them
    // would rearrange every window instead of one.
    const KeyBinding* const tile = defaults().find("t");
    const KeyBinding* const cascade = defaults().find("T");
    CK_CHECK(tile != nullptr && cascade != nullptr);
    CK_CHECK(tile->key != cascade->key);

    CK_CHECK(defaults().find("") == nullptr);
    CK_CHECK(defaults().find("nosuchkey") == nullptr);
}

CK_TEST(digit_chords_are_dispatched_by_value_not_through_the_table) {
    // 1-9 select a window by number, so they carry a value rather than
    // choosing a handler. The table must not claim them.
    for (char digit = '1'; digit <= '9'; ++digit) {
        const std::string chord(1, digit);
        CK_CHECK(digit_chord(chord) == digit - '0');
        CK_CHECK(defaults().find(chord) == nullptr);
    }
    CK_CHECK(digit_chord("0") == 0);
    CK_CHECK(digit_chord("c") == 0);
    CK_CHECK(digit_chord("12") == 0);
}

CK_TEST(key_events_are_spelled_the_same_way_they_are_advertised) {
    CK_CHECK(chord_spelling(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "c"}}) == "c");
    CK_CHECK(chord_spelling(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "T"}}) == "T");
    CK_CHECK(chord_spelling(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}}) == "Esc");
    CK_CHECK(chord_spelling(ckv::KeyEvent{kPrefix}) == "C-b");
    // The spelling used for lookup is the spelling shown in the footer, so a
    // key that dispatches is a key the reader was told about.
    const KeyBinding* const found =
        defaults().find(chord_spelling(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "d"}}));
    CK_CHECK(found != nullptr && found->key == ckm::client::commands::kDetach);
}

CK_TEST(the_prefix_is_written_the_way_a_terminal_reader_expects) {
    CK_CHECK(prefix_label(kPrefix) == "^B");
    CK_CHECK(prefix_label(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "a"}) == "^A");
    // A non-Ctrl prefix falls back to ckVision's own canonical spelling
    // rather than inventing a second convention.
    CK_CHECK(prefix_label(ckv::KeyChord{ckv::Key::F12, ckv::Modifier::None, ""}) == "F12");
}

CK_TEST(the_footer_shows_high_priority_keys_first_and_the_prefix_popup_shows_all_of_them) {
    const std::vector<const KeyBinding*> terminal = defaults().footer(Context::Terminal);
    CK_CHECK(!terminal.empty());
    for (std::size_t i = 1; i < terminal.size(); ++i)
        CK_CHECK(terminal[i - 1]->footer_priority >= terminal[i]->footer_priority);
    for (const KeyBinding* binding : terminal) {
        CK_CHECK(binding->footer_priority > 0);
        CK_CHECK(!binding->chord.empty());
    }

    // The prefix popup is the moment the reader is choosing, so it withholds
    // nothing that is one key away — including the keys with no footer
    // priority of their own.
    const std::vector<const KeyBinding*> pending = defaults().footer(Context::Prefix);
    CK_CHECK(pending.size() > terminal.size());
    const bool has_unprioritized =
        std::any_of(pending.begin(), pending.end(),
                    [](const KeyBinding* binding) { return binding->footer_priority == 0; });
    CK_CHECK(has_unprioritized);
    for (const KeyBinding* binding : pending) CK_CHECK(!binding->chord.empty());
}

CK_TEST(every_row_names_a_command_in_one_of_the_two_namespaces) {
    // Identity is a namespaced string, and the prefix says who owns it
    // (ckVision D-013). ckmux declares under "ckmux."; everything else in the
    // table is one of the library's own commands, referenced rather than
    // duplicated under a private synonym.
    std::set<std::string_view> keys;
    for (const KeyBinding& binding : defaults().bindings()) {
        CK_CHECK(binding.key.starts_with("ckmux.") || binding.key.starts_with("ckv."));
        // No two rows are the same command: a table with one command under
        // two chords would make "which key does this?" unanswerable.
        CK_CHECK(keys.insert(binding.key).second);
    }
}

CK_TEST(a_table_learns_its_ids_from_the_registry_that_assigned_them) {
    // The other half of "nobody picks a number": until a registry has
    // declared a key, the row that names it has no id, and after resolution
    // it has exactly the one that registry assigned.
    ckv::ui::CommandRegistry registry;
    Keymap keymap;
    keymap.resolve(registry);
    // A fresh registry declares the standard set and nothing else, so the
    // library's rows resolve and ckmux's own do not exist yet.
    const KeyBinding* const next = keymap.find("n");
    CK_CHECK(next != nullptr);
    if (next != nullptr) CK_CHECK(next->command == registry.standard().next_window);
    const KeyBinding* const new_terminal = keymap.find("c");
    CK_CHECK(new_terminal != nullptr);
    if (new_terminal != nullptr) CK_CHECK(new_terminal->command == ckv::ui::kInvalidCommand);

    const ckv::ui::CommandId declared = registry.declare(
        ckv::ui::CommandDescriptor{.key = std::string(ckm::client::commands::kNewTerminal),
                                   .title = "&New Terminal"});
    keymap.resolve(registry);
    const KeyBinding* const resolved = keymap.find("c");
    CK_CHECK(resolved != nullptr);
    if (resolved != nullptr) CK_CHECK(resolved->command == declared);
}
