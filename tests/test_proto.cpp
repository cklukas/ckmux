// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The wire protocol (the protocol spec, WP-1). No socket, no server, no
// clock: a codec is a pure function over values, and the whole point of
// testing it here is that every case below would otherwise only be reachable
// by getting a real client and a real server into the state that produces it.
//
// Four kinds of case, because they answer four different questions. Round
// trips ask "does every message survive the wire". Properties ask "does it
// survive whatever a peer actually sends" — random valid messages, and random
// bytes that must error rather than crash. Stream cases ask "does a decoder
// cope with what a socket really delivers", which is never one frame at a
// time. And cap cases ask "does a hostile peer cost a connection rather than
// the process".
#include "common/proto.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "cvision/testing/cktest.hpp"

using namespace ckm::proto;

namespace {

ckv::Cell cell(std::string_view text, ckv::Color fg = ckv::Color::default_color(),
               ckv::Color bg = ckv::Color::default_color(), ckv::Attr attrs = ckv::Attr{}) {
    ckv::Style style;
    style.fg = fg;
    style.bg = bg;
    style.attrs = attrs;
    return ckv::Cell::from_grapheme(text, style);
}

// Encode, decode, and insist the value came back. Returns the decoded message
// so a caller can look closer when the equality is not the whole claim.
Message round_trip(const Message& original) {
    const std::string bytes = encode(original);
    Message decoded;
    const DecodeResult result = decode(bytes, decoded);
    CK_CHECK(result.ok());
    CK_CHECK(result.consumed == bytes.size());
    CK_CHECK(type_of(decoded) == type_of(original));
    CK_CHECK(decoded == original);
    return decoded;
}

// One of every message, with every field set to something that is not its
// default — a round trip that only ever sees zeros proves nothing about field
// order, and a field left at its default would survive being dropped.
std::vector<Message> every_message() {
    std::vector<Message> messages;

    messages.emplace_back(Hello{kProtocolVersion, "ckmux 0.0.1 (M1)", ClientKind::Cli});
    messages.emplace_back(HelloAck{kProtocolVersion, "ckmux 0.0.1 (M1)"});
    messages.emplace_back(Refuse{"server speaks 1, client speaks 2 — restart the server: ckmux kill-server"});
    messages.emplace_back(Ping{0xFEEDFACECAFEBEEDull});
    messages.emplace_back(Pong{0xFEEDFACECAFEBEEDull});
    messages.emplace_back(ListSessions{});
    messages.emplace_back(NewSession{"work", 1, "/bin/zsh"});
    messages.emplace_back(Attach{7, 120, 40, 1080, 720, 1});
    messages.emplace_back(Detach{});
    messages.emplace_back(ClientResize{80, 24, 720, 432});
    messages.emplace_back(RenameSession{9, "renamed"});
    messages.emplace_back(KillSession{9});
    messages.emplace_back(KillServer{});

    SessionList list;
    list.sessions.push_back(SessionInfo{1, "first", 3, 1, -1000, 2000});
    list.sessions.push_back(SessionInfo{2, "second", 0, 0, 0, 0});
    messages.emplace_back(std::move(list));
    messages.emplace_back(SessionsChanged{});

    Attached attached;
    attached.session = 4;
    attached.snapshot.desktop_columns = 100;
    attached.snapshot.desktop_rows = 30;
    attached.snapshot.focused_term = 11;
    TerminalState state;
    state.term = 11;
    state.index = 2;
    state.title = "vim plans/05-protocol.md";
    // The reader's own name for it, and deliberately not the same string as
    // the child's title: the two travel together and a codec that carried one
    // of them twice would round-trip cleanly if they matched.
    state.custom_title = "protocol notes";
    state.flags = static_cast<std::uint8_t>(TermMetaFlag::Bell) |
                  static_cast<std::uint8_t>(TermMetaFlag::Activity);
    state.columns = 80;
    state.rows = 24;
    state.rect = Rect{-2, 3, 40, 12};
    state.z_order = 5;
    state.zoomed = 1;
    // The right-hand half of a 50/50 split, which is what a reattaching client
    // restores proportionally rather than from the rect above (WP-30). A codec
    // that dropped it would put a tiled reader's windows back at absolute cell
    // positions from a terminal they may no longer be using.
    state.tile = TileFraction{kTileFractionWhole / 2, 0, kTileFractionWhole / 2,
                              kTileFractionWhole};
    state.cursor = CursorOp{7, 9, 2, 1, 1};
    state.modes = static_cast<std::uint32_t>(ModeBit::MouseReporting) |
                  static_cast<std::uint32_t>(ModeBit::MouseEncodingSgr) |
                  static_cast<std::uint32_t>(ModeBit::AlternateBuffer) |
                  // The tracking level as well as the flag, since a codec that
                  // carried only the flag would leave a program that asked for
                  // DEC 1000 reading 1003's motion reports as something else.
                  (3u << kMouseTrackingShift) |
                  // And the kitty keyboard enhancements, in the five bits above
                  // it: a program that turned the legacy fallback off is sent
                  // the legacy encoding by a client that was never told (M-R2).
                  (0b10011u << kKeyboardFlagsShift);
    state.grid = to_runs({cell("a"), cell("a"), cell("b", ckv::Color::indexed(31)),
                          ckv::Cell::continuation(ckv::Style{})});
    state.scrollback = {to_runs({cell("o"), cell("l"), cell("d")}), to_runs({cell("e"), cell("r")})};
    state.images = {77, 78};
    state.printer_mode = PrinterMode::Capture;
    state.printer_scope = PrinterScope::Session;
    state.printer_state = PrinterState::Capturing;
    state.printer_bytes = 12345;
    state.print_jobs.push_back(PrintJobInfo{1, PrintJobKind::Autoprint, 900, 30, -7});
    // What a reattaching reader was losing (R8): the clipboard watermark, the
    // child's own fate, and the terminal's newest complaint. A codec that
    // dropped any of them would put a dead shell back on screen as a live
    // terminal, or replay a clipboard write nobody asked for.
    state.clipboard_serial = 9;
    state.exited = 1;
    state.exit_status = -6;
    state.hold = 1;
    state.diagnostic_kind = DiagnosticKind::UnsupportedSequence;
    state.diagnostic = "unsupported child OSC sequence";
    attached.snapshot.terminals.push_back(std::move(state));
    messages.emplace_back(std::move(attached));

    messages.emplace_back(Detached{DetachReason::Takeover, "another client attached"});

    // WP-49's two. `SetReaderMode` carries the combination a reader reaches for
    // most and the one the server refuses least trivially — aimed at the
    // others, asking them to watch.
    messages.emplace_back(SetReaderMode{static_cast<std::uint8_t>(ReaderScope::Others),
                                        static_cast<std::uint8_t>(AttachMode::Watch)});
    messages.emplace_back(ReaderMode{static_cast<std::uint8_t>(AttachMode::Watch)});

    LayoutDelta layout;
    layout.session = 4;
    layout.entries.push_back(LayoutEntry{11, Rect{1, 2, 30, 10}, 1, 0, TileFraction{}});
    layout.entries.push_back(LayoutEntry{12, Rect{-5, 0, 20, 8}, 2, 1, TileFraction{}});
    layout.focused_term = 12;
    layout.desktop_columns = 100;
    layout.desktop_rows = 30;
    messages.emplace_back(std::move(layout));

    messages.emplace_back(TermOpened{11, 4, 2, Rect{1, 1, 40, 12}, "zsh", 80, 24});
    messages.emplace_back(TermClosed{11, 1, -9, 1});
    // A custom title as well as the child's, and two different strings: a
    // codec that wrote one field twice, or dropped the new one, would round-
    // trip cleanly if they were equal.
    messages.emplace_back(
        // Serials as well as the level: a round trip that left them at zero
        // would pass while the edge WP-41 needs travelled nowhere (WP-44).
        TermMeta{11, 2, "make", "build", static_cast<std::uint8_t>(TermMetaFlag::Activity), 3, 41});
    messages.emplace_back(NewTerminal{4, "htop", Rect{0, 0, 60, 20}});
    // Non-default force and grace, so a codec that silently drops either
    // field fails here rather than in a dialog whose checkbox stopped
    // meaning anything.
    messages.emplace_back(CloseTerminal{11, 0, 30});
    messages.emplace_back(KillTerminal{11});
    messages.emplace_back(RespawnTerminal{11});
    messages.emplace_back(MoveTerminal{11, 5, 1});
    messages.emplace_back(MoveResize{11, Rect{-1, -2, 33, 11}});
    messages.emplace_back(Raise{11});
    messages.emplace_back(FocusTerm{11});
    messages.emplace_back(ZoomTerm{11, 1});

    SetLayout report;
    report.entries.push_back(LayoutEntry{11, Rect{-6, 4, 44, 14}, 0, 0, TileFraction{}});
    report.entries.push_back(LayoutEntry{12, Rect{3, 0, 22, 7}, 1, 1, TileFraction{}});
    messages.emplace_back(std::move(report));

    messages.emplace_back(RenameTerminal{11, "build"});
    messages.emplace_back(Input{11, std::string("\x1b[A\x00 bytes", 10)});
    messages.emplace_back(PasteChunk{11, 3, 1, "pasted text"});
    messages.emplace_back(PasteAck{3});
    // The act, not the fact: `ClientResize` says how big one reader's screen
    // is, this asks for the SESSION's desktop to become a size (WP-40).
    messages.emplace_back(SetDesktopSize{90, 30});
    messages.emplace_back(WatchStats{1});
    // Every field distinct and nonzero, so a codec that writes one twice or
    // drops one fails here; permille above 1000, because more than one core
    // is a value this field must carry, not an overflow.
    messages.emplace_back(TermStats{11, 8370, 220'200'960, 100'663'296,
                                    static_cast<std::uint8_t>(
                                        static_cast<std::uint8_t>(TermStatsFlag::HasReal) |
                                        static_cast<std::uint8_t>(TermStatsFlag::Alive))});

    GridDelta delta;
    delta.term = 11;
    delta.seq = 4242;
    delta.ops.emplace_back(ScrollOp{0, 24, 3});
    delta.ops.emplace_back(ScrollOp{2, 20, -1});
    delta.ops.emplace_back(CellsOp{5, 6, to_runs({cell("x"), cell("x"), cell("y")})});
    delta.ops.emplace_back(CursorOp{1, 2, 1, 0, 1});
    delta.ops.emplace_back(ModesOp{0xFFu | kMouseTrackingMask,
                                   0x0Fu | (2u << kMouseTrackingShift)});
    delta.ops.emplace_back(TitleOp{"a new title"});
    delta.ops.emplace_back(ScrollbackPushOp{{to_runs({cell("s"), cell("b")})}});
    // The geometry op, whose absence let a repaint of a smaller terminal be
    // applied to a larger mirror with nothing anywhere noticing (C3). It sits
    // last in the variant and is written under its own tag, which is the bug
    // the trailing `else` in the writer would have reintroduced.
    delta.ops.emplace_back(ResizeOp{80, 24});
    messages.emplace_back(std::move(delta));

    messages.emplace_back(ImageAddBegin{77, 640, 480});
    messages.emplace_back(ImageChunk{77, 1, std::string(1024, '\xAB')});
    messages.emplace_back(ImageEnd{77});
    messages.emplace_back(ImagePlace{11, 77, Rect{4, 5, 10, 6}, -3, 7});
    messages.emplace_back(ImageRemove{11, 77});
    messages.emplace_back(ClipboardSet{11, "copied out of a program"});
    messages.emplace_back(
        TermDiagnostic{11, DiagnosticKind::LimitExceeded, "child clipboard payload exceeded"});
    messages.emplace_back(Error{404, "rename-session", "a session called 'work' already exists"});
    messages.emplace_back(SetPrinterPolicy{PrinterScope::Global, 0, PrinterMode::Capture, 256 * 1024,
                                           1024 * 1024});
    messages.emplace_back(PrintState{11, PrinterMode::Ask, PrinterState::Full, 4096, 2});
    messages.emplace_back(PrintJobAdded{11, PrintJobInfo{5, PrintJobKind::Controller, 700, 21, 99}});
    messages.emplace_back(PrintJobFetch{11, 5});
    messages.emplace_back(PrintJobData{11, 5, 0, 1, std::string(2048, 'p')});
    messages.emplace_back(PrintJobDiscard{11, 0});

    return messages;
}

}  // namespace

