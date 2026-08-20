// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/printer_output.hpp"

#include <string>

#include "client/stats_format.hpp"

namespace ckm::client {
namespace {

bool is_csi_final(unsigned char byte) { return byte >= 0x40 && byte <= 0x7E; }

// Where a string-terminated sequence ends: ST (`ESC \`) or, for OSC only, a
// BEL. Answers one past the terminator, or the end of the input for a sequence
// the child never closed — a job can be cut off mid-sequence by an overflow or
// a child that died, and a scanner that ran off the end would be a crash on
// exactly the input a reader most wants to look at.
std::size_t end_of_string_sequence(std::string_view job, std::size_t at, bool bel_ends) {
    while (at < job.size()) {
        const unsigned char byte = static_cast<unsigned char>(job[at]);
        if (bel_ends && byte == 0x07) return at + 1;
        if (byte == 0x1B && at + 1 < job.size() && job[at + 1] == '\\') return at + 2;
        ++at;
    }
    return job.size();
}

}  // namespace

std::string print_job_as_text(std::string_view job) {
    std::string plain;
    plain.reserve(job.size());
    std::size_t at = 0;
    while (at < job.size()) {
        const unsigned char byte = static_cast<unsigned char>(job[at]);
        if (byte == 0x1B) {
            if (at + 1 >= job.size()) break;  // a lone trailing ESC: nothing follows to keep
            const char kind = job[at + 1];
            if (kind == '[') {
                // CSI: parameters and intermediates, then one final byte.
                at += 2;
                while (at < job.size() && !is_csi_final(static_cast<unsigned char>(job[at]))) ++at;
                if (at < job.size()) ++at;
                continue;
            }
            if (kind == ']') {  // OSC — ends at BEL or ST
                at = end_of_string_sequence(job, at + 2, /*bel_ends=*/true);
                continue;
            }
            if (kind == 'P' || kind == 'X' || kind == '^' || kind == '_') {
                // DCS, SOS, PM, APC — ST only.
                at = end_of_string_sequence(job, at + 2, /*bel_ends=*/false);
                continue;
            }
            // Everything else is a two-byte escape (charset selection, RIS,
            // index): the escape and its one following byte both go.
            at += 2;
            continue;
        }
        if (byte == '\n' || byte == '\t') {
            plain.push_back(job[at++]);
            continue;
        }
        if (byte == '\f') {
            // A page break is structure a person cares about, so it survives
            // as one — as a blank line rather than as a control byte an editor
            // would draw as `^L`.
            plain.push_back('\n');
            ++at;
            continue;
        }
        if (byte == '\r') {
            // Dropped rather than kept: a printer stream is full of CR LF, and
            // a file with bare CRs in it reads as one overwritten line in half
            // the editors that open it. The LF that follows carries the break.
            ++at;
            continue;
        }
        if (byte < 0x20 || byte == 0x7F) {
            ++at;  // remaining C0 and DEL: mechanism, not meaning
            continue;
        }
        plain.push_back(job[at++]);
    }
    return plain;
}

std::string print_job_as_ansi(std::string_view job) { return std::string(job); }

std::string print_job_formatted(std::string_view job, PrinterSaveFormat format) {
    return format == PrinterSaveFormat::Ansi ? print_job_as_ansi(job) : print_job_as_text(job);
}

std::string_view print_job_extension(PrinterSaveFormat format) {
    return format == PrinterSaveFormat::Ansi ? ".ansi" : ".txt";
}

std::string print_job_auto_name(std::uint64_t terminal, std::uint64_t job,
                                PrinterSaveFormat format) {
    return "ckmux-print-" + std::to_string(terminal) + "-" + std::to_string(job) +
           std::string(print_job_extension(format));
}

std::string_view print_job_kind_name(proto::PrintJobKind kind) {
    // The reader's words for what produced the document, not the sequence's.
    // "Controller" is what `CSI 5 i` is called in the standard and means
    // nothing to somebody looking at a list of their own captures.
    switch (kind) {
        case proto::PrintJobKind::Screen: return "Screen";
        case proto::PrintJobKind::Line: return "Line";
        case proto::PrintJobKind::Controller: return "Printed by the program";
        case proto::PrintJobKind::Autoprint: return "Autoprint";
    }
    return "Printed by the program";
}

std::string print_job_summary(const proto::PrintJobInfo& job) {
    std::string row(print_job_kind_name(job.kind));
    if (job.bytes == 0) {
        // The overflow case. Neither a size nor a line count, because both
        // would be zero and a row reading "0 B · 0 lines" says "nothing was
        // printed" — the opposite of what happened, which is that something
        // too large was printed and thrown away.
        row += " · too large, not kept";
        return row;
    }
    row += " · " + format_bytes(job.bytes);
    row += job.lines == 1 ? " · 1 line" : " · " + std::to_string(job.lines) + " lines";
    return row;
}

}  // namespace ckm::client
