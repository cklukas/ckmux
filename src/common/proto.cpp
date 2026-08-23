// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/proto.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ckm::proto {
namespace {

// --- Writing --------------------------------------------------------------
//
// What a writer must not do is disagree with the reader about byte order or
// field width, which is why both live in this one file.
//
// It is sticky-failing, symmetric with `Reader`: a value that cannot be SAID
// in this format — a string longer than the length field that precedes it — is
// refused, and the writer stays failed, so one check after the whole message
// has been written is enough. The alternative was what this did before:
// truncate the length and append every byte anyway, which manufactures the one
// thing the layer exists to prevent — the two ends disagreeing about where a
// field ends (the engineering standard: assert what cannot be).
class Writer {
public:
    explicit Writer(std::string& out) : out_(out) {}

    bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }

    void u8(std::uint8_t value) { out_.push_back(static_cast<char>(value)); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xFFu));
        u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    }
    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value & 0xFFFFu));
        u16(static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
    }
    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
        u32(static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFu));
    }
    // Signed values go over the wire in their two's-complement bit pattern
    // and come back the same way. Casting through the unsigned type of the
    // same width is the one conversion the standard defines for every value,
    // where a reinterpret of a negative number is implementation-defined.
    void i16(std::int16_t value) { u16(static_cast<std::uint16_t>(value)); }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

    void str(std::string_view text) {
        // The length field is two bytes wide. A longer string writes a length
        // that has wrapped and then appends every byte anyway, so a decoder
        // reads a prefix and then reads the remainder as whatever field comes
        // next — a desync manufactured by the sender, at the one layer whose
        // whole job is that the two ends agree.
        if (text.size() > 0xFFFFu) {
            fail();
            return;
        }
        u16(static_cast<std::uint16_t>(text.size()));
        out_.append(text);
    }
    void blob(std::string_view bytes) {
        if (bytes.size() > 0xFFFFFFFFull) {
            fail();
            return;
        }
        u32(static_cast<std::uint32_t>(bytes.size()));
        out_.append(bytes);
    }

    void color(const ckv::Color& value) {
        // One tagged u32: kind in the top byte, the rest whatever that kind
        // has. An indexed colour keeps its index end to end (ckVision L-22) —
        // flattening it to RGB here would be the multiplexer deciding what a
        // reader's palette means, and would make per-terminal re-theming
        // impossible for good.
        if (value.is_rgb()) {
            u32((2u << 24) | (static_cast<std::uint32_t>(value.r()) << 16) |
                (static_cast<std::uint32_t>(value.g()) << 8) | static_cast<std::uint32_t>(value.b()));
        } else if (value.is_indexed()) {
            u32((1u << 24) | static_cast<std::uint32_t>(value.index()));
        } else {
            u32(0);
        }
    }

    void cell(const ckv::Cell& value) {
        str(value.grapheme());
        // A continuation cell — the far half of a wide character — is width 0
        // with no text, and has to survive as such: rebuilt as a space it
        // would turn every wide character on the screen into two.
        u8(static_cast<std::uint8_t>(std::clamp(value.width(), 0, 255)));
        color(value.style().fg);
        color(value.style().bg);
        u8(static_cast<std::uint8_t>(value.style().attrs));
        u16(0);  // reserved: underline styles/colour, hyperlink id (the terminal-emulation spec U1/U2)
    }

    void runs(const std::vector<CellRun>& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        for (const CellRun& run : value) {
            u16(run.run_length);
            cell(run.cell);
        }
    }

    void lines(const std::vector<std::vector<CellRun>>& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        for (const std::vector<CellRun>& line : value) runs(line);
    }

    void rect(const Rect& value) {
        i16(value.x);
        i16(value.y);
        u16(value.width);
        u16(value.height);
    }

    void tile(const TileFraction& value) {
        u16(value.x);
        u16(value.y);
        u16(value.width);
        u16(value.height);
    }

    void cursor(const CursorOp& value) {
        u16(value.column);
        u16(value.row);
        u8(value.style);
        u8(value.visible);
        u8(value.blink);
    }

    void job(const PrintJobInfo& value) {
        u64(value.job);
        u8(static_cast<std::uint8_t>(value.kind));
        u32(value.bytes);
        u32(value.lines);
        i64(value.at);
    }

private:
    std::string& out_;
    bool ok_ = true;
};

