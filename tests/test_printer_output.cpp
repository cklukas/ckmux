// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// PRINT-6's rules: what a captured job becomes on the way out.
//
// A job's text is verbatim — ckVision keeps exactly what the child sent,
// because the escapes in it are what a program formatting a page meant to
// send. Deciding what to do with them is this layer's, and the decision is to
// offer both answers rather than pick one. These pin the answers, and in
// particular the cases where "strip the escapes" is harder than it looks: a
// sequence the child never terminated, and control bytes that carry layout a
// person cares about rather than mechanism they do not.
#include "client/printer_output.hpp"

#include <string>

#include "client/stats_format.hpp"
#include "cvision/testing/cktest.hpp"

using ckm::PrinterSaveFormat;
using ckm::client::print_job_as_ansi;
using ckm::client::print_job_as_text;
using ckm::client::print_job_auto_name;
using ckm::client::print_job_extension;
using ckm::client::print_job_formatted;
using ckm::client::print_job_summary;
namespace proto = ckm::proto;

CK_TEST(the_ansi_format_hands_back_exactly_what_the_child_sent) {
    // The whole point of offering it: a document going somewhere that
    // understands the sequences, or a reader for whom what went wrong IS the
    // escapes. Byte for byte, including the ones the text format removes.
    const std::string job = "\x1b[1mBold\x1b[0m\r\nplain\n\x1b]0;title\x07tail";
    CK_CHECK(print_job_as_ansi(job) == job);
    CK_CHECK(print_job_formatted(job, PrinterSaveFormat::Ansi) == job);
    CK_CHECK(print_job_extension(PrinterSaveFormat::Ansi) == ".ansi");
}

CK_TEST(the_text_format_removes_the_sequences_and_keeps_the_words) {
    const std::string job = "\x1b[1mBold\x1b[0m text\n";
    CK_CHECK(print_job_as_text(job) == "Bold text\n");
    CK_CHECK(print_job_formatted(job, PrinterSaveFormat::Text) == "Bold text\n");
    CK_CHECK(print_job_extension(PrinterSaveFormat::Text) == ".txt");
}

CK_TEST(a_string_sequence_is_removed_whole_rather_than_leaving_its_payload_behind) {
    // The failure a naive stripper makes: it drops `ESC ]` and then prints the
    // window title into the reader's document. OSC ends at BEL or ST, DCS and
    // friends at ST only, and the payload between is not text.
    CK_CHECK(print_job_as_text("a\x1b]0;window title\x07z") == "az");
    CK_CHECK(print_job_as_text("a\x1b]0;window title\x1b\\z") == "az");
    CK_CHECK(print_job_as_text("a\x1bPq#0;2;0;0;0#0~~\x1b\\z") == "az");
    CK_CHECK(print_job_as_text("a\x1b^private\x1b\\z") == "az");
    CK_CHECK(print_job_as_text("a\x1b_app\x1b\\z") == "az");
}

CK_TEST(a_sequence_the_child_never_finished_ends_the_document_rather_than_the_program) {
    // Exactly the input a reader most wants to look at: a job cut off by an
    // overflow, or by a child that died mid-sequence. A scanner that ran past
    // the end here would crash on the one document somebody is investigating.
    CK_CHECK(print_job_as_text("kept\x1b]0;never closed") == "kept");
    CK_CHECK(print_job_as_text("kept\x1b[") == "kept");
    CK_CHECK(print_job_as_text("kept\x1b") == "kept");
    CK_CHECK(print_job_as_text("kept\x1bP") == "kept");
    // And a CSI with parameters but no final byte.
    CK_CHECK(print_job_as_text("kept\x1b[38;5;") == "kept");
}

CK_TEST(the_control_bytes_that_carry_layout_survive_and_the_rest_do_not) {
    // Tabs and newlines are what a page looks like; a bare CR is not. A file
    // with bare CRs reads as one overwritten line in half the editors that
    // open it, and the LF beside it already carries the break.
    CK_CHECK(print_job_as_text("a\tb\nc\r\nd") == "a\tb\nc\nd");
    // A form feed is a page break — real structure — so it survives as a blank
    // line rather than as a byte an editor draws as `^L`.
    CK_CHECK(print_job_as_text("page one\fpage two") == "page one\npage two");
    // The remaining C0 codes and DEL are mechanism, not meaning.
    CK_CHECK(print_job_as_text("a\x01\x02z\x7f") == "az");
}

CK_TEST(an_auto_name_is_stable_and_never_handed_to_two_documents) {
    // No timestamp, deliberately: the server's clock is monotonic rather than
    // wall-clock, so a name built from it would be meaningless to a person and
    // different across a restart. The job id is never reused within a
    // terminal's life, which is what makes the name unique without one.
    CK_CHECK(print_job_auto_name(2, 7, PrinterSaveFormat::Text) == "ckmux-print-2-7.txt");
    CK_CHECK(print_job_auto_name(2, 7, PrinterSaveFormat::Ansi) == "ckmux-print-2-7.ansi");
    // Same job, same name, every time — a reader who saves twice overwrites
    // their own file rather than accumulating copies they did not ask for.
    CK_CHECK(print_job_auto_name(2, 7, PrinterSaveFormat::Text) ==
             print_job_auto_name(2, 7, PrinterSaveFormat::Text));
    // Different jobs never collide, whichever number differs.
    CK_CHECK(print_job_auto_name(2, 8, PrinterSaveFormat::Text) !=
             print_job_auto_name(2, 7, PrinterSaveFormat::Text));
    CK_CHECK(print_job_auto_name(3, 7, PrinterSaveFormat::Text) !=
             print_job_auto_name(2, 7, PrinterSaveFormat::Text));
}

CK_TEST(a_job_row_says_what_made_it_in_the_readers_words_not_the_standards) {
    proto::PrintJobInfo job;
    job.kind = proto::PrintJobKind::Controller;
    job.bytes = 12'700;
    job.lines = 42;
    const std::string row = print_job_summary(job);
    // "Controller" is what `CSI 5 i` is called in the standard and means
    // nothing to somebody looking at a list of their own captures.
    CK_CHECK(row.find("Printed by the program") != std::string::npos);
    CK_CHECK(row.find("Controller") == std::string::npos);
    // And the size comes from the one shared formatter (the interface spec).
    CK_CHECK(row.find(ckm::client::format_bytes(12'700)) != std::string::npos);
    CK_CHECK(row.find("42 lines") != std::string::npos);

    job.lines = 1;
    CK_CHECK(print_job_summary(job).find("1 line") != std::string::npos);
    CK_CHECK(print_job_summary(job).find("1 lines") == std::string::npos);
}

CK_TEST(an_overflowed_job_says_it_was_too_large_rather_than_reporting_zero) {
    // The row that would otherwise lie. An emulator that abandons an
    // over-limit job frees the buffer, so the metadata is zero bytes and zero
    // lines — and "0 B · 0 lines" reads as "nothing was printed", which is the
    // opposite of what happened.
    proto::PrintJobInfo overflowed;
    overflowed.kind = proto::PrintJobKind::Controller;
    overflowed.bytes = 0;
    overflowed.lines = 0;
    const std::string row = print_job_summary(overflowed);
    CK_CHECK(row.find("too large, not kept") != std::string::npos);
    CK_CHECK(row.find("0 B") == std::string::npos);
    CK_CHECK(row.find("0 lines") == std::string::npos);
}
