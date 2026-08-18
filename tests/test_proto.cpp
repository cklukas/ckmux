// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The wire protocol (plans/05-protocol.md, WP-1). No socket, no server, no
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
    state.flags = static_cast<std::uint8_t>(TermMetaFlag::Bell) |
                  static_cast<std::uint8_t>(TermMetaFlag::Activity);
    state.columns = 80;
    state.rows = 24;
    state.rect = Rect{-2, 3, 40, 12};
    state.z_order = 5;
    state.zoomed = 1;
    state.cursor = CursorOp{7, 9, 2, 1, 1};
    state.modes = static_cast<std::uint32_t>(ModeBit::MouseReporting) |
                  static_cast<std::uint32_t>(ModeBit::MouseEncodingSgr) |
                  static_cast<std::uint32_t>(ModeBit::AlternateBuffer);
    state.grid = to_runs({cell("a"), cell("a"), cell("b", ckv::Color::indexed(31)),
                          ckv::Cell::continuation(ckv::Style{})});
    state.scrollback = {to_runs({cell("o"), cell("l"), cell("d")}), to_runs({cell("e"), cell("r")})};
    state.images = {77, 78};
    state.printer_mode = PrinterMode::Capture;
    state.printer_scope = PrinterScope::Session;
    state.printer_state = PrinterState::Capturing;
    state.printer_bytes = 12345;
    state.print_jobs.push_back(PrintJobInfo{1, PrintJobKind::Autoprint, 900, 30, -7});
    attached.snapshot.terminals.push_back(std::move(state));
    messages.emplace_back(std::move(attached));

    messages.emplace_back(Detached{DetachReason::Takeover, "another client attached"});

    LayoutDelta layout;
    layout.session = 4;
    layout.entries.push_back(LayoutEntry{11, Rect{1, 2, 30, 10}, 1, 0});
    layout.entries.push_back(LayoutEntry{12, Rect{-5, 0, 20, 8}, 2, 1});
    layout.focused_term = 12;
    layout.desktop_columns = 100;
    layout.desktop_rows = 30;
    messages.emplace_back(std::move(layout));

    messages.emplace_back(TermOpened{11, 4, 2, Rect{1, 1, 40, 12}, "zsh", 80, 24});
    messages.emplace_back(TermClosed{11, 1, -9, 1});
    messages.emplace_back(TermMeta{11, 2, "make", static_cast<std::uint8_t>(TermMetaFlag::Activity)});
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
    messages.emplace_back(RenameTerminal{11, "build"});
    messages.emplace_back(Input{11, std::string("\x1b[A\x00 bytes", 10)});
    messages.emplace_back(PasteChunk{11, 3, 1, "pasted text"});
    messages.emplace_back(PasteAck{3});

    GridDelta delta;
    delta.term = 11;
    delta.seq = 4242;
    delta.ops.emplace_back(ScrollOp{0, 24, 3});
    delta.ops.emplace_back(ScrollOp{2, 20, -1});
    delta.ops.emplace_back(CellsOp{5, 6, to_runs({cell("x"), cell("x"), cell("y")})});
    delta.ops.emplace_back(CursorOp{1, 2, 1, 0, 1});
    delta.ops.emplace_back(ModesOp{0xFFu, 0x0Fu});
    delta.ops.emplace_back(TitleOp{"a new title"});
    delta.ops.emplace_back(ScrollbackPushOp{{to_runs({cell("s"), cell("b")})}});
    messages.emplace_back(std::move(delta));

    messages.emplace_back(ImageAddBegin{77, 640, 480});
    messages.emplace_back(ImageChunk{77, 1, std::string(1024, '\xAB')});
    messages.emplace_back(ImageEnd{77});
    messages.emplace_back(ImagePlace{11, 77, Rect{4, 5, 10, 6}, -3, 7});
    messages.emplace_back(ImageRemove{11, 77});
    messages.emplace_back(ClipboardSet{"copied out of a program"});
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
    // The reason plans/05's cell says "tagged": a palette index flattened to
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
    // exists to prevent (plans/04).
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
            switch (random() % 6) {
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
                case 3: delta.ops.emplace_back(ModesOp{random(), random()}); break;
                case 4: {
                    std::string title;
                    const int length = static_cast<int>(random() % 64);
                    for (int i = 0; i < length; ++i) title.push_back(static_cast<char>('a' + (random() % 26)));
                    delta.ops.emplace_back(TitleOp{std::move(title)});
                    break;
                }
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
    // (plans/05, invariant 4).
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
