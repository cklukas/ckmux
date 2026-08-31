// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-15 — the coverage map, and the reason it is a test rather than a table in
// a comment.
//
// M3 acceptance asks that "every operation-table row is green at the wire".
// Coverage for those rows is deliberately spread across suites rather than
// duplicated into one — `attach` is driven where attaching is tested, and
// re-driving it here would mean two tests to keep in step and one of them
// eventually wrong. But scattered coverage makes the *claim* unverifiable: with
// thirteen rows in one document and their tests in nine files, "every row is
// covered" rests on whoever last checked, and a row added to the spec next
// month is covered by nobody and noticed by no one.
//
// So the map is machine-checked, in both directions, against two sources that
// move independently:
//
//   * The session model — the operations table, read at run time.
//     A row added there with no entry below fails this test.
//   * `cktest::cases()` — every case linked into this binary, with the file it
//     came from. An entry below naming a test that has been renamed, deleted or
//     moved to another file fails this test.
//
// Neither direction is redundant. The first catches a spec that grew; the
// second catches a suite that shrank. A map checked in only one direction
// drifts silently in the other, which is the failure this file exists to
// prevent rather than to demonstrate.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cvision/testing/cktest.hpp"

namespace {

// One operations-table row, and the test that drives it.
struct Covered {
    // The row's key: its operation name as the table spells it, with the
    // decoration stripped — `close-terminal id {force, grace}` keys on
    // `close-terminal`. Keyed rather than matched whole because the table's
    // first column carries argument lists and wire shapes that are edited
    // without the operation changing.
    const char* row;
    // The case that drives it, and the suite it lives in. Both are checked:
    // the name against every case linked into this binary, the file against
    // where that case was actually compiled from.
    const char* test;
    const char* suite;
    // Why this test and not another, where the choice is not obvious. A map
    // that says only "covered" invites the next reader to trust it.
    const char* because;
};

// The map. One entry per row of the session model's operations table — no more, no
// fewer, and this file fails if that stops being true.
constexpr Covered kMap[] = {
    {"new-session", "a_name_another_session_holds_is_refused_at_creation_and_at_rename",
     "test_m3_acceptance.cpp",
     "creation is exercised by most suites in passing; this is the one that drives the row's own "
     "stated edge, the name collision, and its refusal"},
    {"attach", "attaching_hands_over_every_terminal_whole", "test_attach.cpp",
     "the row's semantics are the snapshot: layout, every terminal's screen, scrollback and images"},
    {"detach", "a_client_that_dies_is_a_detach_and_its_terminals_do_not_notice", "test_attach.cpp",
     "the row says socket close IS a detach, which is the half a deliberate Detach message cannot show"},
    {"new-terminal", "new_terminal_opens_an_independent_window_and_takes_focus",
     "test_client_smoke.cpp",
     "drives the placement policy the row names, not merely the allocation"},
    {"close-terminal", "closing_asks_first_and_the_window_falls_when_the_child_exits",
     "test_server_loop.cpp",
     "the row is about the ASKING — SIGHUP then SIGTERM with the PTY still draining — rather than "
     "about the window going"},
    {"Child exits by itself", "a_child_that_exits_on_its_own_closes_its_window",
     "test_server_loop.cpp",
     "the `on-exit` default; the hold variants are config behaviour tested with the config"},
    {"move-terminal", "a_move_reparents_the_terminal_and_the_child_never_notices",
     "test_server_loop.cpp",
     "the row's whole claim is that the child does not notice, which is what this names"},
    {"rename-session", "renaming_a_session_reaches_every_client_that_is_watching",
     "test_m3_acceptance.cpp",
     "the row says BROADCAST; a rename that updated the server and told nobody would pass a "
     "codec round-trip perfectly"},
    {"rename-terminal", "a_name_a_reader_gives_a_terminal_outlives_the_client_that_gave_it",
     "test_rename_wire.cpp",
     "the row makes it session state, so surviving the client is the property that distinguishes "
     "it from a local caption"},
    {"kill-terminal", "a_killed_program_gets_no_chance_to_finish_and_the_terminal_goes",
     "test_kill_terminal.cpp",
     "SIGKILL rather than the asking, which is the whole difference from close-terminal"},
    {"kill-session", "killing_a_session_detaches_its_reader_with_the_reason_that_explains_it",
     "test_m3_acceptance.cpp",
     "the row's edge is the reason code: a client detached without one cannot tell a kill from a "
     "takeover, and the two want opposite responses"},
    {"kill-server", "kill_server_can_still_reach_a_server_of_another_protocol_version",
     "test_server_lifecycle.cpp",
     "kill-server has to work across versions or a stale server cannot be stopped by the binary "
     "that finds it"},
    {"resize-desktop", "the_first_client_to_attach_sets_the_sessions_desktop",
     "test_session_desktop.cpp",
     "the row's semantics start at attach: the client declares, the server stores it as session "
     "state"},
};

std::filesystem::path session_model_path() {
#if defined(CKMUX_SOURCE_DIR)
    return std::filesystem::path(CKMUX_SOURCE_DIR) / "plans" / "03-session-model.md";
#else
    return {};
#endif
}

// The operations table's row keys, read from the document rather than from
// memory of it.
std::vector<std::string> rows_in_the_table(const std::filesystem::path& path) {
    std::vector<std::string> rows;
    std::ifstream in(path);
    if (!in) return rows;
    std::string line;
    bool inside = false;
    bool header_seen = false;
    while (std::getline(in, line)) {
        if (line.rfind("## ", 0) == 0) {
            if (inside) break;  // the section ended
            inside = line.find("Operations and their exact semantics") != std::string::npos;
            continue;
        }
        if (!inside || line.rfind("| ", 0) != 0) continue;
        if (!header_seen) {  // the `| Operation | Semantics | Edge cases |` row
            header_seen = true;
            continue;
        }
        // Column one, trimmed, then reduced to the operation's own name: the
        // text inside the first pair of backticks up to its first space, or the
        // whole cell when the row is prose ("Child exits by itself").
        const std::size_t end = line.find('|', 2);
        if (end == std::string::npos) continue;
        std::string cell = line.substr(2, end - 2);
        while (!cell.empty() && cell.back() == ' ') cell.pop_back();
        std::string key = cell;
        if (const std::size_t tick = cell.find('`'); tick != std::string::npos) {
            const std::size_t close = cell.find('`', tick + 1);
            key = cell.substr(tick + 1, close == std::string::npos ? std::string::npos
                                                                   : close - tick - 1);
            if (const std::size_t space = key.find(' '); space != std::string::npos)
                key = key.substr(0, space);
        }
        if (!key.empty()) rows.push_back(key);
    }
    return rows;
}

}  // namespace