// --- Reading --------------------------------------------------------------
//
// Every method answers "was there room, and was the value sayable?" — never
// "here is a value, trust me". A reader that has failed once stays failed, so
// a decode function can read a dozen fields and check once at the end rather
// than after each: the first failure is what matters and everything after it
// is noise.
class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    bool ok() const noexcept { return ok_; }
    std::size_t remaining() const noexcept { return ok_ ? bytes_.size() - position_ : 0; }
    bool exhausted() const noexcept { return remaining() == 0; }
    void fail() noexcept { ok_ = false; }

    std::uint8_t u8() {
        if (!take(1)) return 0;
        return static_cast<std::uint8_t>(bytes_[position_ - 1]);
    }
    std::uint16_t u16() {
        const std::uint16_t low = u8();
        const std::uint16_t high = u8();
        return static_cast<std::uint16_t>(low | (high << 8));
    }
    std::uint32_t u32() {
        const std::uint32_t low = u16();
        const std::uint32_t high = u16();
        return low | (high << 16);
    }
    std::uint64_t u64() {
        const std::uint64_t low = u32();
        const std::uint64_t high = u32();
        return low | (high << 32);
    }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

    std::string str() {
        const std::uint16_t length = u16();
        return bytes_of(length);
    }
    std::string blob(std::uint32_t cap = kMaxStringBytes) {
        const std::uint32_t length = u32();
        if (length > cap) {
            fail();
            return {};
        }
        return bytes_of(length);
    }

    // A count that is about to size a container. Checked against what is
    // actually LEFT in the payload, divided by the smallest an entry can
    // possibly occupy — because "one byte per entry" is not the honest bound
    // when an entry is sixty-two bytes: a four-megabyte frame could claim four
    // million terminal states and cost a reserve of some hundreds of megabytes
    // out of a decode that promises never to throw.
    //
    // With the real minimum the expansion is bounded and small: the worst case
    // is `payload / 16` `CellRun`s, about three times the frame in vector
    // storage, where it used to be a hundred and eighty times it. `entry_bytes`
    // has no default so that a new call site cannot forget to say what its
    // entries cost.
    std::uint32_t count(std::size_t entry_bytes) {
        const std::uint32_t value = u32();
        if (!ok_ || entry_bytes == 0 || value > remaining() / entry_bytes) {
            fail();
            return 0;
        }
        return value;
    }

    ckv::Color color() {
        const std::uint32_t value = u32();
        switch (value >> 24) {
            case 0:
                // A default colour carries nothing else; a peer that set the
                // payload bits anyway is not speaking this protocol.
                if ((value & 0x00FFFFFFu) != 0) fail();
                return ckv::Color::default_color();
            case 1:
                if ((value & 0x00FFFF00u) != 0) fail();
                return ckv::Color::indexed(static_cast<std::uint8_t>(value & 0xFFu));
            case 2:
                return ckv::Color::rgb(static_cast<std::uint8_t>((value >> 16) & 0xFFu),
                                       static_cast<std::uint8_t>((value >> 8) & 0xFFu),
                                       static_cast<std::uint8_t>(value & 0xFFu));
            default:
                fail();
                return ckv::Color::default_color();
        }
    }

    ckv::Cell cell() {
        const std::string text = str();
        if (text.size() > kMaxCellTextBytes) fail();
        const std::uint8_t width = u8();
        ckv::Style style;
        style.fg = color();
        style.bg = color();
        style.attrs = static_cast<ckv::Attr>(u8());
        if (u16() != 0) fail();  // reserved must be zero
        if (!ok_) return {};
        // Width and text have to agree, because the pair is what the renderer
        // trusts: width 0 with no text is the continuation half of a wide
        // character, and any other combination is a cell that would draw
        // wrongly rather than fail loudly.
        if (width == 0) {
            if (!text.empty()) {
                fail();
                return {};
            }
            return ckv::Cell::continuation(style);
        }
        if (text.empty()) {
            fail();
            return {};
        }
        return ckv::Cell::from_grapheme(text, style);
    }

    std::vector<CellRun> runs() {
        // 2 run_length + 2 text length + 1 width + 4 fg + 4 bg + 1 attrs +
        // 2 reserved: sixteen bytes for a continuation cell, which is the
        // shortest a run can be.
        const std::uint32_t entries = count(16);
        std::vector<CellRun> result;
        result.reserve(entries);
        for (std::uint32_t i = 0; i < entries && ok_; ++i) {
            CellRun run;
            run.run_length = u16();
            // A zero-length run says nothing and would let a peer pad a
            // message with entries that cost the decoder work and the mirror
            // nothing.
            if (run.run_length == 0) fail();
            run.cell = cell();
            if (ok_) result.push_back(std::move(run));
        }
        return result;
    }

    std::vector<std::vector<CellRun>> lines() {
        // A line is at least its own run count.
        const std::uint32_t entries = count(4);
        std::vector<std::vector<CellRun>> result;
        result.reserve(entries);
        for (std::uint32_t i = 0; i < entries && ok_; ++i) result.push_back(runs());
        return result;
    }

    Rect rect() {
        Rect value;
        value.x = i16();
        value.y = i16();
        value.width = u16();
        value.height = u16();
        return value;
    }

    // Read as written and NOT clamped to `kTileFractionWhole`. A fraction past
    // the whole is a peer that has miscomputed one, and the restore it feeds
    // places a window past the desktop's edge — which is a place a reader can
    // drag a window to anyway, so there is nothing here to make safe. Clamping
    // would hide the arithmetic mistake instead of showing it.
    TileFraction tile() {
        TileFraction value;
        value.x = u16();
        value.y = u16();
        value.width = u16();
        value.height = u16();
        return value;
    }

    CursorOp cursor() {
        CursorOp value;
        value.column = u16();
        value.row = u16();
        value.style = u8();
        value.visible = u8();
        value.blink = u8();
        return value;
    }

    PrintJobInfo job() {
        PrintJobInfo value;
        value.job = u64();
        value.kind = enum_of<PrintJobKind>(u8(), 3);
        value.bytes = u32();
        value.lines = u32();
        value.at = i64();
        return value;
    }

    // An enum with a value it does not have is malformed, not a value to be
    // clamped: a mode nobody defined would otherwise become "ask" and a
    // reader would never learn their peer said something impossible.
    template <typename Enum>
    Enum enum_of(std::uint8_t value, std::uint8_t highest) {
        if (value > highest) {
            fail();
            return static_cast<Enum>(0);
        }
        return static_cast<Enum>(value);
    }

private:
    bool take(std::size_t bytes) {
        if (!ok_ || bytes_.size() - position_ < bytes) {
            ok_ = false;
            return false;
        }
        position_ += bytes;
        return true;
    }
    std::string bytes_of(std::size_t length) {
        if (!take(length)) return {};
        return std::string(bytes_.substr(position_ - length, length));
    }

    std::string_view bytes_;
    std::size_t position_ = 0;
    bool ok_ = true;
};

// --- Per-message payloads -------------------------------------------------
//
// One overload pair per message. They are grouped rather than spread over the
// structs so that a change to the wire format is visible as a change to this
// file, and so a struct stays a description of state rather than of bytes.

template <MessageType T>
void write(Writer&, const Empty<T>&) {}
template <MessageType T>
void read(Reader&, Empty<T>&) {}

template <MessageType T>
void write(Writer& w, const TerminalRef<T>& m) { w.u64(m.term); }
template <MessageType T>
void read(Reader& r, TerminalRef<T>& m) { m.term = r.u64(); }