CK_TEST(every_attach_mode_survives_the_wire_including_the_one_added_last) {
    // The protocol spec's rule, applied to the field it was written about. The catalogue
    // holds ONE `Attach`, so it proves one mode; `Attach.share` shipped with
    // WP-44 round-tripped by exactly that guard and set by no line in the
    // client, and "every alternative round-trips" turned out to be a claim
    // about the encoder rather than about anything a reader could reach.
    //
    // The producer half is `test_attach_flags.cpp`. This half is the byte.
    for (const AttachMode mode : {AttachMode::TakeOver, AttachMode::Join, AttachMode::Watch}) {
        Attach request;
        request.session = 7;
        request.columns = 120;
        request.rows = 40;
        request.mode = static_cast<std::uint8_t>(mode);
        request.host_sixel = 1;
        Message decoded;
        const std::string bytes = encode(request);
        CK_CHECK(decode(bytes, decoded).error == DecodeError::None);
        const auto* arrived = std::get_if<Attach>(&decoded);
        CK_CHECK(arrived != nullptr);
        if (arrived == nullptr) continue;
        CK_CHECK(arrived->mode == static_cast<std::uint8_t>(mode));
        // The neighbouring byte, because these two are adjacent on the wire and
        // a decoder that read them in the wrong order would pass every check
        // above for `Join` — whose value is 1, and so is this.
        CK_CHECK(arrived->host_sixel == 1);
    }
}

