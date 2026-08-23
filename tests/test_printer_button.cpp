// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// PRINT-3: the frame button's four states and its live counter.
//
// The button exists because printer-controller mode freezes the screen by
// design — every byte goes to the spool instead of the grid — so without it a
// reader cannot tell a busy program from a wedged one. Most of what is pinned
// here is therefore about telling states apart that look alike from outside,
// and in particular the pair a boolean cannot separate: a document being kept
// and a document being thrown away.
#include "client/printer_button.hpp"

#include <string>

#include "client/stats_format.hpp"
#include "cvision/testing/cktest.hpp"

using ckm::client::PrinterButtonModel;
using ckm::client::PrinterButtonState;
using ckm::client::printer_button_label;
using ckm::client::printer_button_state;
namespace proto = ckm::proto;

namespace {

// A terminal actively collecting a document nobody has answered for yet.
PrinterButtonModel asking(std::uint64_t bytes) {
    PrinterButtonModel model;
    model.mode = proto::PrinterMode::Ask;
    model.state = proto::PrinterState::Capturing;
    model.pending_bytes = bytes;
    return model;
}

}  // namespace

CK_TEST(a_terminal_that_has_printed_nothing_shows_no_button_at_all) {
    // An ordinary window carries no extra row. A permanent badge saying
    // "nothing is happening" is a row of the reader's desktop spent on a
    // sentence that is true of almost every window almost always.
    PrinterButtonModel quiet;
    CK_CHECK(printer_button_state(quiet) == PrinterButtonState::Hidden);
    CK_CHECK(printer_button_label(quiet).empty());
}

CK_TEST(capture_turned_off_shows_nothing_even_while_a_child_prints) {
    // `off` is not a state of this button, it is the absence of one: a reader
    // who turned capture off has said they do not want to be told about
    // printing, and a badge is the notification they declined.
    PrinterButtonModel off = asking(4096);
    off.mode = proto::PrinterMode::Off;
    CK_CHECK(printer_button_state(off) == PrinterButtonState::Hidden);
    CK_CHECK(printer_button_label(off).empty());
}

CK_TEST(an_unanswered_ask_shows_a_counter_that_moves_with_the_document) {
    // The live number is the whole reason this is a counter and not a badge:
    // the screen is frozen, so the counter is the only thing telling a reader
    // that the program is working rather than stuck.
    CK_CHECK(printer_button_state(asking(12'700)) == PrinterButtonState::Asking);
    const std::string early = printer_button_label(asking(12'700));
    const std::string later = printer_button_label(asking(1'300'000));
    // "12 KB", not the interface spec's "12.4 KB": that example contradicted the rule
    // stated in the same sentence — one decimal below ten and none above — and
    // 12.4 is above ten. The formatter is the coherent one; the document has
    // been corrected rather than the code bent to an example.
    CK_CHECK(early == "[ PRINT? · 12 KB ]");
    CK_CHECK(later == "[ PRINT? · 1.2 MB ]");
    CK_CHECK(early != later);
}

CK_TEST(a_sunk_capture_never_claims_to_be_capturing) {
    // The pair a bool cannot tell apart, and the one that matters most. Bytes
    // are arriving and the controller is on in both cases; in one of them the
    // terminal is keeping nothing. A reader told "capturing" through a sink is
    // being promised a document that does not exist.
    PrinterButtonModel sunk = asking(999'999);
    sunk.state = proto::PrinterState::Sunk;
    CK_CHECK(printer_button_state(sunk) == PrinterButtonState::Sunk);
    // And the label offers the only useful next move rather than a number.
    // "0 KB" here would read as "nothing printed", which is the opposite of
    // what happened.
    CK_CHECK(printer_button_label(sunk) == "[ Print settings ]");
    CK_CHECK(printer_button_label(sunk).find("PRINT?") == std::string::npos);

    // Sinking outranks answered-and-capturing too, not only the ask.
    sunk.mode = proto::PrinterMode::Capture;
    sunk.answered = true;
    sunk.jobs = 3;
    CK_CHECK(printer_button_state(sunk) == PrinterButtonState::Sunk);
}

CK_TEST(an_answered_ask_stops_asking_and_starts_counting_jobs) {
    // Answering is the client's own memory: it is a fact about what THIS
    // reader has been shown, and a second client attaching to the same session
    // has not been asked anything.
    PrinterButtonModel model = asking(1'300'000);
    model.jobs = 3;
    CK_CHECK(printer_button_state(model) == PrinterButtonState::Asking);

    model.answered = true;
    CK_CHECK(printer_button_state(model) == PrinterButtonState::Holding);
    CK_CHECK(printer_button_label(model) == "[ PRINT · 3 · 1.2 MB ]");
}

CK_TEST(capture_mode_never_asks_because_the_reader_already_said_yes) {
    PrinterButtonModel model = asking(1'300'000);
    model.mode = proto::PrinterMode::Capture;
    model.jobs = 3;
    // `answered` is not even consulted: the reader answered by setting the
    // mode, and asking again would be asking a question they have settled.
    CK_CHECK(!model.answered);
    CK_CHECK(printer_button_state(model) == PrinterButtonState::Holding);
}

CK_TEST(a_full_spool_still_counts_what_was_kept_and_says_no_more_is_coming) {
    // Unlike the sunk case there IS something to open — the completed jobs
    // were consented to and are still whole — so the count stays and what is
    // added is that nothing further will be kept.
    PrinterButtonModel full;
    full.mode = proto::PrinterMode::Capture;
    full.state = proto::PrinterState::Full;
    full.jobs = 2;
    full.pending_bytes = 1024 * 1024;
    CK_CHECK(printer_button_state(full) == PrinterButtonState::Full);
    CK_CHECK(printer_button_label(full) == "[ PRINT · 2 · 1.0 MB — full ]");
    CK_CHECK(printer_button_label(full).find("2") != std::string::npos);
}

CK_TEST(a_terminal_holding_jobs_keeps_its_button_after_the_child_stops_printing) {
    // The controller is off and nothing is collecting, but two captures are
    // waiting for a reader who has not looked yet. A button that vanished with
    // the controller would take the only route to them off the screen.
    PrinterButtonModel resting;
    resting.mode = proto::PrinterMode::Capture;
    resting.state = proto::PrinterState::Idle;
    resting.jobs = 2;
    CK_CHECK(printer_button_state(resting) == PrinterButtonState::Holding);
    CK_CHECK(!printer_button_label(resting).empty());

    // And with nothing held and nothing collecting it does go.
    resting.jobs = 0;
    CK_CHECK(printer_button_state(resting) == PrinterButtonState::Hidden);
}

CK_TEST(every_byte_count_the_button_shows_comes_from_the_one_shared_formatter) {
    // The interface spec requires one formatter everywhere — button, dialogs, job list —
    // and the failure it guards against is two that agree today. This pins the
    // button against `stats_format`'s own answers rather than against literals
    // of its own, so a change there cannot leave the button behind.
    for (const std::uint64_t bytes : {std::uint64_t{0}, std::uint64_t{512},
                                      std::uint64_t{12'700}, std::uint64_t{1'300'000},
                                      std::uint64_t{350'000'000}}) {
        PrinterButtonModel model = asking(bytes);
        const std::string expected = "[ PRINT? · " + ckm::client::format_bytes(bytes) + " ]";
        CK_CHECK(printer_button_label(model) == expected);
    }
}