template <MessageType T>
void write(Writer& w, const ImageRef<T>& m) { w.u64(m.id); }
template <MessageType T>
void read(Reader& r, ImageRef<T>& m) { m.id = r.u64(); }

template <MessageType T>
void write(Writer& w, const Nonce<T>& m) { w.u64(m.nonce); }
template <MessageType T>
void read(Reader& r, Nonce<T>& m) { m.nonce = r.u64(); }

void write(Writer& w, const Hello& m) {
    w.u32(m.proto_version);
    w.str(m.build);
    w.u8(static_cast<std::uint8_t>(m.client_kind));
}
void read(Reader& r, Hello& m) {
    m.proto_version = r.u32();
    m.build = r.str();
    m.client_kind = r.enum_of<ClientKind>(r.u8(), 1);
}

void write(Writer& w, const HelloAck& m) {
    w.u32(m.proto_version);
    w.str(m.build);
}
void read(Reader& r, HelloAck& m) {
    m.proto_version = r.u32();
    m.build = r.str();
}

void write(Writer& w, const Refuse& m) { w.str(m.reason); }
void read(Reader& r, Refuse& m) { m.reason = r.str(); }

void write(Writer& w, const NewSession& m) {
    w.str(m.name);
    w.u8(m.spawn_first);
    w.str(m.command);
}
void read(Reader& r, NewSession& m) {
    m.name = r.str();
    m.spawn_first = r.u8();
    m.command = r.str();
}

void write(Writer& w, const Attach& m) {
    w.u64(m.session);
    w.u16(m.columns);
    w.u16(m.rows);
    w.u16(m.pixel_width);
    w.u16(m.pixel_height);
    w.u8(m.host_sixel);
    w.u8(m.mode);
}
void read(Reader& r, Attach& m) {
    m.session = r.u64();
    m.columns = r.u16();
    m.rows = r.u16();
    m.pixel_width = r.u16();
    m.pixel_height = r.u16();
    m.host_sixel = r.u8();
    m.mode = r.u8();
}

void write(Writer& w, const SetReaderMode& m) {
    w.u8(m.scope);
    w.u8(m.mode);
}
void read(Reader& r, SetReaderMode& m) {
    m.scope = r.u8();
    m.mode = r.u8();
}

void write(Writer& w, const ReaderMode& m) { w.u8(m.mode); }
void read(Reader& r, ReaderMode& m) { m.mode = r.u8(); }

void write(Writer& w, const ClientResize& m) {
    w.u16(m.columns);
    w.u16(m.rows);
    w.u16(m.pixel_width);
    w.u16(m.pixel_height);
}
void read(Reader& r, ClientResize& m) {
    m.columns = r.u16();
    m.rows = r.u16();
    m.pixel_width = r.u16();
    m.pixel_height = r.u16();
}

void write(Writer& w, const NewTerminal& m) {
    w.u64(m.session);
    w.str(m.command);
    w.rect(m.rect);
    w.u8(m.host_sixel);
}
void read(Reader& r, NewTerminal& m) {
    m.session = r.u64();
    m.command = r.str();
    m.rect = r.rect();
    m.host_sixel = r.u8();
}

void write(Writer& w, const MoveTerminal& m) {
    w.u64(m.term);
    w.u64(m.destination_session);
    w.u8(m.to_new_session);
}
void read(Reader& r, MoveTerminal& m) {
    m.term = r.u64();
    m.destination_session = r.u64();
    m.to_new_session = r.u8();
}

void write(Writer& w, const CloseTerminal& m) {
    w.u64(m.term);
    w.u8(m.force);
    w.u16(m.grace_seconds);
}
void read(Reader& r, CloseTerminal& m) {
    m.term = r.u64();
    m.force = r.u8();
    m.grace_seconds = r.u16();
}

void write(Writer& w, const MoveResize& m) {
    w.u64(m.term);
    w.rect(m.rect);
}
void read(Reader& r, MoveResize& m) {
    m.term = r.u64();
    m.rect = r.rect();
}

void write(Writer& w, const ZoomTerm& m) {
    w.u64(m.term);
    w.u8(m.on);
}
void read(Reader& r, ZoomTerm& m) {
    m.term = r.u64();
    m.on = r.u8();
}

void write(Writer& w, const RenameSession& m) {
    w.u64(m.id);
    w.str(m.name);
}
void read(Reader& r, RenameSession& m) {
    m.id = r.u64();
    m.name = r.str();
}

void write(Writer& w, const RenameTerminal& m) {
    w.u64(m.id);
    w.str(m.name);
}
void read(Reader& r, RenameTerminal& m) {
    m.id = r.u64();
    m.name = r.str();
}

void write(Writer& w, const KillSession& m) {
    w.u64(m.session);
    w.u8(m.force);
    w.u16(m.grace_seconds);
}
void read(Reader& r, KillSession& m) {
    m.session = r.u64();
    m.force = r.u8();
    m.grace_seconds = r.u16();
}

void write(Writer& w, const Input& m) {
    w.u64(m.term);
    w.blob(m.bytes);
}
void read(Reader& r, Input& m) {
    m.term = r.u64();
    m.bytes = r.blob();
}

void write(Writer& w, const PasteChunk& m) {
    w.u64(m.term);
    w.u32(m.seq);
    w.u8(m.final_chunk);
    w.blob(m.bytes);
}
void read(Reader& r, PasteChunk& m) {
    m.term = r.u64();
    m.seq = r.u32();
    m.final_chunk = r.u8();
    m.bytes = r.blob();
}

void write(Writer& w, const PasteAck& m) { w.u32(m.seq); }
void read(Reader& r, PasteAck& m) { m.seq = r.u32(); }