CK_TEST(every_message_in_the_catalogue_survives_the_wire) {
    const std::vector<Message> messages = every_message();
    for (const Message& message : messages) round_trip(message);
    // Every type, not merely every struct: a message added to the variant and
    // forgotten here would leave a hole nobody notices until the wire needs
    // it. The count is the catalogue's own size, so this fails when one is
    // added rather than drifting quietly.
    CK_CHECK(messages.size() == std::variant_size_v<Message>);
    std::vector<MessageType> seen;
    for (const Message& message : messages) {
        const MessageType type = type_of(message);
        for (const MessageType other : seen) CK_CHECK(other != type);
        seen.push_back(type);
    }
}

CK_TEST(the_modes_word_gives_every_facility_its_own_bits) {
    // One word carries the flags, the mouse tracking LEVEL and the kitty
    // keyboard enhancements, and two facilities sharing a bit would be a
    // multiplexer turning one of them on by mentioning the other. Checked as
    // arithmetic rather than by example, because the failure this prevents is
    // somebody adding a third field above the second one and stopping a bit
    // short.
    std::uint32_t flags = 0;
    for (const ModeBit bit :
         {ModeBit::MouseReporting, ModeBit::MouseEncodingSgr, ModeBit::BracketedPaste,
          ModeBit::ApplicationCursorKeys, ModeBit::FocusReporting, ModeBit::AlternateBuffer,
          ModeBit::AlternateScroll})
        flags |= static_cast<std::uint32_t>(bit);
    CK_CHECK((flags & kMouseTrackingMask) == 0u);
    CK_CHECK((flags & kKeyboardFlagsMask) == 0u);
    CK_CHECK((kMouseTrackingMask & kKeyboardFlagsMask) == 0u);
    // Five bits, which is every enhancement the kitty protocol defines: a mask
    // one bit short would drop `ReportAssociatedText` in silence.
    CK_CHECK((kKeyboardFlagsMask >> kKeyboardFlagsShift) == 0b11111u);

    // And the whole word survives the wire, which is what makes the layout
    // worth pinning here rather than in the client.
    GridDelta delta;
    delta.term = 3;
    delta.ops.emplace_back(ModesOp{0xFFFFFFFFu, flags | (2u << kMouseTrackingShift) |
                                                    (0b11111u << kKeyboardFlagsShift)});
    const Message decoded = round_trip(delta);
    const auto& modes = std::get<ModesOp>(std::get<GridDelta>(decoded).ops.front());
    CK_CHECK(((modes.values & kMouseTrackingMask) >> kMouseTrackingShift) == 2u);
    CK_CHECK(((modes.values & kKeyboardFlagsMask) >> kKeyboardFlagsShift) == 0b11111u);
}

CK_TEST(a_window_layout_says_the_same_thing_in_both_directions) {
    // A client reports an arrangement and the server states one back, and the
    // two use one entry shape on purpose: a field added to one direction and
    // forgotten in the other would round-trip perfectly in every test that only
    // ever looked at one of them, and would lose a window's place on exactly the
    // path this exists for.
    std::vector<LayoutEntry> arrangement;
    // Off the left edge and above the top, which is a real thing a reader can
    // drag a window into and the case WP-30's move rule is written for — and a
    // signed field that had been written as unsigned would come back as 65533
    // rather than -3, placing the window at the far side of a desktop nobody
    // has.
    arrangement.push_back(LayoutEntry{101, Rect{-3, -1, 40, 12}, 0, 0, TileFraction{}});
    arrangement.push_back(LayoutEntry{102, Rect{9, 5, 25, 8}, 1, 0, TileFraction{}});
    arrangement.push_back(LayoutEntry{103, Rect{0, 0, 80, 24}, 2, 1, TileFraction{}});
    // And the tile shares, which travel beside the rects rather than instead of
    // them (WP-30): the left half of a 50/50 split, a window that was floating,
    // and a maximized one. Zero extent is how the wire says "there was no
    // tiling", so the middle entry deliberately leaves it at the default.
    arrangement[0].tile = TileFraction{0, 0, kTileFractionWhole / 2, kTileFractionWhole};
    arrangement[2].tile = TileFraction{0, 0, kTileFractionWhole, kTileFractionWhole};

    SetLayout reported;
    reported.entries = arrangement;
    LayoutDelta stated;
    stated.session = 8;
    stated.entries = arrangement;
    stated.focused_term = 103;
    stated.desktop_columns = 80;
    stated.desktop_rows = 24;

    // Held rather than bound through the call: `std::get` hands back a
    // reference into the value, and a reference bound to one a function
    // returned is not lifetime-extended.
    const Message report_bytes = round_trip(reported);
    const Message statement_bytes = round_trip(stated);
    const auto& decoded_report = std::get<SetLayout>(report_bytes);
    const auto& decoded_statement = std::get<LayoutDelta>(statement_bytes);
    CK_CHECK(decoded_report.entries == decoded_statement.entries);
    CK_CHECK(decoded_report.entries.size() == 3U);
    CK_CHECK(decoded_report.entries[0].rect.x == -3);
    CK_CHECK(decoded_report.entries[0].rect.y == -1);
    // The order the client reported them in survives, because the z-order is a
    // field rather than a position — a decoder that sorted or reversed them
    // would put a maximized window under the ones it covers.
    CK_CHECK(decoded_report.entries[2].term == 103);
    CK_CHECK(decoded_report.entries[2].zoomed == 1);
    // The tile share survives both ways, and its absence is a value rather than
    // a gap: the floating window comes back saying "no tiling" and answers
    // `filled()` with false, which is what sends it down the move-to-fit path
    // instead of the proportional one.
    CK_CHECK(decoded_report.entries[0].tile.width == kTileFractionWhole / 2);
    CK_CHECK(decoded_report.entries[0].tile.height == kTileFractionWhole);
    CK_CHECK(decoded_report.entries[0].tile.filled());
    CK_CHECK(!decoded_report.entries[1].tile.filled());
    CK_CHECK(decoded_statement.entries[2].tile.filled());

    // A session whose last window closed reports no windows, which is a
    // statement and not an absence of one: a report that could not say "none
    // left" would leave the server holding the arrangement of a desktop that no
    // longer has anything on it.
    const Message empty_report = round_trip(SetLayout{});
    CK_CHECK(std::get<SetLayout>(empty_report).entries.empty());
}

