// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// What a captured print job becomes on its way out of ckmux (PRINT-6): the
// two save formats, and the name a file gets when the reader is not asked for
// one.
//
// A job's text is **verbatim** — ckVision keeps exactly what the child sent,
// because the escape sequences in it are what a program formatting a page for
// a printer meant to send, and a terminal that stripped them would be deciding
// something on the reader's behalf (ckVision's `TerminalPrinterJob`). Deciding
// is this layer's job, and it offers both answers rather than picking one:
//
//   * `.ansi` — the original stream, byte for byte. What to reach for when the
//     document is going somewhere that understands it, or when what went wrong
//     is the point.
//   * `.txt` — the same document with the escapes taken out, which is what a
//     person opening it in an editor almost always wants.
//
// Kept out of the dialogs on purpose: this is the part with rules, and rules
// are worth testing without building a window to hold them.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/config.hpp"
#include "common/proto.hpp"

namespace ckm::client {

// The job's text with terminal control sequences removed, leaving the printed
// characters and the line structure.
//
// What survives: printable text, `\n`, and `\t` — the two C0 codes that carry
// layout a person cares about. A form feed (`\f`, which DECPFF appends after a
// print) becomes a blank line rather than a literal control byte, because a
// page break is real structure and an editor showing `^L` for it is showing
// the mechanism instead of the meaning.
//
// What goes: CSI, OSC, DCS/APC/PM/SOS strings and their terminators, single
// escapes, and the remaining C0/DEL bytes. A document is being handed to a
// person, and a person's editor renders `\x1b[1m` as four wrong characters.
//
// Deliberately NOT a general terminal parser. It removes sequences rather than
// interpreting them: a job that used absolute cursor motion to lay out a page
// loses that layout, which is exactly why `.ansi` exists beside this and why
// the Print Output window says which format it is about to save.
std::string print_job_as_text(std::string_view job);

// The verbatim stream, unchanged. A function rather than a bare copy at the
// call site so that both formats are named the same way, and so the one that
// does nothing is as visible in the code as the one that does.
std::string print_job_as_ansi(std::string_view job);

std::string print_job_formatted(std::string_view job, PrinterSaveFormat format);

// The extension for a format, with its dot.
std::string_view print_job_extension(PrinterSaveFormat format);

// The name a saved job gets when `[printer] save-ask-name` is off, so a reader
// who prints repeatedly is not answering a file dialog every time.
//
// Shaped to sort usefully and to collide only with itself:
// `ckmux-print-<terminal>-<job>.<ext>`. The terminal and job numbers rather
// than a timestamp, because the server's clock is monotonic rather than
// wall-clock (the conventions — determinism is testable or it is not real), and a
// name built from a monotonic reading would be meaningless to a person and
// unstable across a restart. The job id is never reused within a terminal's
// life, so a name is never handed to two documents.
std::string print_job_auto_name(std::uint64_t terminal, std::uint64_t job,
                                PrinterSaveFormat format);

// What one job's row says in the Print Output window's list: what produced it,
// how big it is, and how many lines. Byte counts go through `stats_format`'s
// shared formatter, which the interface spec requires everywhere.
std::string print_job_summary(const proto::PrintJobInfo& job);

// What a reader is told about a job whose buffer the emulator freed. Not a
// size and not a count: the document is gone, and a row reading "0 B" would
// say "nothing was printed", which is the opposite of what happened.
std::string_view print_job_kind_name(proto::PrintJobKind kind);

}  // namespace ckm::client