void write(Writer& w, const SessionList& m) {
    w.u32(static_cast<std::uint32_t>(m.sessions.size()));
    for (const SessionInfo& session : m.sessions) {
        w.u64(session.id);
        w.str(session.name);
        w.u16(session.terminals);
        w.u8(session.attached);
        w.i64(session.created);
        w.i64(session.last_attached);
    }
}
void read(Reader& r, SessionList& m) {
    // 8 id + 2 name length + 2 terminals + 1 attached + 8 created +
    // 8 last_attached.
    const std::uint32_t entries = r.count(29);
    m.sessions.reserve(entries);
    for (std::uint32_t i = 0; i < entries && r.ok(); ++i) {
        SessionInfo session;
        session.id = r.u64();
        session.name = r.str();
        session.terminals = r.u16();
        session.attached = r.u8();
        session.created = r.i64();
        session.last_attached = r.i64();
        if (r.ok()) m.sessions.push_back(std::move(session));
    }
}

void write(Writer& w, const TerminalState& s) {
    w.u64(s.term);
    w.u16(s.index);
    w.str(s.title);
    w.str(s.custom_title);
    w.u8(s.flags);
    w.u16(s.columns);
    w.u16(s.rows);
    w.rect(s.rect);
    w.u16(s.z_order);
    w.u8(s.zoomed);
    w.tile(s.tile);
    w.cursor(s.cursor);
    w.u32(s.modes);
    w.runs(s.grid);
    w.lines(s.scrollback);
    w.u32(static_cast<std::uint32_t>(s.images.size()));
    for (const std::uint64_t id : s.images) w.u64(id);
    w.u8(static_cast<std::uint8_t>(s.printer_mode));
    w.u8(static_cast<std::uint8_t>(s.printer_scope));
    w.u8(static_cast<std::uint8_t>(s.printer_state));
    w.u32(s.printer_bytes);
    w.u32(static_cast<std::uint32_t>(s.print_jobs.size()));
    for (const PrintJobInfo& job : s.print_jobs) w.job(job);
    w.u64(s.clipboard_serial);
    w.u8(s.exited);
    w.i32(s.exit_status);
    w.u8(s.hold);
    w.u8(static_cast<std::uint8_t>(s.diagnostic_kind));
    w.str(s.diagnostic);
    w.u32(s.bell_serial);
    w.u32(s.activity_serial);
}
void read(Reader& r, TerminalState& s) {
    s.term = r.u64();
    s.index = r.u16();
    s.title = r.str();
    s.custom_title = r.str();
    s.flags = r.u8();
    s.columns = r.u16();
    s.rows = r.u16();
    s.rect = r.rect();
    s.z_order = r.u16();
    s.zoomed = r.u8();
    s.tile = r.tile();
    s.cursor = r.cursor();
    s.modes = r.u32();
    s.grid = r.runs();
    s.scrollback = r.lines();
    const std::uint32_t images = r.count(8);  // one id
    s.images.reserve(images);
    for (std::uint32_t i = 0; i < images && r.ok(); ++i) s.images.push_back(r.u64());
    s.printer_mode = r.enum_of<PrinterMode>(r.u8(), 2);
    s.printer_scope = r.enum_of<PrinterScope>(r.u8(), 2);
    s.printer_state = r.enum_of<PrinterState>(r.u8(), 3);
    s.printer_bytes = r.u32();
    // 8 job + 1 kind + 4 bytes + 4 lines + 8 at.
    const std::uint32_t jobs = r.count(25);
    s.print_jobs.reserve(jobs);
    for (std::uint32_t i = 0; i < jobs && r.ok(); ++i) s.print_jobs.push_back(r.job());
    s.clipboard_serial = r.u64();
    s.exited = r.u8();
    s.exit_status = r.i32();
    s.hold = r.u8();
    s.diagnostic_kind = r.enum_of<DiagnosticKind>(r.u8(), 3);
    s.diagnostic = r.str();
    s.bell_serial = r.u32();
    s.activity_serial = r.u32();
}

void write(Writer& w, const Attached& m) {
    w.u64(m.session);
    w.u16(m.snapshot.desktop_columns);
    w.u16(m.snapshot.desktop_rows);
    w.u64(m.snapshot.focused_term);
    w.u32(static_cast<std::uint32_t>(m.snapshot.terminals.size()));
    for (const TerminalState& state : m.snapshot.terminals) write(w, state);
}
void read(Reader& r, Attached& m) {
    m.session = r.u64();
    m.snapshot.desktop_columns = r.u16();
    m.snapshot.desktop_rows = r.u16();
    m.snapshot.focused_term = r.u64();
    // Eighty-nine bytes is a terminal state with no title, no custom title, no
    // grid, no history, no images, no print jobs and no diagnostic — every
    // fixed field and the four counts that say the variable ones are empty. It
    // moves with every fixed field added: WP-30's eight-byte tile share took it
    // from seventy-nine, and the custom title's own two-byte length took it
    // from eighty-seven. A bound left behind is a hostile count checked against
    // a smaller entry than this codec can actually produce.
    const std::uint32_t terminals = r.count(97);
    m.snapshot.terminals.reserve(terminals);
    for (std::uint32_t i = 0; i < terminals && r.ok(); ++i) {
        TerminalState state;
        read(r, state);
        if (r.ok()) m.snapshot.terminals.push_back(std::move(state));
    }
}

void write(Writer& w, const Detached& m) {
    w.u8(static_cast<std::uint8_t>(m.reason));
    w.str(m.text);
}
void read(Reader& r, Detached& m) {
    m.reason = r.enum_of<DetachReason>(r.u8(), 3);
    m.text = r.str();
}