CK_TEST(a_frame_states_its_own_length_type_and_zero_flags) {
    // The header, read by hand, because everything else in this file trusts
    // the decoder to read it and one test should not.
    const std::string bytes = encode(Ping{0x0102030405060708ull});
    CK_CHECK(bytes.size() == kHeaderBytes + 8);
    CK_CHECK(static_cast<unsigned char>(bytes[0]) == 8);  // payload_len, little-endian
    CK_CHECK(static_cast<unsigned char>(bytes[1]) == 0);
    CK_CHECK(static_cast<unsigned char>(bytes[2]) == 0);
    CK_CHECK(static_cast<unsigned char>(bytes[3]) == 0);
    CK_CHECK(static_cast<unsigned char>(bytes[4]) == 0x04);  // type Ping = 0x0004
    CK_CHECK(static_cast<unsigned char>(bytes[5]) == 0x00);
    CK_CHECK(static_cast<unsigned char>(bytes[6]) == 0);  // flags, reserved
    CK_CHECK(static_cast<unsigned char>(bytes[7]) == 0);
    // ...and the payload is little-endian too, low byte first.
    CK_CHECK(static_cast<unsigned char>(bytes[8]) == 0x08);
    CK_CHECK(static_cast<unsigned char>(bytes[15]) == 0x01);
}

CK_TEST(an_indexed_colour_survives_as_an_index_rather_than_as_pixels) {
    // The reason the protocol spec's cell says "tagged": a palette index flattened to
    // RGB here would make per-terminal re-theming impossible for good, and
    // would mean a reader's own terminal theme stopped applying to a child's
    // SGR 31 the moment ckmux was in the way (ckVision L-22).
    CellsOp op;
    op.row = 0;
    op.column = 0;
    op.runs = to_runs({cell("i", ckv::Color::indexed(31), ckv::Color::indexed(4)),
                       cell("r", ckv::Color::rgb(1, 2, 3), ckv::Color::rgb(250, 251, 252)),
                       cell("d")});
    GridDelta delta;
    delta.term = 1;
    delta.ops.emplace_back(std::move(op));
    const Message decoded = round_trip(delta);

    const auto& ops = std::get<GridDelta>(decoded).ops;
    CK_CHECK(ops.size() == 1U);
    const std::vector<ckv::Cell> cells = from_runs(std::get<CellsOp>(ops[0]).runs);
    CK_CHECK(cells.size() == 3U);
    CK_CHECK(cells[0].style().fg.is_indexed());
    if (cells[0].style().fg.is_indexed()) CK_CHECK(cells[0].style().fg.index() == 31);
    CK_CHECK(cells[1].style().fg.is_rgb());
    if (cells[1].style().fg.is_rgb()) CK_CHECK(cells[1].style().fg.r() == 1);
    CK_CHECK(cells[2].style().fg.is_default());
}

CK_TEST(a_continuation_cell_stays_the_far_half_of_a_wide_character) {
    // Rebuilt as a space it would turn every wide character on the screen
    // into two — the classic multiplexer bug the shared width authority
    // exists to prevent (the terminal-emulation spec).
    const std::vector<ckv::Cell> original{cell("漢"), ckv::Cell::continuation(ckv::Style{}), cell("a")};
    CellsOp op;
    op.runs = to_runs(original);
    GridDelta delta;
    delta.ops.emplace_back(std::move(op));
    const Message decoded = round_trip(delta);
    const std::vector<ckv::Cell> cells =
        from_runs(std::get<CellsOp>(std::get<GridDelta>(decoded).ops[0]).runs);
    CK_CHECK(cells.size() == 3U);
    CK_CHECK(cells[0].width() == 2);
    CK_CHECK(cells[1].is_continuation());
    CK_CHECK(!cells[2].is_continuation());
}