CK_TEST(every_operations_table_row_names_a_test_that_exists) {
    // Direction one: the spec against the map. A row added to
    // The session model with nothing driving it fails here, which is
    // the case that cannot be caught by running the suites — an uncovered
    // operation produces no failure anywhere, only silence.
    const std::filesystem::path path = session_model_path();
    // Not a skip. A build that cannot find the spec cannot check the claim
    // this file exists to make, and passing quietly would be the worst of the
    // three outcomes.
    CK_CHECK(!path.empty());
    CK_CHECK(std::filesystem::exists(path));
    if (path.empty() || !std::filesystem::exists(path)) return;

    const std::vector<std::string> rows = rows_in_the_table(path);
    std::printf("  [m3 map] %zu rows in the operations table, %zu entries in the map\n", rows.size(),
                sizeof(kMap) / sizeof(kMap[0]));
    // The table was thirteen rows when this was written. The number is not
    // asserted — rows may legitimately be added — but an empty read means the
    // parser stopped matching the document's shape, which would make every
    // check below vacuous.
    CK_CHECK(rows.size() >= 13U);

    for (const std::string& row : rows) {
        bool mapped = false;
        for (const Covered& entry : kMap)
            if (row == entry.row) mapped = true;
        if (!mapped)
            std::printf("  [m3 map] UNCOVERED operations-table row: \"%s\"\n", row.c_str());
        CK_CHECK(mapped);
    }
}

CK_TEST(every_mapped_test_still_exists_where_the_map_says_it_does) {
    // Direction two: the map against the suites. An entry naming a case that
    // was renamed, deleted or moved to another file fails here — which is the
    // way this map would otherwise rot, because nothing about renaming a test
    // tells you that a plan row was pointing at it.
    for (const Covered& entry : kMap) {
        const cktest::Case* found = nullptr;
        for (const cktest::Case& candidate : cktest::cases())
            if (std::string_view(candidate.name) == entry.test) found = &candidate;
        if (found == nullptr)
            std::printf("  [m3 map] row \"%s\" names a test that no longer exists: %s\n", entry.row,
                        entry.test);
        CK_CHECK(found != nullptr);
        if (found == nullptr) continue;
        // And in the file the map claims, so a case moved between suites is
        // caught rather than silently still passing.
        const std::string_view actual = cktest::source_basename(found->source);
        if (actual != entry.suite)
            std::printf("  [m3 map] row \"%s\": test lives in %.*s, map says %s\n", entry.row,
                        static_cast<int>(actual.size()), actual.data(), entry.suite);
        CK_CHECK(actual == entry.suite);
    }
}

CK_TEST(the_map_carries_no_entry_for_an_operation_the_spec_no_longer_has) {
    // The positive partner to the first case, and the reason the pair is a
    // bijection rather than a subset check: without this, a map could name
    // twenty operations for a thirteen-row table and satisfy "every row is
    // covered" completely while half its entries described a spec that no
    // longer exists.
    const std::filesystem::path path = session_model_path();
    CK_CHECK(!path.empty() && std::filesystem::exists(path));
    if (path.empty() || !std::filesystem::exists(path)) return;
    const std::vector<std::string> rows = rows_in_the_table(path);

    for (const Covered& entry : kMap) {
        bool present = false;
        for (const std::string& row : rows)
            if (row == entry.row) present = true;
        if (!present)
            std::printf("  [m3 map] STALE map entry: \"%s\" is not a row in the table\n", entry.row);
        CK_CHECK(present);
    }
}