// The entry list both layout messages carry, written once. A client reports an
// arrangement and the server states one back; two copies of these eight lines
// would be two definitions of one shape, and a field added to one of them would
// make the two ends disagree in exactly the direction that never round-trips.
void write_layout_entries(Writer& w, const std::vector<LayoutEntry>& entries) {
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const LayoutEntry& entry : entries) {
        w.u64(entry.term);
        w.rect(entry.rect);
        w.u16(entry.z_order);
        w.u8(entry.zoomed);
        w.tile(entry.tile);
    }
}
void read_layout_entries(Reader& r, std::vector<LayoutEntry>& entries) {
    // 8 term + 8 rect + 2 z_order + 1 zoomed + 8 tile.
    const std::uint32_t count = r.count(27);
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        LayoutEntry entry;
        entry.term = r.u64();
        entry.rect = r.rect();
        entry.z_order = r.u16();
        entry.zoomed = r.u8();
        entry.tile = r.tile();
        if (r.ok()) entries.push_back(entry);
    }
}

void write(Writer& w, const SetLayout& m) { write_layout_entries(w, m.entries); }
void read(Reader& r, SetLayout& m) { read_layout_entries(r, m.entries); }

void write(Writer& w, const WatchStats& m) { w.u8(m.on); }
void read(Reader& r, WatchStats& m) { m.on = r.u8(); }

void write(Writer& w, const SetDesktopSize& m) {
    w.u16(m.columns);
    w.u16(m.rows);
}
void read(Reader& r, SetDesktopSize& m) {
    m.columns = r.u16();
    m.rows = r.u16();
}

void write(Writer& w, const LayoutDelta& m) {
    w.u64(m.session);
    write_layout_entries(w, m.entries);
    w.u64(m.focused_term);
    w.u16(m.desktop_columns);
    w.u16(m.desktop_rows);
}
void read(Reader& r, LayoutDelta& m) {
    m.session = r.u64();
    read_layout_entries(r, m.entries);
    m.focused_term = r.u64();
    m.desktop_columns = r.u16();
    m.desktop_rows = r.u16();
}

void write(Writer& w, const TermOpened& m) {
    w.u64(m.term);
    w.u64(m.session);
    w.u16(m.index);
    w.rect(m.rect);
    w.str(m.title);
    w.u16(m.columns);
    w.u16(m.rows);
}
void read(Reader& r, TermOpened& m) {
    m.term = r.u64();
    m.session = r.u64();
    m.index = r.u16();
    m.rect = r.rect();
    m.title = r.str();
    m.columns = r.u16();
    m.rows = r.u16();
}

void write(Writer& w, const TermClosed& m) {
    w.u64(m.term);
    w.u8(m.exited);
    w.i32(m.exit_status);
    w.u8(m.hold);
}
void read(Reader& r, TermClosed& m) {
    m.term = r.u64();
    m.exited = r.u8();
    m.exit_status = r.i32();
    m.hold = r.u8();
}

void write(Writer& w, const TermMeta& m) {
    w.u64(m.term);
    w.u16(m.index);
    w.str(m.title);
    w.str(m.custom_title);
    w.u8(m.flags);
    w.u32(m.bell_serial);
    w.u32(m.activity_serial);
}
void read(Reader& r, TermMeta& m) {
    m.term = r.u64();
    m.index = r.u16();
    m.title = r.str();
    m.custom_title = r.str();
    m.flags = r.u8();
    m.bell_serial = r.u32();
    m.activity_serial = r.u32();
}

// The op tags. Their own byte, so a GridDelta reads as a sequence of tagged
// operations rather than as a struct whose shape depends on a mode set
// somewhere else.
enum class GridOpTag : std::uint8_t {
    Scroll = 0,
    Cells = 1,
    Cursor = 2,
    Modes = 3,
    Title = 4,
    ScrollbackPush = 5,
    Resize = 6,
};

void write(Writer& w, const GridDelta& m) {
    w.u64(m.term);
    w.u32(m.seq);
    w.u32(static_cast<std::uint32_t>(m.ops.size()));
    for (const GridOp& op : m.ops) {
        std::visit(
            [&w](const auto& value) {
                using Op = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Op, ScrollOp>) {
                    w.u8(static_cast<std::uint8_t>(GridOpTag::Scroll));
                    w.u16(value.top);
                    w.u16(value.bottom);
                    w.i16(value.lines);
                } else if constexpr (std::is_same_v<Op, CellsOp>) {
                    w.u8(static_cast<std::uint8_t>(GridOpTag::Cells));
                    w.u16(value.row);
                    w.u16(value.column);
                    w.runs(value.runs);
                } else if constexpr (std::is_same_v<Op, CursorOp>) {
                    w.u8(static_cast<std::uint8_t>(GridOpTag::Cursor));
                    w.cursor(value);
                } else if constexpr (std::is_same_v<Op, ModesOp>) {
                    w.u8(static_cast<std::uint8_t>(GridOpTag::Modes));
                    w.u32(value.changed_mask);
                    w.u32(value.values);
                } else if constexpr (std::is_same_v<Op, TitleOp>) {
                    w.u8(static_cast<std::uint8_t>(GridOpTag::Title));
                    w.str(value.title);
                } else if constexpr (std::is_same_v<Op, ScrollbackPushOp>) {
                    // Named rather than caught by a trailing `else`. The `else`
                    // that used to sit here assumed it was the last
                    // alternative, so the next op added to the variant would
                    // have been written down as a scrollback push — a bug that
                    // compiles cleanly and only shows up on the wire.
                    w.u8(static_cast<std::uint8_t>(GridOpTag::ScrollbackPush));
                    w.lines(value.lines);
                } else {
                    static_assert(std::is_same_v<Op, ResizeOp>,
                                  "every GridOp alternative needs its own tag here");
                    w.u8(static_cast<std::uint8_t>(GridOpTag::Resize));
                    w.u16(value.columns);
                    w.u16(value.rows);
                }
            },
            op);
    }
}
void read(Reader& r, GridDelta& m) {
    m.term = r.u64();
    m.seq = r.u32();
    // The smallest op there is: a `Title` with an empty string — its tag byte
    // and the two-byte length that says so.
    const std::uint32_t ops = r.count(3);
    m.ops.reserve(ops);
    for (std::uint32_t i = 0; i < ops && r.ok(); ++i) {
        switch (r.enum_of<GridOpTag>(r.u8(), 6)) {
            case GridOpTag::Scroll: {
                ScrollOp op;
                op.top = r.u16();
                op.bottom = r.u16();
                op.lines = r.i16();
                if (r.ok()) m.ops.emplace_back(op);
                break;
            }
            case GridOpTag::Cells: {
                CellsOp op;
                op.row = r.u16();
                op.column = r.u16();
                op.runs = r.runs();
                if (r.ok()) m.ops.emplace_back(std::move(op));
                break;
            }
            case GridOpTag::Cursor: {
                const CursorOp op = r.cursor();
                if (r.ok()) m.ops.emplace_back(op);
                break;
            }
            case GridOpTag::Modes: {
                ModesOp op;
                op.changed_mask = r.u32();
                op.values = r.u32();
                if (r.ok()) m.ops.emplace_back(op);
                break;
            }
            case GridOpTag::Title: {
                TitleOp op;
                op.title = r.str();
                if (r.ok()) m.ops.emplace_back(std::move(op));
                break;
            }
            case GridOpTag::ScrollbackPush: {
                ScrollbackPushOp op;
                op.lines = r.lines();
                if (r.ok()) m.ops.emplace_back(std::move(op));
                break;
            }
            case GridOpTag::Resize: {
                ResizeOp op;
                op.columns = r.u16();
                op.rows = r.u16();
                if (r.ok()) m.ops.emplace_back(op);
                break;
            }
        }
    }
}