CK_TEST(runs_and_cells_are_inverses_and_a_run_is_shorter_than_what_it_stands_for) {
    std::vector<ckv::Cell> cells(300, cell(" "));
    cells[100] = cell("x");
    const std::vector<CellRun> runs = to_runs(cells);
    // Three runs for three stretches, not three hundred entries: this is the
    // whole reason the grid is run-length encoded.
    CK_CHECK(runs.size() == 3U);
    CK_CHECK(from_runs(runs).size() == cells.size());
    const std::vector<ckv::Cell> restored = from_runs(runs);
    for (std::size_t i = 0; i < cells.size(); ++i)
        CK_CHECK(restored[i].grapheme() == cells[i].grapheme());

    // A run cannot claim more than u16 holds, so a very long stretch becomes
    // several runs rather than one that wraps around to nothing.
    const std::vector<ckv::Cell> huge(70000, cell("."));
    const std::vector<CellRun> huge_runs = to_runs(huge);
    CK_CHECK(huge_runs.size() == 2U);
    CK_CHECK(from_runs(huge_runs).size() == huge.size());

    // And going the other way, a run set that sums past the largest grid this
    // protocol carries rebuilds nothing at all. Each of these claims 65535
    // cells in sixteen bytes, so a payload that passed every length check can
    // still ask for a grid nobody has the memory for — and `from_runs` sits
    // under a decode that promises never to throw, so the floor is here.
    std::vector<CellRun> past_any_grid(64, CellRun{0xFFFFu, cell("x")});
    CK_CHECK(from_runs(past_any_grid).empty());
    // The ceiling is not a blanket refusal: a run set that fits still rebuilds.
    past_any_grid.resize(4);
    CK_CHECK(from_runs(past_any_grid).size() == 4U * 0xFFFFu);
}

CK_TEST(a_cells_ops_size_is_the_exact_number_of_grid_delta_payload_bytes_it_adds) {
    CellsOp op;
    op.row = 5;
    op.column = 6;
    op.runs = to_runs({cell("x"), cell("x"), cell("漢"), cell("z")});
    GridDelta delta;
    delta.term = 7;
    delta.seq = 9;
    delta.ops.push_back(op);

    // GridDelta's fixed payload is term + seq + op count. Everything after
    // that is the CellsOp contribution the sparse-row partitioner compares.
    constexpr std::size_t kGridDeltaFixedPayload = 8 + 4 + 4;
    CK_CHECK(encode(delta).size() ==
             kHeaderBytes + kGridDeltaFixedPayload + encoded_size(op));
}

// --- Properties -----------------------------------------------------------

CK_TEST(random_valid_messages_survive_the_wire) {
    // Structure varies, not just values: lengths, run counts, string
    // contents and which ops a delta carries. A fixed shape would only ever
    // exercise one path through the length handling.
    std::mt19937 random(20260817u);
    const auto byte = [&random] { return static_cast<char>(random() & 0xFFu); };

    for (int iteration = 0; iteration < 400; ++iteration) {
        GridDelta delta;
        delta.term = (static_cast<std::uint64_t>(random()) << 32) | random();
        delta.seq = random();
        const int ops = static_cast<int>(random() % 6);
        for (int op = 0; op < ops; ++op) {
            switch (random() % 7) {
                case 0:
                    delta.ops.emplace_back(ScrollOp{static_cast<std::uint16_t>(random()),
                                                    static_cast<std::uint16_t>(random()),
                                                    static_cast<std::int16_t>(random())});
                    break;
                case 1: {
                    CellsOp cells_op;
                    cells_op.row = static_cast<std::uint16_t>(random());
                    cells_op.column = static_cast<std::uint16_t>(random());
                    std::vector<ckv::Cell> cells;
                    const int count = static_cast<int>(random() % 40);
                    for (int i = 0; i < count; ++i) {
                        ckv::Style style;
                        style.fg = (random() % 3) == 0   ? ckv::Color::default_color()
                                   : (random() % 2) == 0 ? ckv::Color::indexed(static_cast<std::uint8_t>(random()))
                                                         : ckv::Color::rgb(static_cast<std::uint8_t>(random()),
                                                                           static_cast<std::uint8_t>(random()),
                                                                           static_cast<std::uint8_t>(random()));
                        style.attrs = static_cast<ckv::Attr>(random() & 0x3Fu);
                        cells.push_back((random() % 8) == 0 ? ckv::Cell::continuation(style)
                                                            : ckv::Cell::from_grapheme("q", style));
                    }
                    cells_op.runs = to_runs(cells);
                    delta.ops.emplace_back(std::move(cells_op));
                    break;
                }
                case 2:
                    delta.ops.emplace_back(CursorOp{static_cast<std::uint16_t>(random()),
                                                    static_cast<std::uint16_t>(random()),
                                                    static_cast<std::uint8_t>(random() % 4),
                                                    static_cast<std::uint8_t>(random() % 2),
                                                    static_cast<std::uint8_t>(random() % 2)});
                    break;
                case 3: delta.ops.emplace_back(ModesOp{
                    static_cast<std::uint32_t>(random()),
                    static_cast<std::uint32_t>(random())}); break;
                case 4: {
                    std::string title;
                    const int length = static_cast<int>(random() % 64);
                    for (int i = 0; i < length; ++i) title.push_back(static_cast<char>('a' + (random() % 26)));
                    delta.ops.emplace_back(TitleOp{std::move(title)});
                    break;
                }
                case 5:
                    delta.ops.emplace_back(ResizeOp{static_cast<std::uint16_t>(random()),
                                                    static_cast<std::uint16_t>(random())});
                    break;
                default: {
                    ScrollbackPushOp push;
                    const int lines = static_cast<int>(random() % 4);
                    for (int line = 0; line < lines; ++line)
                        push.lines.push_back(to_runs({cell("s"), cell("b")}));
                    delta.ops.emplace_back(std::move(push));
                    break;
                }
            }
        }
        round_trip(delta);

        // A blob carrying arbitrary bytes, including embedded zeros, since
        // child input is exactly that and a length-prefixed field is the only
        // reason it can be.
        std::string bytes;
        const int length = static_cast<int>(random() % 200);
        for (int i = 0; i < length; ++i) bytes.push_back(byte());
        round_trip(Input{delta.term, bytes});
    }
}

CK_TEST(random_bytes_are_refused_and_never_crash) {
    // The adversarial half. Nothing here should be decodable; what matters is
    // that every rejection is a value returned, not a throw, a read past the
    // end, or a message left half-filled.
    std::mt19937 random(1234567u);
    int decoded_anything = 0;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        std::string bytes;
        const int length = static_cast<int>(random() % 64);
        for (int i = 0; i < length; ++i) bytes.push_back(static_cast<char>(random() & 0xFFu));
        Message message;
        const DecodeResult result = decode(bytes, message);
        if (result.ok()) ++decoded_anything;
    }
    // Random noise decoding as a valid frame is not impossible — a short
    // payload of the right type could be — so this asserts the shape of the
    // outcome rather than that it never happens: nothing crashed, and the
    // overwhelming majority was refused.
    CK_CHECK(decoded_anything < 100);
}