void write(Writer& w, const ImageAddBegin& m) {
    w.u64(m.id);
    w.u16(m.width);
    w.u16(m.height);
}
void read(Reader& r, ImageAddBegin& m) {
    m.id = r.u64();
    m.width = r.u16();
    m.height = r.u16();
}

void write(Writer& w, const ImageChunk& m) {
    w.u64(m.id);
    w.u32(m.seq);
    w.blob(m.bytes);
}
void read(Reader& r, ImageChunk& m) {
    m.id = r.u64();
    m.seq = r.u32();
    // Chunked by construction, so a chunk claiming more than the chunk cap is
    // a peer that is not chunking (the protocol spec).
    m.bytes = r.blob(kMaxChunkPayloadBytes);
}

void write(Writer& w, const ImagePlace& m) {
    w.u64(m.term);
    w.u64(m.id);
    w.rect(m.cells);
    w.i16(m.pixel_offset_x);
    w.i16(m.pixel_offset_y);
}
void read(Reader& r, ImagePlace& m) {
    m.term = r.u64();
    m.id = r.u64();
    m.cells = r.rect();
    m.pixel_offset_x = r.i16();
    m.pixel_offset_y = r.i16();
}

void write(Writer& w, const ImageRemove& m) {
    w.u64(m.term);
    w.u64(m.id);
}
void read(Reader& r, ImageRemove& m) {
    m.term = r.u64();
    m.id = r.u64();
}

void write(Writer& w, const ClipboardSet& m) {
    w.u64(m.term);
    w.blob(m.text);
}
void read(Reader& r, ClipboardSet& m) {
    m.term = r.u64();
    m.text = r.blob();
}

void write(Writer& w, const TermDiagnostic& m) {
    w.u64(m.term);
    w.u8(static_cast<std::uint8_t>(m.kind));
    w.str(m.text);
}
void read(Reader& r, TermDiagnostic& m) {
    m.term = r.u64();
    m.kind = r.enum_of<DiagnosticKind>(r.u8(), 3);
    m.text = r.str();
}

void write(Writer& w, const TermStats& m) {
    w.u64(m.term);
    w.u32(m.cpu_permille);
    w.u64(m.rss_bytes);
    w.u64(m.real_bytes);
    w.u8(m.flags);
}
void read(Reader& r, TermStats& m) {
    m.term = r.u64();
    m.cpu_permille = r.u32();
    m.rss_bytes = r.u64();
    m.real_bytes = r.u64();
    m.flags = r.u8();
}

void write(Writer& w, const Error& m) {
    w.u16(m.code);
    w.str(m.context);
    w.str(m.human);
}
void read(Reader& r, Error& m) {
    m.code = r.u16();
    m.context = r.str();
    m.human = r.str();
}

void write(Writer& w, const SetPrinterPolicy& m) {
    w.u8(static_cast<std::uint8_t>(m.scope));
    w.u64(m.target);
    w.u8(static_cast<std::uint8_t>(m.mode));
    w.u32(m.ask_cache);
    w.u32(m.spool_limit);
}
void read(Reader& r, SetPrinterPolicy& m) {
    m.scope = r.enum_of<PrinterScope>(r.u8(), 2);
    m.target = r.u64();
    m.mode = r.enum_of<PrinterMode>(r.u8(), 2);
    m.ask_cache = r.u32();
    m.spool_limit = r.u32();
}

void write(Writer& w, const PrintState& m) {
    w.u64(m.term);
    w.u8(static_cast<std::uint8_t>(m.mode));
    w.u8(static_cast<std::uint8_t>(m.state));
    w.u32(m.bytes);
    w.u16(m.jobs);
}
void read(Reader& r, PrintState& m) {
    m.term = r.u64();
    m.mode = r.enum_of<PrinterMode>(r.u8(), 2);
    m.state = r.enum_of<PrinterState>(r.u8(), 3);
    m.bytes = r.u32();
    m.jobs = r.u16();
}

void write(Writer& w, const PrintJobAdded& m) {
    w.u64(m.term);
    w.job(m.job);
}
void read(Reader& r, PrintJobAdded& m) {
    m.term = r.u64();
    m.job = r.job();
}

void write(Writer& w, const PrintJobFetch& m) {
    w.u64(m.term);
    w.u64(m.job);
}
void read(Reader& r, PrintJobFetch& m) {
    m.term = r.u64();
    m.job = r.u64();
}

void write(Writer& w, const PrintJobData& m) {
    w.u64(m.term);
    w.u64(m.job);
    w.u32(m.seq);
    w.u8(m.final_chunk);
    w.blob(m.bytes);
}
void read(Reader& r, PrintJobData& m) {
    m.term = r.u64();
    m.job = r.u64();
    m.seq = r.u32();
    m.final_chunk = r.u8();
    m.bytes = r.blob(kMaxChunkPayloadBytes);
}

void write(Writer& w, const PrintJobDiscard& m) {
    w.u64(m.term);
    w.u64(m.job);
}
void read(Reader& r, PrintJobDiscard& m) {
    m.term = r.u64();
    m.job = r.u64();
}

// --- The generic paths ----------------------------------------------------

// The cap a type's payload is held to. A snapshot is the one message that
// legitimately carries a screen and its history; a chunk is bounded by being
// a chunk; everything else is a control message and a megabyte is already
// absurd for one.
std::uint32_t payload_cap(MessageType type) {
    switch (type) {
        case MessageType::Attached: return kMaxSnapshotPayloadBytes;
        case MessageType::ImageChunk:
        case MessageType::PrintJobData: return kMaxChunkPayloadBytes + 64;  // header fields plus the blob
        default: return kMaxPayloadBytes;
    }
}

// Finds the variant alternative whose `kType` matches and decodes into it.
// Walking the variant rather than switching on the type is what keeps this
// from drifting: a message added to `Message` is decodable, and one that is
// not in `Message` cannot be named at all.
template <std::size_t Index = 0>
bool decode_alternative(MessageType type, Reader& reader, Message& out) {
    if constexpr (Index < std::variant_size_v<Message>) {
        using Alternative = std::variant_alternative_t<Index, Message>;
        if (Alternative::kType == type) {
            Alternative value;
            read(reader, value);
            if (!reader.ok()) return false;
            out = std::move(value);
            return true;
        }
        return decode_alternative<Index + 1>(type, reader, out);
    } else {
        (void)type;
        (void)reader;
        (void)out;
        return false;
    }
}

template <std::size_t Index = 0>
bool is_known_type(MessageType type) {
    if constexpr (Index < std::variant_size_v<Message>) {
        if (std::variant_alternative_t<Index, Message>::kType == type) return true;
        return is_known_type<Index + 1>(type);
    } else {
        (void)type;
        return false;
    }
}

}  // namespace

Rect to_wire(const ckv::Rect& rect) {
    return Rect{static_cast<std::int16_t>(rect.x), static_cast<std::int16_t>(rect.y),
                static_cast<std::uint16_t>(std::max(0, rect.width)),
                static_cast<std::uint16_t>(std::max(0, rect.height))};
}

ckv::Rect from_wire(const Rect& rect) {
    return ckv::Rect{rect.x, rect.y, static_cast<int>(rect.width), static_cast<int>(rect.height)};
}

std::vector<CellRun> to_runs(const std::vector<ckv::Cell>& cells) {
    std::vector<CellRun> runs;
    for (const ckv::Cell& cell : cells) {
        const bool extends = !runs.empty() && runs.back().run_length < 0xFFFFu &&
                             runs.back().cell.grapheme() == cell.grapheme() &&
                             runs.back().cell.width() == cell.width() &&
                             runs.back().cell.style() == cell.style();
        if (extends) {
            ++runs.back().run_length;
            continue;
        }
        runs.push_back(CellRun{1, cell});
    }
    return runs;
}

std::vector<ckv::Cell> from_runs(const std::vector<CellRun>& runs) {
    std::size_t total = 0;
    for (const CellRun& run : runs) total += run.run_length;
    // A run claims up to 65535 cells in sixteen bytes, so a payload that
    // passed every length check can still ask for a grid nobody has memory
    // for. Callers check the total against a width they know; this is the
    // floor under them, because a `reserve` that throws comes out of a decode
    // path that promises it never will.
    if (total > kMaxGridCells) return {};
    std::vector<ckv::Cell> cells;
    cells.reserve(total);
    for (const CellRun& run : runs)
        for (std::uint16_t i = 0; i < run.run_length; ++i) cells.push_back(run.cell);
    return cells;
}

// --- What a value will cost, before it is written -------------------------
//
// A second encoder that drifts from the first is worse than none, so each of
// these is the matching `Writer` method above read as arithmetic, field for
// field, and `the_size_a_snapshot_will_take_is_known_before_it_is_written`
// asserts they agree with what `encode` actually produces. Nothing here may
// be an estimate: the caller uses it to decide what fits, and a number that
// was close would decide wrongly and say nothing about it.

std::size_t encoded_size(const CellRun& run) {
    // `Writer::runs` per entry: 2 run_length, then `Writer::cell` — 2 text
    // length + N text + 1 width + 4 fg + 4 bg + 1 attrs + 2 reserved.
    return 16 + run.cell.grapheme().size();
}

std::size_t encoded_size(const std::vector<CellRun>& runs) {
    std::size_t total = 4;  // the run count `Writer::runs` writes first
    for (const CellRun& run : runs) total += encoded_size(run);
    return total;
}

std::size_t encoded_size(const std::vector<std::vector<CellRun>>& lines) {
    std::size_t total = 4;  // the line count `Writer::lines` writes first
    for (const std::vector<CellRun>& line : lines) total += encoded_size(line);
    return total;
}