CK_TEST(a_truncated_or_corrupted_frame_is_refused_rather_than_half_read) {
    const std::string whole = encode(Attach{7, 120, 40, 1080, 720});
    // Every prefix of a real frame: each is either incomplete or refused, and
    // none of them yields a message.
    for (std::size_t length = 0; length < whole.size(); ++length) {
        Message message;
        const DecodeResult result = decode(std::string_view(whole).substr(0, length), message);
        CK_CHECK(!result.ok());
        CK_CHECK(result.error == DecodeError::Incomplete);
    }
    // And every single-byte corruption of the payload either decodes to
    // something (a field's value changed, which the wire cannot detect and
    // does not claim to) or is refused — never a crash.
    for (std::size_t index = kHeaderBytes; index < whole.size(); ++index) {
        std::string corrupted = whole;
        corrupted[index] = static_cast<char>(corrupted[index] ^ 0xFF);
        Message message;
        (void)decode(corrupted, message);
    }
}

CK_TEST(a_message_left_unset_stays_unset_when_a_frame_is_refused) {
    // A decoder that half-fills its output is worse than one that fails: the
    // caller cannot tell which fields it may trust.
    Message message = Ping{42};
    const std::string bad = encode(Attach{1, 2, 3, 4, 5}).substr(0, kHeaderBytes + 3);
    CK_CHECK(!decode(bad, message).ok());
    CK_CHECK(std::holds_alternative<Ping>(message));
    CK_CHECK(std::get<Ping>(message).nonce == 42U);
}

// --- Framing: what a socket actually delivers ------------------------------

CK_TEST(a_frame_split_across_reads_is_reassembled) {
    const std::string frame = encode(TermOpened{11, 4, 2, Rect{1, 1, 40, 12}, "zsh", 80, 24});
    FrameReader reader;
    Message message;
    // One byte at a time is the worst case a socket can hand over, and the
    // only one that exercises every partial state of the header.
    for (std::size_t i = 0; i + 1 < frame.size(); ++i) {
        CK_CHECK(reader.append(std::string_view(frame).substr(i, 1)));
        CK_CHECK(reader.next(message) == DecodeError::Incomplete);
    }
    CK_CHECK(reader.append(std::string_view(frame).substr(frame.size() - 1)));
    CK_CHECK(reader.next(message) == DecodeError::None);
    CK_CHECK(std::get<TermOpened>(message).title == "zsh");
    CK_CHECK(reader.buffered() == 0U);
}

CK_TEST(several_frames_coalesced_into_one_read_come_out_one_at_a_time) {
    std::string stream;
    stream += encode(Ping{1});
    stream += encode(Ping{2});
    stream += encode(Input{5, "keys"});
    FrameReader reader;
    CK_CHECK(reader.append(stream));

    Message message;
    CK_CHECK(reader.next(message) == DecodeError::None);
    CK_CHECK(std::get<Ping>(message).nonce == 1U);
    CK_CHECK(reader.next(message) == DecodeError::None);
    CK_CHECK(std::get<Ping>(message).nonce == 2U);
    CK_CHECK(reader.next(message) == DecodeError::None);
    CK_CHECK(std::get<Input>(message).bytes == "keys");
    CK_CHECK(reader.next(message) == DecodeError::Incomplete);
    CK_CHECK(reader.buffered() == 0U);
}

CK_TEST(a_frame_and_a_half_leaves_the_half_for_the_next_read) {
    // The ordinary case, and the one a hand-rolled loop gets wrong: a read
    // that ends mid-frame must not lose the tail.
    const std::string first = encode(Ping{7});
    const std::string second = encode(Pong{8});
    FrameReader reader;
    CK_CHECK(reader.append(first + second.substr(0, 4)));
    Message message;
    CK_CHECK(reader.next(message) == DecodeError::None);
    CK_CHECK(std::get<Ping>(message).nonce == 7U);
    CK_CHECK(reader.next(message) == DecodeError::Incomplete);
    CK_CHECK(reader.buffered() == 4U);
    CK_CHECK(reader.append(second.substr(4)));
    CK_CHECK(reader.next(message) == DecodeError::None);
    CK_CHECK(std::get<Pong>(message).nonce == 8U);
}

// --- Caps: a hostile peer costs a connection, not the process --------------

CK_TEST(a_payload_past_its_cap_is_refused_before_its_bytes_are_waited_for) {
    // The order matters: a peer claiming a gigabyte must be refused on the
    // strength of the claim, not after a reader has buffered a gigabyte in
    // order to refuse it.
    std::string header;
    header.push_back(static_cast<char>(0x01));  // payload_len = 0x02000001, past 1 MiB
    header.push_back(static_cast<char>(0x00));
    header.push_back(static_cast<char>(0x00));
    header.push_back(static_cast<char>(0x02));
    header.push_back(static_cast<char>(0x04));  // type Ping
    header.push_back(static_cast<char>(0x00));
    header.push_back(static_cast<char>(0x00));  // flags
    header.push_back(static_cast<char>(0x00));
    Message message;
    const DecodeResult result = decode(header, message);
    CK_CHECK(result.error == DecodeError::PayloadTooLarge);

    // And the stream reader refuses to grow past one frame's worth, so the
    // same claim cannot be turned into unbounded buffering by dribbling.
    FrameReader reader;
    CK_CHECK(reader.append(header));
    CK_CHECK(reader.next(message) == DecodeError::PayloadTooLarge);
}