std::size_t encoded_size(const TerminalState& state) {
    // Everything `write(Writer&, const TerminalState&)` puts down that is not
    // the grid or the history: 8 term + 2 index + 2 title length +
    // 2 custom-title length + 1 flags + 2 columns + 2 rows + 8 rect +
    // 2 z_order + 1 zoomed + 8 tile + 7 cursor + 4 modes + 4 image count +
    // 1 printer mode + 1 scope + 1 state + 4 printer bytes + 4 job count +
    // 8 clipboard serial + 1 exited + 4 exit status + 1 hold +
    // 1 diagnostic kind + 2 diagnostic length +
    // 4 bell serial + 4 activity serial.
    //
    // The enumeration above is the ONLY way to check this number, so a field
    // added to the struct and to `write` without a line here leaves the
    // predictor short and nothing says so at the point of the mistake. That
    // happened when the two serials went in (WP-41): 81 stayed 81, the
    // predictor ran 8 bytes short per terminal, and it surfaced in the diff
    // engine's snapshot-size case rather than anywhere near the change.
    constexpr std::size_t kFixed = 89;
    // 8 per image id; 8 job + 1 kind + 4 bytes + 4 lines + 8 at per job.
    return kFixed + state.title.size() + state.custom_title.size() + state.diagnostic.size() +
           encoded_size(state.grid) + encoded_size(state.scrollback) + 8 * state.images.size() +
           25 * state.print_jobs.size();
}

std::size_t encoded_size(const Snapshot& snapshot) {
    // 2 desktop columns + 2 desktop rows + 8 focused + 4 terminal count. The
    // `Attached` that carries it adds the eight bytes of its session id, which
    // belong to the message rather than to the snapshot.
    std::size_t total = 16;
    for (const TerminalState& state : snapshot.terminals) total += encoded_size(state);
    return total;
}

MessageType type_of(const Message& message) {
    return std::visit([](const auto& value) { return std::decay_t<decltype(value)>::kType; }, message);
}

std::string encode(const Message& message, bool* oversize) {
    if (oversize != nullptr) *oversize = false;
    std::string payload;
    Writer writer(payload);
    std::visit([&writer](const auto& value) { write(writer, value); }, message);

    const MessageType type = type_of(message);
    // Checked after the bytes exist rather than as a limit inside the writer:
    // a per-field test on the hot path buys nothing, because what the check
    // rejects is bounded by state the server legitimately holds, and the
    // sender decides what to put in a snapshot before it builds one
    // (`kSnapshotPayloadBudget`).
    if (!writer.ok() || payload.size() > payload_cap(type)) {
        if (oversize != nullptr) *oversize = true;
        return {};
    }

    std::string frame;
    frame.reserve(kHeaderBytes + payload.size());
    Writer header(frame);
    header.u32(static_cast<std::uint32_t>(payload.size()));
    header.u16(static_cast<std::uint16_t>(type));
    header.u16(0);  // flags, reserved
    frame.append(payload);
    return frame;
}

std::string_view describe(DecodeError error) {
    switch (error) {
        case DecodeError::None: return "ok";
        case DecodeError::Incomplete: return "incomplete frame";
        case DecodeError::PayloadTooLarge: return "payload larger than its type allows";
        case DecodeError::UnknownType: return "unknown message type";
        case DecodeError::ReservedFlags: return "reserved flags were not zero";
        case DecodeError::Malformed: return "malformed payload";
        case DecodeError::TrailingBytes: return "payload had bytes left over";
    }
    return "unknown error";
}

DecodeResult decode(std::string_view bytes, Message& message) {
    if (bytes.size() < kHeaderBytes) return DecodeResult{DecodeError::Incomplete, 0};

    Reader header(bytes.substr(0, kHeaderBytes));
    const std::uint32_t payload_length = header.u32();
    const auto type = static_cast<MessageType>(header.u16());
    const std::uint16_t flags = header.u16();

    // Order matters here. The length is checked against its cap BEFORE
    // waiting for the bytes: a peer claiming four gigabytes must be refused
    // now, not after a reader has buffered four gigabytes waiting to refuse
    // it. That is the difference between a bounded error and a denial of
    // service.
    if (!is_known_type(type)) return DecodeResult{DecodeError::UnknownType, 0};
    if (payload_length > payload_cap(type)) return DecodeResult{DecodeError::PayloadTooLarge, 0};
    if (flags != 0) return DecodeResult{DecodeError::ReservedFlags, 0};
    if (bytes.size() - kHeaderBytes < payload_length) return DecodeResult{DecodeError::Incomplete, 0};

    const std::size_t frame_length = kHeaderBytes + payload_length;
    Reader reader(bytes.substr(kHeaderBytes, payload_length));
    Message decoded;
    if (!decode_alternative(type, reader, decoded))
        return DecodeResult{DecodeError::Malformed, frame_length};
    // A payload with bytes left over is not this protocol's: every message has
    // a fixed shape, so a remainder means the sender and this decoder disagree
    // about what the type means — which is exactly the desync that must not be
    // papered over.
    if (!reader.exhausted()) return DecodeResult{DecodeError::TrailingBytes, frame_length};

    message = std::move(decoded);
    return DecodeResult{DecodeError::None, frame_length};
}

bool FrameReader::append(std::string_view bytes) {
    compact();
    // The largest a frame may be, plus its header. A peer that declares a
    // snapshot and then dribbles bytes cannot make this grow past one frame's
    // worth, which is what keeps a wedged or hostile peer's cost bounded.
    constexpr std::size_t kMaxBuffered = kHeaderBytes + kMaxSnapshotPayloadBytes;
    if (buffer_.size() + bytes.size() > kMaxBuffered) return false;
    buffer_.append(bytes);
    return true;
}

DecodeError FrameReader::next(Message& message) {
    const std::string_view view(buffer_.data() + consumed_, buffer_.size() - consumed_);
    const DecodeResult result = decode(view, message);
    if (result.ok()) {
        consumed_ += result.consumed;
        // Nothing left is the common case at the end of a read, and it is the
        // cheapest moment to reset rather than compact.
        if (consumed_ == buffer_.size()) clear();
        return DecodeError::None;
    }
    return result.error;
}

void FrameReader::clear() noexcept {
    buffer_.clear();
    consumed_ = 0;
}

void FrameReader::compact() {
    if (consumed_ == 0) return;
    buffer_.erase(0, consumed_);
    consumed_ = 0;
}

}  // namespace ckm::proto