CK_TEST(a_snapshot_may_be_large_where_a_control_message_may_not) {
    // Three caps, one per kind of message, because a snapshot legitimately
    // carries a screen and its history while a Ping never carries anything.
    const auto framed = [](MessageType type, std::uint32_t payload_length) {
        std::string bytes;
        bytes.push_back(static_cast<char>(payload_length & 0xFFu));
        bytes.push_back(static_cast<char>((payload_length >> 8) & 0xFFu));
        bytes.push_back(static_cast<char>((payload_length >> 16) & 0xFFu));
        bytes.push_back(static_cast<char>((payload_length >> 24) & 0xFFu));
        bytes.push_back(static_cast<char>(static_cast<std::uint16_t>(type) & 0xFFu));
        bytes.push_back(static_cast<char>((static_cast<std::uint16_t>(type) >> 8) & 0xFFu));
        bytes.push_back(static_cast<char>(0));
        bytes.push_back(static_cast<char>(0));
        return bytes;
    };
    Message message;
    // Two megabytes: too much for a control message, fine for a snapshot —
    // which then reports Incomplete, because the cap passed and the bytes did
    // not arrive.
    CK_CHECK(decode(framed(MessageType::Ping, 2u * 1024u * 1024u), message).error ==
             DecodeError::PayloadTooLarge);
    CK_CHECK(decode(framed(MessageType::Attached, 2u * 1024u * 1024u), message).error ==
             DecodeError::Incomplete);
    // Past even the snapshot cap, it is refused again.
    CK_CHECK(decode(framed(MessageType::Attached, 17u * 1024u * 1024u), message).error ==
             DecodeError::PayloadTooLarge);
    // An image chunk is bounded by being a chunk.
    CK_CHECK(decode(framed(MessageType::ImageChunk, 512u * 1024u), message).error ==
             DecodeError::PayloadTooLarge);
}

CK_TEST(an_unknown_type_and_a_nonzero_flag_are_both_connection_errors) {
    // There is one version integer, so neither can legitimately occur: an
    // unknown type is not a forward-compatible extension to skip, and a
    // reserved bit that is set means the peer is speaking something else
    // (the protocol spec, invariant 4).
    std::string bytes = encode(Ping{1});
    bytes[4] = static_cast<char>(0xEE);  // a type nothing declares
    bytes[5] = static_cast<char>(0xEE);
    Message message;
    CK_CHECK(decode(bytes, message).error == DecodeError::UnknownType);

    bytes = encode(Ping{1});
    bytes[6] = static_cast<char>(0x01);  // reserved flags
    CK_CHECK(decode(bytes, message).error == DecodeError::ReservedFlags);
}

CK_TEST(a_payload_with_bytes_left_over_is_refused) {
    // Every message has a fixed shape, so a remainder means the sender and
    // this decoder disagree about what the type means — the desync that must
    // not be papered over.
    const std::string good = encode(Ping{1});
    std::string padded = good;
    padded.push_back('!');
    padded[0] = static_cast<char>(static_cast<unsigned char>(padded[0]) + 1);  // claim the extra byte
    Message message;
    CK_CHECK(decode(padded, message).error == DecodeError::TrailingBytes);
}

CK_TEST(a_field_that_cannot_mean_anything_is_refused) {
    // Values the structs cannot represent, checked at the boundary rather
    // than clamped: a detach reason nobody defined would otherwise become
    // "user" and a reader would never learn their peer said something
    // impossible.
    std::string bytes = encode(Detached{DetachReason::User, "x"});
    bytes[kHeaderBytes] = static_cast<char>(9);  // no such reason
    Message message;
    CK_CHECK(decode(bytes, message).error == DecodeError::Malformed);

    // A zero-length cell run says nothing and would let a peer pad a message
    // with entries that cost the decoder work and the mirror nothing.
    CellsOp op;
    op.runs = to_runs({cell("a")});
    GridDelta delta;
    delta.ops.emplace_back(op);
    std::string delta_bytes = encode(delta);
    // term(8) seq(4) ops(4) tag(1) row(2) col(2) run count(4) then run_length.
    const std::size_t run_length_at = kHeaderBytes + 8 + 4 + 4 + 1 + 2 + 2 + 4;
    // Proven to be that field rather than asserted: writing 1 there leaves a
    // frame that still decodes, so a Malformed from writing 0 is this rule
    // firing and not a byte offset that landed somewhere else by luck.
    delta_bytes[run_length_at] = 1;
    delta_bytes[run_length_at + 1] = 0;
    CK_CHECK(decode(delta_bytes, message).ok());
    delta_bytes[run_length_at] = 0;
    CK_CHECK(decode(delta_bytes, message).error == DecodeError::Malformed);
}

CK_TEST(a_count_larger_than_the_payload_is_refused_before_anything_is_reserved) {
    // The allocation-guard case: a peer claiming four billion entries in a
    // sixteen-byte payload must be refused on arithmetic, not after a
    // reserve() the machine cannot satisfy.
    SessionList list;
    list.sessions.push_back(SessionInfo{1, "s", 0, 0, 0, 0});
    std::string bytes = encode(list);
    bytes[kHeaderBytes] = static_cast<char>(0xFF);
    bytes[kHeaderBytes + 1] = static_cast<char>(0xFF);
    bytes[kHeaderBytes + 2] = static_cast<char>(0xFF);
    bytes[kHeaderBytes + 3] = static_cast<char>(0x0F);
    Message message;
    CK_CHECK(decode(bytes, message).error == DecodeError::Malformed);
}

CK_TEST(a_count_is_bounded_by_what_an_entry_actually_costs_not_by_one_byte_each) {
    // The other half of the same guard, and the one a "count <= bytes left"
    // check lets through: a terminal state is sixty-two bytes at its very
    // smallest, so a payload with a hundred bytes left in it cannot honestly
    // announce a hundred of them. It could, and the reserve that followed was
    // some hundreds of megabytes out of a four-megabyte frame — a bad_alloc
    // from a decode that promises never to throw (13-architecture-review,
    // M-P2).
    Attached attached;
    attached.session = 4;
    TerminalState state;
    state.term = 11;
    state.columns = 2;
    state.rows = 1;
    state.grid = to_runs({cell("a"), cell("b")});
    attached.snapshot.terminals.push_back(std::move(state));
    std::string bytes = encode(attached);
    CK_CHECK(!bytes.empty());

    // session u64, desktop cols u16, desktop rows u16, focused u64 — then the
    // terminal count, and after it everything that is left.
    const std::size_t count_at = kHeaderBytes + 8 + 2 + 2 + 8;
    const std::uint32_t left = static_cast<std::uint32_t>(bytes.size() - count_at - 4);
    // Exactly the number of bytes remaining: legal under "one byte per entry",
    // and an announcement of `left` terminal states in `left` bytes.
    CK_CHECK(left > 1U);
    bytes[count_at] = static_cast<char>(left & 0xFFu);
    bytes[count_at + 1] = static_cast<char>((left >> 8) & 0xFFu);
    bytes[count_at + 2] = static_cast<char>((left >> 16) & 0xFFu);
    bytes[count_at + 3] = static_cast<char>((left >> 24) & 0xFFu);
    Message message;
    CK_CHECK(decode(bytes, message).error == DecodeError::Malformed);

    // And a count of one still decodes, so the refusal above is the arithmetic
    // firing rather than the offset having landed on some other field.
    bytes[count_at] = static_cast<char>(1);
    bytes[count_at + 1] = static_cast<char>(0);
    bytes[count_at + 2] = static_cast<char>(0);
    bytes[count_at + 3] = static_cast<char>(0);
    CK_CHECK(decode(bytes, message).ok());
}

CK_TEST(a_string_longer_than_its_length_field_is_refused_rather_than_truncated) {
    // What this replaces: the length wrapped to its low sixteen bits and every
    // byte was appended anyway, so the peer read a prefix as the title and the
    // remainder as whatever field came next — a desync manufactured by the
    // sender, at the one layer whose whole job is that the two ends agree
    // (13-architecture-review, m-str).
    GridDelta delta;
    delta.term = 11;
    delta.seq = 1;
    delta.ops.emplace_back(TitleOp{std::string(70000, 't')});
    bool oversize = false;
    const std::string refused = encode(delta, &oversize);
    CK_CHECK(refused.empty());
    CK_CHECK(oversize);

    // A payload past the cap its own type is held to is refused the same way,
    // because a frame over the cap is not a large frame — it is one the peer's
    // decoder will not read, and a sender that finds that out at the socket
    // has already lost the message and the connection with it (C1).
    oversize = false;
    CK_CHECK(encode(Input{11, std::string(2u * 1024u * 1024u, 'x')}, &oversize).empty());
    CK_CHECK(oversize);

    // And an ordinary message still encodes, with the flag left alone: this is
    // an out-parameter for the one caller that can produce an oversize frame,
    // not a new outcome every call site has to think about.
    oversize = true;
    const std::string good = encode(Ping{1}, &oversize);
    CK_CHECK(good.size() == kHeaderBytes + 8);
    CK_CHECK(!oversize);
}

CK_TEST(an_error_code_this_build_does_not_define_survives_the_wire_intact) {
    // The old-client-meets-new-server case, which no build in this fleet
    // reproduces: both ends of every other test here know every code, so none
    // of them can tell a decoder that passes an unknown value through from one
    // that rejects the frame. The difference matters — a pass-through shows the
    // reader a correct sentence under a generic category, while a rejection
    // drops the connection over a message the server was right to send.
    //
    // Written when WP-13 added `LimitReached = 5`. The value below is not a
    // code this build defines and is not expected to become one; what is being
    // pinned is that `Error::code` is a `std::uint16_t` on the wire and in the
    // struct, decoded with no validation against the enum, so a code added by
    // a NEWER peer arrives whole rather than as a protocol error.
    ckm::proto::Error future;
    future.code = 9999;
    future.context = "SomethingThisBuildHasNeverHeardOf";
    future.human = "the server said something newer than this client";

    const std::string bytes = ckm::proto::encode(future);
    ckm::proto::Message decoded;
    const ckm::proto::DecodeResult result = ckm::proto::decode(bytes, decoded);
    CK_CHECK(result.error == ckm::proto::DecodeError::None);
    const auto* const back = std::get_if<ckm::proto::Error>(&decoded);
    CK_CHECK(back != nullptr);
    if (back != nullptr) {
        CK_CHECK(back->code == 9999);
        // The sentence is the part a reader actually reads, and it is the part
        // that must survive: a client that cannot name the category can still
        // show what the server said.
        CK_CHECK(back->human == future.human);
        CK_CHECK(back->context == future.context);
    }
}

CK_TEST(the_size_predicted_for_a_terminal_state_is_the_size_actually_written) {
    // `encoded_size(const TerminalState&)` predicts from a hardcoded `kFixed`
    // plus the variable parts. Nothing checks that constant against the writer
    // it is predicting, so a field added to the struct and to `write` without
    // a matching adjustment leaves the predictor short — and a predictor that
    // under-reports is what makes a buffer too small and framing go wrong
    // under exactly the conditions nobody tests.
    //
    // That happened when WP-41 added the two serials: `kFixed` stayed at 81,
    // ran eight bytes short per terminal, and surfaced in the DIFF ENGINE's
    // snapshot-size case — a suite away from the change, and only because that
    // case happened to compare a frame against its prediction. This one lives
    // beside the codec so the next wire field fails where it was added.
    //
    // Every field is set to something non-default on purpose: a state left
    // empty would let the predictor forget any field whose default encodes to
    // zero bytes and still agree.
    ckm::proto::Attached message;
    message.session = 3;
    ckm::proto::TerminalState state;
    state.term = 12;
    state.index = 4;
    state.title = "a title";
    state.custom_title = "a reader's name";
    state.flags = 3;
    state.columns = 80;
    state.rows = 24;
    state.rect = ckm::proto::Rect{1, 2, 30, 10};
    state.z_order = 2;
    state.zoomed = 1;
    state.cursor.row = 3;
    state.cursor.column = 5;
    state.clipboard_serial = 9;
    state.exited = 1;
    state.exit_status = 7;
    state.hold = 1;
    state.diagnostic = "something to say";
    state.bell_serial = 11;
    state.activity_serial = 13;
    message.snapshot.terminals.push_back(std::move(state));

    // The frame the codec actually produces, against what it said it would be.
    // `Attached` writes the session id (8) after the header and then the
    // snapshot, which is the same arithmetic test_diff_engine uses — stated
    // here against a fully populated state rather than a bare one.
    const std::string frame = ckm::proto::encode(message);
    CK_CHECK(frame.size() == ckm::proto::kHeaderBytes + 8 +
                                 ckm::proto::encoded_size(message.snapshot));
}
