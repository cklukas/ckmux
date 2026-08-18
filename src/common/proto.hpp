// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The client⇄server wire protocol (plans/05-protocol.md).
//
// One definition of every message, used by both sides — the server encodes
// what the client decodes out of the same struct, so the two cannot come to
// disagree about a field's order or width. Nothing here knows about a socket:
// encoding produces bytes and decoding consumes them, which is why the whole
// catalogue is testable without a server, a PTY or a clock (plans/09).
//
// Three rules shape all of it.
//
// **A decoder never trusts its input.** Every read is bounds-checked against
// what is actually there, every length is checked against a cap before it is
// used to size anything, and a malformed frame is an error value rather than
// an exception, a crash, or a partially-filled struct. A hostile or corrupt
// peer costs a connection and nothing else (plans/05, invariant 2).
//
// **Recovery is universal.** There is no repair path for a desynchronised
// mirror: any protocol error drops the connection, the client reconnects and
// re-snapshots. That is why the decoders can be strict — being strict costs a
// reconnect, and being lenient costs a mirror that is subtly wrong for hours.
//
// **Pre-1.0 there is one version integer**, bumped on any wire change. An
// unknown message type is therefore not a forward-compatible extension to be
// skipped; it cannot legitimately occur, so it is an error (invariant 4).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cvision/core/cell.hpp"
#include "cvision/core/cursor.hpp"
#include "cvision/core/geometry.hpp"
#include "cvision/core/terminal_subsession.hpp"

namespace ckm::proto {

// Bumped on ANY wire change, pre-1.0, because there is nothing finer to bump.
// A mismatch is answered with `Refuse` and the remedy in words, since an
// upgrade leaves old servers running and this message will be seen.
//
// v2: `Attach.host_sixel` — the client's outer terminal reported Sixel, so
// the server may advertise it to the children this client opens (WP-16).
//
// v3, four changes in one bump because none of them shipped under a version of
// their own: `GridDelta`'s `Resize` op, which is what stops a shrinking
// terminal from leaving stale rows on a mirror (13-architecture-review C3);
// two mouse-tracking bits in `ModesOp`'s word, so a client encodes DEC 1000,
// 1002 and 1003 as the three levels they are rather than as one switch; and
// `CloseTerminal`'s `force`/`grace_seconds` with `MoveTerminal`'s
// `to_new_session`, which changed the wire when close-with-grace landed and
// were still riding on v2.
//
// v3 also carries what a reattaching reader was losing, and it EXTENDS v3
// rather than bumping to v4 because v3 has shipped nowhere: the version integer
// exists so two builds can tell whether they can talk, and two numbers for a
// wire nobody has ever spoken would say the same thing twice. What was added
// (R8 in 13-architecture-review): the kitty keyboard flags as five bits of
// `ModesOp`'s word, beside the mouse-tracking level; `TerminalState`'s
// `clipboard_serial`, its `exited`/`exit_status`/`hold`, and its latest
// diagnostic; `ClipboardSet`'s terminal id, so a write can be routed into the
// terminal that asked for it rather than guessed at; and `TermDiagnostic`, the
// message that carries a terminal's newest complaint while a client is
// watching.
inline constexpr std::uint32_t kProtocolVersion = 3;

// What `Hello` and `HelloAck` put in their `build` field. Not a version check —
// `kProtocolVersion` is the only thing that decides whether two ends can talk —
// but a reader diagnosing a refusal wants to know which two builds met, and a
// log line naming them is the difference between a bug report and a shrug.
inline constexpr std::string_view kBuildIdentity = "ckmux 0.0.1";

// u32 payload_len, u16 type, u16 flags.
inline constexpr std::size_t kHeaderBytes = 8;

// Caps enforced on decode, before any allocation sized by the value being
// read. A frame past its cap is a protocol error, not a truncation: the peer
// has said something this version of the protocol cannot mean.
inline constexpr std::uint32_t kMaxPayloadBytes = 1024u * 1024u;             // everything...
inline constexpr std::uint32_t kMaxChunkPayloadBytes = 256u * 1024u;        // ...except image/job chunks
inline constexpr std::uint32_t kMaxSnapshotPayloadBytes = 16u * 1024u * 1024u;  // ...and a full snapshot
// What a SENDER holds a snapshot to, as against `kMaxSnapshotPayloadBytes`,
// which is what a DECODER will still accept. They are different numbers on
// purpose: a decoder must take whatever a legitimate build of this program
// produced, while a sender must leave itself room to be wrong. Measured before
// this existed: 100 columns and the default ten-thousand-line history made a
// 16.29 MiB `Attached`, past the decoder's own limit — the client exited with
// "the server sent something this build cannot read" and the session was
// permanently unattachable (13-architecture-review, C1).
//
// Four megabytes is thousands of lines of real text once it is run-length
// encoded, and it is also what keeps an attach quick: a reader waiting for a
// desktop is waiting for the screens, not for history they may never page to.
inline constexpr std::uint32_t kSnapshotPayloadBudget = 4u * 1024u * 1024u;
// The largest a single string or blob field may claim. Bounded separately
// from the frame so that a decoder rejects an absurd length before it
// reserves anything, rather than after.
inline constexpr std::uint32_t kMaxStringBytes = 64u * 1024u;
// A cell holds one grapheme cluster. Fifteen bytes is four codepoints of
// combining marks past the base — more than any real cluster and less than a
// blob a peer could use to make a grid enormous one cell at a time.
inline constexpr std::size_t kMaxCellTextBytes = 15;
// A window title is a line of text. Four kilobytes is far past any real one
// and far under the field's own two-byte length, so a child that writes a
// megabyte through OSC 2 costs a truncated title rather than a delta that
// cannot be put on the wire at all.
inline constexpr std::size_t kMaxTitleBytes = 4096;

// The largest grid this protocol will carry, and therefore the largest a
// terminal on it may become. One definition, both ends: a decoder will not
// rehydrate more cells than this out of a run set however the run lengths sum,
// and a mirror refuses a geometry past it rather than sizing itself by it — so
// neither end can be talked into an allocation by the other's arithmetic. The
// sizes on the wire are two `u16`s, so without a ceiling somewhere a peer asks
// for 65535 by 65535 — four billion cells out of a kilobyte of frame — and is
// granted it.
inline constexpr std::uint16_t kMaxGridColumns = 1000;
inline constexpr std::uint16_t kMaxGridRows = 1000;
inline constexpr std::size_t kMaxGridCells =
    static_cast<std::size_t>(kMaxGridColumns) * static_cast<std::size_t>(kMaxGridRows);

// 0x00xx control · 0x01xx session ops · 0x02xx terminal I/O · 0x03xx state
// sync · 0x04xx events and errors. The range says at a glance which half of
// the system a message belongs to, and a value's numeric neighbours are its
// conceptual ones.
enum class MessageType : std::uint16_t {
    // --- 0x00xx control ---
    Hello = 0x0001,
    HelloAck = 0x0002,
    Refuse = 0x0003,
    Ping = 0x0004,
    Pong = 0x0005,

    // --- 0x01xx session operations ---
    ListSessions = 0x0101,
    NewSession = 0x0102,
    Attach = 0x0103,
    Detach = 0x0104,
    ClientResize = 0x0105,
    RenameSession = 0x0106,
    KillSession = 0x0107,
    KillServer = 0x0108,
    SessionList = 0x0109,
    SessionsChanged = 0x010A,
    Attached = 0x010B,
    Detached = 0x010C,

    // --- 0x02xx terminal I/O and lifecycle ---
    NewTerminal = 0x0201,
    CloseTerminal = 0x0202,
    KillTerminal = 0x0203,
    RespawnTerminal = 0x0204,
    MoveTerminal = 0x0205,
    MoveResize = 0x0206,
    Raise = 0x0207,
    FocusTerm = 0x0208,
    ZoomTerm = 0x0209,
    RenameTerminal = 0x020A,
    Input = 0x020B,
    PasteChunk = 0x020C,
    PasteAck = 0x020D,

    // --- 0x03xx state sync ---
    LayoutDelta = 0x0301,
    TermOpened = 0x0302,
    TermClosed = 0x0303,
    TermMeta = 0x0304,
    GridDelta = 0x0305,
    ImageAddBegin = 0x0306,
    ImageChunk = 0x0307,
    ImageEnd = 0x0308,
    ImagePlace = 0x0309,
    ImageRemove = 0x030A,
    ClipboardSet = 0x030B,
    TermDiagnostic = 0x030C,

    // --- 0x04xx events, errors, printer ---
    Error = 0x0401,
    SetPrinterPolicy = 0x0402,
    PrintState = 0x0403,
    PrintJobAdded = 0x0404,
    PrintJobFetch = 0x0405,
    PrintJobData = 0x0406,
    PrintJobDiscard = 0x0407,
};

// Which end a connection is: the server sends no state-sync messages to a
// `cli` client beyond direct replies (plans/05, CLI mapping).
enum class ClientKind : std::uint8_t { Ui = 0, Cli = 1 };

// Why a client was detached. `Takeover` is the one a reader sees most: the
// latest client always wins, and the old one is told rather than dropped
// silently (plans/03).
enum class DetachReason : std::uint8_t { User = 0, Takeover = 1, SessionKilled = 2, ServerShutdown = 3 };

// What a terminal's complaint was about. Mirrors
// `ckv::core::TerminalDiagnostic::Kind` one for one, and is written down here
// for the reason `Rect` is: the wire format is stated once, in this file, and
// does not silently follow a change in the library's own enum.
enum class DiagnosticKind : std::uint8_t {
    LimitExceeded = 0,
    UnsupportedSequence = 1,
    MalformedSequence = 2,
    ChildExited = 3,
};

enum class PrinterScope : std::uint8_t { Terminal = 0, Session = 1, Global = 2 };
enum class PrinterMode : std::uint8_t { Ask = 0, Capture = 1, Off = 2 };
enum class PrinterState : std::uint8_t { Idle = 0, Capturing = 1, Sunk = 2, Full = 3 };
enum class PrintJobKind : std::uint8_t { Screen = 0, Line = 1, Controller = 2, Autoprint = 3 };

// A rectangle on the wire. ckVision's own `Rect` is what everything above
// this layer uses; this exists so the wire format is stated once and does not
// silently follow a change in the library's struct layout.
struct Rect {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;

    friend bool operator==(const Rect&, const Rect&) = default;
};

Rect to_wire(const ckv::Rect& rect);
ckv::Rect from_wire(const Rect& rect);

// One run of identical cells. The grid and the scrollback are both sequences
// of these: a terminal's rows are mostly repetition, and a run length is two
// bytes where the cells it stands for would be dozens.
struct CellRun {
    std::uint16_t run_length = 1;
    ckv::Cell cell;

    friend bool operator==(const CellRun& a, const CellRun& b) {
        return a.run_length == b.run_length && a.cell.grapheme() == b.cell.grapheme() &&
               a.cell.width() == b.cell.width() && a.cell.style() == b.cell.style();
    }
};

// Cells to runs and back. Exposed because both sides need them — the server
// to encode a grid, the client to rehydrate one — and because they are worth
// testing on their own: a run-length encoder that loses a cell loses it
// invisibly.
std::vector<CellRun> to_runs(const std::vector<ckv::Cell>& cells);
std::vector<ckv::Cell> from_runs(const std::vector<CellRun>& runs);

// --- GridDelta operations (plans/05, the hot path) -------------------------

// A region shift the client performs with a memmove rather than by receiving
// every cell again. This is what makes a full-screen scroll cost bytes
// proportional to nothing instead of to the screen.
struct ScrollOp {
    std::uint16_t top = 0;
    std::uint16_t bottom = 0;
    std::int16_t lines = 0;  // positive scrolls up, negative down

    friend bool operator==(const ScrollOp&, const ScrollOp&) = default;
};

struct CellsOp {
    std::uint16_t row = 0;
    std::uint16_t column = 0;
    std::vector<CellRun> runs;

    friend bool operator==(const CellsOp&, const CellsOp&) = default;
};

struct CursorOp {
    std::uint16_t column = 0;
    std::uint16_t row = 0;
    std::uint8_t style = 0;  // ckv::CursorShape
    std::uint8_t visible = 1;
    std::uint8_t blink = 0;

    friend bool operator==(const CursorOp&, const CursorOp&) = default;
};

// The child's mode flags, as a changed-mask plus values rather than a full
// set. A terminal that turns mouse reporting on says one bit changed; sending
// the whole set would make every mode change look like every other.
struct ModesOp {
    std::uint32_t changed_mask = 0;
    std::uint32_t values = 0;

    friend bool operator==(const ModesOp&, const ModesOp&) = default;
};

// The bits `ModesOp` carries. They mirror `ckv::core::TerminalSnapshot`'s own
// mode fields one for one, because the client's mirror IS a rehydrated
// snapshot and a bit with no field to land in would be a bit nobody could
// use. Mouse encoding is two bits, since it is three states rather than a
// flag (plans/05; ckVision's TerminalMouseEncoding).
enum class ModeBit : std::uint32_t {
    MouseReporting = 1u << 0,
    MouseEncodingSgr = 1u << 1,
    BracketedPaste = 1u << 2,
    ApplicationCursorKeys = 1u << 3,
    FocusReporting = 1u << 4,
    AlternateBuffer = 1u << 5,
    AlternateScroll = 1u << 6,
};

// The tracking LEVEL, as two bits above the flags, because DEC 1000, 1002 and
// 1003 are three levels of one facility and not three switches: a host that
// collapses them sends a program written for 1000 a stream of motion reports
// it never asked for, and a program that parses only what it asked for reads
// the surplus as something else entirely (ckVision's TerminalMouseTracking
// says the same thing from the other side). `MouseReporting` stays as the
// coarse answer most callers want, exactly as ckVision keeps
// `mouse_reporting_enabled` beside `mouse_tracking`.
//
// Two bits hold all four values the level has, so nothing read out of this
// mask can be a level that does not exist.
inline constexpr std::uint32_t kMouseTrackingShift = 7;
inline constexpr std::uint32_t kMouseTrackingMask = 0b11u << kMouseTrackingShift;

// The kitty keyboard enhancements the child has on, as five bits above the
// tracking level — `ckv::core::TerminalKeyboardFlags`, unshifted, one bit each.
//
// They live in the modes word because that is what they are. ckVision says the
// same thing from its side: the flags are "modes in all but their spelling,
// being switches a program turns on for as long as it needs them and puts back
// on the way out", and its `TerminalDamage::modes` fires when they move — so a
// delta transport carries them for free and a snapshot carries the whole set,
// which is exactly the two paths a mode already has here.
//
// Without them the client's `TerminalView` encoded every key the legacy way
// while the server's emulator answered the child's re-probe with the flags ON:
// the program had turned the legacy fallback off and was being sent it anyway
// (M-R2 in 13-architecture-review).
inline constexpr std::uint32_t kKeyboardFlagsShift = 9;
inline constexpr std::uint32_t kKeyboardFlagsMask = 0b11111u << kKeyboardFlagsShift;

struct TitleOp {
    std::string title;

    friend bool operator==(const TitleOp&, const TitleOp&) = default;
};

// Lines leaving the screen for the history. The client mirror keeps the whole
// bounded scrollback, which is what makes paging and copy mode local — a
// reader scrolling back does not wait for a round trip (plans/05).
struct ScrollbackPushOp {
    // One entry per line, each run-length encoded on its own so a line's
    // boundaries survive: the mirror stores rows, not a flat stream.
    std::vector<std::vector<CellRun>> lines;

    friend bool operator==(const ScrollbackPushOp&, const ScrollbackPushOp&) = default;
};

// The size of the grid the ops after it describe.
//
// A `GridDelta` carried no geometry, and a repaint of a SMALLER terminal
// passed every bounds check against a larger mirror: the rows and columns past
// the new edge kept whatever the last program had put there, and nothing
// anywhere fired (13-architecture-review C3, reproduced). Carrying it as an op
// rather than as a header field says the right thing — the size is stated
// exactly when it changed, and the ops that follow are validated against the
// size the mirror is about to be, not the one it was.
struct ResizeOp {
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;

    friend bool operator==(const ResizeOp&, const ResizeOp&) = default;
};

// Appended, never inserted: the alternatives are matched by index in more than
// one place, so a new op at the end costs nothing and a new op in the middle
// would renumber every one after it.
using GridOp =
    std::variant<ScrollOp, CellsOp, CursorOp, ModesOp, TitleOp, ScrollbackPushOp, ResizeOp>;

// --- The attach snapshot --------------------------------------------------

// One held print job, as metadata only. A megabyte of captured output never
// rides the attach snapshot or the hot path; the payload is pulled with
// `PrintJobFetch` when a reader actually looks (plans/04 WP-PRINT).
struct PrintJobInfo {
    std::uint64_t job = 0;
    PrintJobKind kind = PrintJobKind::Screen;
    std::uint32_t bytes = 0;
    std::uint32_t lines = 0;
    std::int64_t at = 0;

    friend bool operator==(const PrintJobInfo&, const PrintJobInfo&) = default;
};

// Everything about one terminal that a client needs in order to draw it
// without asking again. The field list mirrors ckVision's `TerminalSnapshot`
// deliberately: the client's mirror is a rehydrated one, fed to the stock
// `TerminalView` (plans/07).
struct TerminalState {
    std::uint64_t term = 0;
    std::uint16_t index = 0;
    std::string title;
    std::uint8_t flags = 0;  // bell, activity — see TermMetaFlag
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    Rect rect;
    std::uint16_t z_order = 0;
    std::uint8_t zoomed = 0;
    CursorOp cursor;
    std::uint32_t modes = 0;  // ModeBit values, the whole set rather than a delta
    std::vector<CellRun> grid;
    // In full, and in lines: a reattached client can page back through
    // everything the server still holds without a round trip.
    std::vector<std::vector<CellRun>> scrollback;
    // Placed images, by id. The pixels follow as chunked image messages,
    // because a snapshot that carried them would be a snapshot nobody could
    // cap.
    std::vector<std::uint64_t> images;
    PrinterMode printer_mode = PrinterMode::Ask;
    PrinterScope printer_scope = PrinterScope::Global;
    PrinterState printer_state = PrinterState::Idle;
    std::uint32_t printer_bytes = 0;
    std::vector<PrintJobInfo> print_jobs;

    // --- What a reattaching reader was losing (R8) -------------------------

    // The clipboard WATERMARK, and deliberately not the text.
    //
    // A child's OSC 52 is a live act: "put this on the reader's clipboard now".
    // The text therefore travels only on the live path (`ClipboardSet`), and a
    // snapshot carries the number so that a client which reattaches neither
    // replays an old write over whatever its reader has copied since, nor
    // treats the next real one as something it has already seen. The serial is
    // monotonic per terminal and is never reset — a mirror that started over at
    // zero would let the write after a reattach collide with a watcher's own
    // high-water mark and be dropped in silence (ckVision's `clipboard_serial`
    // says the same from the other end).
    std::uint64_t clipboard_serial = 0;
    // Whether the child has ended, what it ended with, and whether the terminal
    // is being kept anyway. Without these, a snapshot could not restate what a
    // `TermClosed` had already said, so `TerminalMirror::adopt` had to preserve
    // its own knowledge and a server that genuinely restarted a terminal had no
    // way to say so (M-R4). A terminal that has exited and is still in a
    // session IS a held one, which is what `hold` reports.
    std::uint8_t exited = 0;
    std::int32_t exit_status = 0;
    std::uint8_t hold = 0;
    // The terminal's newest complaint, and nothing older. The emulator keeps a
    // bounded ring; a view paints the most recent entry, so that is what a
    // reattaching client needs and the rest would be bytes nobody reads. Empty
    // text means the terminal has never complained.
    DiagnosticKind diagnostic_kind = DiagnosticKind::MalformedSequence;
    std::string diagnostic;

    friend bool operator==(const TerminalState&, const TerminalState&) = default;
};

// What a terminal's `flags` byte carries: what a reader who is not looking at
// this terminal has missed.
//
// Both are MARKS rather than events, and both mean "since the reader was last
// in this terminal". `Bell` is set when the child rings (BEL); `Activity` when
// the child writes to a terminal that is not the focused one — output on the
// terminal a reader is already watching is not news, it is the thing they are
// watching. Both are cleared when that terminal becomes the focused one, which
// is the moment the reader has seen what they said.
//
// A mark and not a count, because what a window marker answers is "is there
// anything here I have not seen?" — and a count of bells while a reader was
// away is a number nobody acts on. The emulator's own `bell_serial` is a count
// for a different consumer (a host that flashes every bell as it happens); this
// is the multiplexer's question.
enum class TermMetaFlag : std::uint8_t { Bell = 1u << 0, Activity = 1u << 1 };

struct Snapshot {
    std::uint16_t desktop_columns = 0;
    std::uint16_t desktop_rows = 0;
    std::uint64_t focused_term = 0;
    std::vector<TerminalState> terminals;

    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

// What these will occupy inside a payload, without producing the bytes.
//
// Exact, and pinned against `encode` by a test — the whole point is deciding
// what to send BEFORE the bytes exist, and a size that had drifted from the
// writer would decide wrongly and silently. Each one counts its own length
// prefix, so a caller adds them up without having to know which fields carry a
// count: the `Snapshot` overload is the `Attached` payload less the eight
// bytes of its session id.
std::size_t encoded_size(const CellRun& run);
std::size_t encoded_size(const std::vector<CellRun>& runs);
std::size_t encoded_size(const std::vector<std::vector<CellRun>>& lines);
std::size_t encoded_size(const TerminalState& state);
std::size_t encoded_size(const Snapshot& snapshot);

struct SessionInfo {
    std::uint64_t id = 0;
    std::string name;
    std::uint16_t terminals = 0;
    std::uint8_t attached = 0;
    std::int64_t created = 0;
    std::int64_t last_attached = 0;

    friend bool operator==(const SessionInfo&, const SessionInfo&) = default;
};

struct LayoutEntry {
    std::uint64_t term = 0;
    Rect rect;
    std::uint16_t z_order = 0;
    std::uint8_t zoomed = 0;

    friend bool operator==(const LayoutEntry&, const LayoutEntry&) = default;
};

// --- Messages -------------------------------------------------------------
//
// Every message carries its own `kType`, so encoding never restates what the
// struct already knows and a new message cannot be added without a type. The
// three shapes that repeat — no payload at all, one terminal id, one nonce —
// are one template each, tagged by their type so they stay distinct in the
// variant. A dozen near-identical structs would be a dozen places to make the
// same mistake.

template <MessageType T>
struct Empty {
    static constexpr MessageType kType = T;
    friend bool operator==(const Empty&, const Empty&) = default;
};

template <MessageType T>
struct TerminalRef {
    static constexpr MessageType kType = T;
    std::uint64_t term = 0;
    friend bool operator==(const TerminalRef&, const TerminalRef&) = default;
};

// An image id on its own — what ends a chunked transfer.
template <MessageType T>
struct ImageRef {
    static constexpr MessageType kType = T;
    std::uint64_t id = 0;
    friend bool operator==(const ImageRef&, const ImageRef&) = default;
};

template <MessageType T>
struct Nonce {
    static constexpr MessageType kType = T;
    std::uint64_t nonce = 0;
    friend bool operator==(const Nonce&, const Nonce&) = default;
};

struct Hello {
    static constexpr MessageType kType = MessageType::Hello;
    std::uint32_t proto_version = kProtocolVersion;
    std::string build;
    ClientKind client_kind = ClientKind::Ui;
    friend bool operator==(const Hello&, const Hello&) = default;
};

struct HelloAck {
    static constexpr MessageType kType = MessageType::HelloAck;
    std::uint32_t proto_version = kProtocolVersion;
    std::string build;
    friend bool operator==(const HelloAck&, const HelloAck&) = default;
};

struct Refuse {
    static constexpr MessageType kType = MessageType::Refuse;
    // Carries both versions and the remedy in words, because this is the one
    // message a reader meets after an upgrade and "version mismatch" tells
    // them nothing they can act on.
    std::string reason;
    friend bool operator==(const Refuse&, const Refuse&) = default;
};

struct NewSession {
    static constexpr MessageType kType = MessageType::NewSession;
    // Empty means "name it for me" — the server owns naming rules (plans/03),
    // so a client that has no opinion says nothing rather than guessing.
    std::string name;
    std::uint8_t spawn_first = 1;
    std::string command;
    friend bool operator==(const NewSession&, const NewSession&) = default;
};

struct Attach {
    static constexpr MessageType kType = MessageType::Attach;
    std::uint64_t session = 0;
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    // Pixel geometry feeds TIOCSWINSZ's pixel fields and the child's
    // XTWINOPS 14/16 replies. Zero is honest ignorance, not a size.
    std::uint16_t pixel_width = 0;
    std::uint16_t pixel_height = 0;
    // Whether the client's OUTER terminal reported Sixel graphics. The
    // server folds it into the advertisement a child of this client's
    // terminals is given (plans/04: "ckmux decides sixel per host
    // capabilities and reflects the choice in the child's profile").
    // Nonzero is yes; zero is no or unknown, which advertise alike.
    std::uint8_t host_sixel = 0;
    friend bool operator==(const Attach&, const Attach&) = default;
};

struct ClientResize {
    static constexpr MessageType kType = MessageType::ClientResize;
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    std::uint16_t pixel_width = 0;
    std::uint16_t pixel_height = 0;
    friend bool operator==(const ClientResize&, const ClientResize&) = default;
};

struct NewTerminal {
    static constexpr MessageType kType = MessageType::NewTerminal;
    // Zero session means "the one I am attached to"; an empty command means
    // the configured shell; a zero-size rect means "place it yourself". Every
    // optional field is a sentinel rather than a presence flag, because a
    // client that has no opinion is the common case and should not have to
    // say so twice.
    std::uint64_t session = 0;
    std::string command;
    Rect rect;
    friend bool operator==(const NewTerminal&, const NewTerminal&) = default;
};

struct MoveTerminal {
    static constexpr MessageType kType = MessageType::MoveTerminal;
    std::uint64_t term = 0;
    std::uint64_t destination_session = 0;
    // Set when the reader chose "a new session" rather than one from the
    // list, and it outranks `destination_session`: an explicit flag, not a
    // magic id, because Attach already gave 0 a different meaning ("the
    // default one") and a wire that reuses a number for a second meaning is
    // a wire someone will eventually decode with the first.
    std::uint8_t to_new_session = 0;
    friend bool operator==(const MoveTerminal&, const MoveTerminal&) = default;
};

struct CloseTerminal {
    static constexpr MessageType kType = MessageType::CloseTerminal;
    std::uint64_t term = 0;
    // The same two answers KillSession carries, scoped to one terminal: the
    // program is asked first — SIGHUP then SIGTERM, by the library that owns
    // the child — and these say how long to wait and whether anything that
    // ignores the asking is killed outright. `force = 0` means the program
    // keeps running, and its terminal with it; a reader who unticks the box
    // is choosing that.
    std::uint8_t force = 1;
    std::uint16_t grace_seconds = 5;
    friend bool operator==(const CloseTerminal&, const CloseTerminal&) = default;
};

struct MoveResize {
    static constexpr MessageType kType = MessageType::MoveResize;
    std::uint64_t term = 0;
    Rect rect;
    friend bool operator==(const MoveResize&, const MoveResize&) = default;
};

struct ZoomTerm {
    static constexpr MessageType kType = MessageType::ZoomTerm;
    std::uint64_t term = 0;
    std::uint8_t on = 0;
    friend bool operator==(const ZoomTerm&, const ZoomTerm&) = default;
};

struct RenameSession {
    static constexpr MessageType kType = MessageType::RenameSession;
    std::uint64_t id = 0;
    std::string name;
    friend bool operator==(const RenameSession&, const RenameSession&) = default;
};

struct RenameTerminal {
    static constexpr MessageType kType = MessageType::RenameTerminal;
    std::uint64_t id = 0;
    std::string name;
    friend bool operator==(const RenameTerminal&, const RenameTerminal&) = default;
};

struct KillSession {
    static constexpr MessageType kType = MessageType::KillSession;
    std::uint64_t session = 0;
    // Every program in it is asked to end — SIGHUP then SIGTERM, by the library
    // that owns the child, so nothing above it ever handles a pid. What these
    // two carry is the part only a reader can decide: how long to wait, and
    // whether anything that ignores the asking is killed outright.
    //
    // `force = 0` means exactly what it says: a program that declines to quit
    // keeps running, and the session with it. A reader who unticks the box is
    // choosing that.
    std::uint8_t force = 1;
    std::uint16_t grace_seconds = 5;
    friend bool operator==(const KillSession&, const KillSession&) = default;
};

struct Input {
    static constexpr MessageType kType = MessageType::Input;
    std::uint64_t term = 0;
    // Already-encoded child bytes. Keys, mouse and focus reports are encoded
    // client-side by ckVision's TerminalView against mirrored mode state
    // (plans/04), so the server writes them to the PTY without understanding
    // them — which is also why this is a blob and not a key event.
    std::string bytes;
    friend bool operator==(const Input&, const Input&) = default;
};

struct PasteChunk {
    static constexpr MessageType kType = MessageType::PasteChunk;
    std::uint64_t term = 0;
    std::uint32_t seq = 0;
    std::uint8_t final_chunk = 0;
    std::string bytes;
    friend bool operator==(const PasteChunk&, const PasteChunk&) = default;
};

struct PasteAck {
    static constexpr MessageType kType = MessageType::PasteAck;
    std::uint32_t seq = 0;
    friend bool operator==(const PasteAck&, const PasteAck&) = default;
};

struct SessionList {
    static constexpr MessageType kType = MessageType::SessionList;
    std::vector<SessionInfo> sessions;
    friend bool operator==(const SessionList&, const SessionList&) = default;
};

struct Attached {
    static constexpr MessageType kType = MessageType::Attached;
    std::uint64_t session = 0;
    Snapshot snapshot;
    friend bool operator==(const Attached&, const Attached&) = default;
};

struct Detached {
    static constexpr MessageType kType = MessageType::Detached;
    DetachReason reason = DetachReason::User;
    std::string text;
    friend bool operator==(const Detached&, const Detached&) = default;
};

struct LayoutDelta {
    static constexpr MessageType kType = MessageType::LayoutDelta;
    std::uint64_t session = 0;
    std::vector<LayoutEntry> entries;
    std::uint64_t focused_term = 0;
    std::uint16_t desktop_columns = 0;
    std::uint16_t desktop_rows = 0;
    friend bool operator==(const LayoutDelta&, const LayoutDelta&) = default;
};

struct TermOpened {
    static constexpr MessageType kType = MessageType::TermOpened;
    std::uint64_t term = 0;
    std::uint64_t session = 0;
    std::uint16_t index = 0;
    Rect rect;
    std::string title;
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    friend bool operator==(const TermOpened&, const TermOpened&) = default;
};

struct TermClosed {
    static constexpr MessageType kType = MessageType::TermClosed;
    std::uint64_t term = 0;
    std::uint8_t exited = 0;
    std::int32_t exit_status = 0;
    // The window stays, with a banner, because a program that failed has said
    // something on that screen and closing it takes the evidence away
    // (plans/03 on-exit).
    std::uint8_t hold = 0;
    friend bool operator==(const TermClosed&, const TermClosed&) = default;
};

struct TermMeta {
    static constexpr MessageType kType = MessageType::TermMeta;
    std::uint64_t term = 0;
    std::uint16_t index = 0;
    std::string title;
    std::uint8_t flags = 0;
    friend bool operator==(const TermMeta&, const TermMeta&) = default;
};

struct GridDelta {
    static constexpr MessageType kType = MessageType::GridDelta;
    std::uint64_t term = 0;
    // Per-terminal and monotonic. A gap the client observes should not happen;
    // when it does, the client reconnects and re-snapshots rather than trying
    // to work out what it missed (plans/05).
    std::uint32_t seq = 0;
    std::vector<GridOp> ops;
    friend bool operator==(const GridDelta&, const GridDelta&) = default;
};

struct ImageAddBegin {
    static constexpr MessageType kType = MessageType::ImageAddBegin;
    std::uint64_t id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    friend bool operator==(const ImageAddBegin&, const ImageAddBegin&) = default;
};

struct ImageChunk {
    static constexpr MessageType kType = MessageType::ImageChunk;
    std::uint64_t id = 0;
    std::uint32_t seq = 0;
    std::string bytes;  // raw RGBA
    friend bool operator==(const ImageChunk&, const ImageChunk&) = default;
};

struct ImagePlace {
    static constexpr MessageType kType = MessageType::ImagePlace;
    std::uint64_t term = 0;
    std::uint64_t id = 0;
    Rect cells;
    std::int16_t pixel_offset_x = 0;
    std::int16_t pixel_offset_y = 0;
    friend bool operator==(const ImagePlace&, const ImagePlace&) = default;
};

struct ImageRemove {
    static constexpr MessageType kType = MessageType::ImageRemove;
    std::uint64_t term = 0;
    std::uint64_t id = 0;
    friend bool operator==(const ImageRemove&, const ImageRemove&) = default;
};

// A child asked to put text on the reader's clipboard (OSC 52) and its
// terminal's policy allowed it.
//
// The terminal travels with the text although a reader has only one clipboard:
// every other terminal-scoped message here names its terminal, and the client
// routes this one into that terminal's mirror so the write reaches the reader
// through the same path a local terminal's does — `TerminalView` sees the
// serial move and hands the text to its host (plans/07 seam 2). A message that
// named no terminal could only be routed by guessing which one had asked.
struct ClipboardSet {
    static constexpr MessageType kType = MessageType::ClipboardSet;
    std::uint64_t term = 0;
    std::string text;
    friend bool operator==(const ClipboardSet&, const ClipboardSet&) = default;
};

// What a terminal has just had to complain about: a sequence it does not
// implement, one that was malformed, a limit a child ran into.
//
// The newest one, one message at a time, rather than the emulator's whole ring.
// A ring is a thing a host reads when it wants to; a transport that shipped it
// whole would re-send entries the client already holds on every change, and the
// view that shows this paints the most recent entry (ckVision's
// `TerminalSubsession::diagnostics`). Sent while a client is watching; the
// snapshot carries the same one entry for a client that has just arrived.
struct TermDiagnostic {
    static constexpr MessageType kType = MessageType::TermDiagnostic;
    std::uint64_t term = 0;
    DiagnosticKind kind = DiagnosticKind::MalformedSequence;
    std::string text;
    friend bool operator==(const TermDiagnostic&, const TermDiagnostic&) = default;
};

// What an `Error` code means. Non-fatal by definition: the connection survives,
// and the client is told why one request could not be honoured (plans/05).
enum class ErrorCode : std::uint16_t {
    Unknown = 0,
    // Understood, and this build does not do it yet. Answering rather than
    // ignoring is the point: a client waiting for a reply that never comes is
    // indistinguishable, to a reader, from a server that has hung.
    NotImplemented = 1,
    NoSuchTerminal = 2,
    NoSuchSession = 3,
    NameTaken = 4,
};

struct Error {
    static constexpr MessageType kType = MessageType::Error;
    std::uint16_t code = 0;
    std::string context;
    std::string human;
    friend bool operator==(const Error&, const Error&) = default;
};

struct SetPrinterPolicy {
    static constexpr MessageType kType = MessageType::SetPrinterPolicy;
    PrinterScope scope = PrinterScope::Terminal;
    std::uint64_t target = 0;
    PrinterMode mode = PrinterMode::Ask;
    std::uint32_t ask_cache = 0;
    std::uint32_t spool_limit = 0;
    friend bool operator==(const SetPrinterPolicy&, const SetPrinterPolicy&) = default;
};

struct PrintState {
    static constexpr MessageType kType = MessageType::PrintState;
    std::uint64_t term = 0;
    PrinterMode mode = PrinterMode::Ask;
    PrinterState state = PrinterState::Idle;
    std::uint32_t bytes = 0;
    std::uint16_t jobs = 0;
    friend bool operator==(const PrintState&, const PrintState&) = default;
};

struct PrintJobAdded {
    static constexpr MessageType kType = MessageType::PrintJobAdded;
    std::uint64_t term = 0;
    PrintJobInfo job;
    friend bool operator==(const PrintJobAdded&, const PrintJobAdded&) = default;
};

struct PrintJobFetch {
    static constexpr MessageType kType = MessageType::PrintJobFetch;
    std::uint64_t term = 0;
    std::uint64_t job = 0;
    friend bool operator==(const PrintJobFetch&, const PrintJobFetch&) = default;
};

struct PrintJobData {
    static constexpr MessageType kType = MessageType::PrintJobData;
    std::uint64_t term = 0;
    std::uint64_t job = 0;
    std::uint32_t seq = 0;
    std::uint8_t final_chunk = 0;
    std::string bytes;
    friend bool operator==(const PrintJobData&, const PrintJobData&) = default;
};

struct PrintJobDiscard {
    static constexpr MessageType kType = MessageType::PrintJobDiscard;
    std::uint64_t term = 0;
    // Zero discards every job this terminal holds, which is what the frame
    // button's "discard" means when nothing in particular is selected.
    std::uint64_t job = 0;
    friend bool operator==(const PrintJobDiscard&, const PrintJobDiscard&) = default;
};

using ListSessions = Empty<MessageType::ListSessions>;
using Detach = Empty<MessageType::Detach>;
using KillServer = Empty<MessageType::KillServer>;
using SessionsChanged = Empty<MessageType::SessionsChanged>;
using KillTerminal = TerminalRef<MessageType::KillTerminal>;
using RespawnTerminal = TerminalRef<MessageType::RespawnTerminal>;
using Raise = TerminalRef<MessageType::Raise>;
using FocusTerm = TerminalRef<MessageType::FocusTerm>;
using ImageEnd = ImageRef<MessageType::ImageEnd>;
using Ping = Nonce<MessageType::Ping>;
using Pong = Nonce<MessageType::Pong>;

// Every message there is. A decoder walks these alternatives to find the one
// whose `kType` matches the frame, so the switch cannot drift out of step
// with the catalogue — adding a message here is the whole of adding it.
using Message =
    std::variant<Hello, HelloAck, Refuse, Ping, Pong, ListSessions, NewSession, Attach, Detach,
                 ClientResize, RenameSession, KillSession, KillServer, SessionList, SessionsChanged,
                 Attached, Detached, NewTerminal, CloseTerminal, KillTerminal, RespawnTerminal,
                 MoveTerminal, MoveResize, Raise, FocusTerm, ZoomTerm, RenameTerminal, Input,
                 PasteChunk, PasteAck, LayoutDelta, TermOpened, TermClosed, TermMeta, GridDelta,
                 ImageAddBegin, ImageChunk, ImageEnd, ImagePlace,
                 ImageRemove, ClipboardSet, TermDiagnostic, Error, SetPrinterPolicy, PrintState,
                 PrintJobAdded, PrintJobFetch, PrintJobData, PrintJobDiscard>;

MessageType type_of(const Message& message);

// --- Encoding and decoding ------------------------------------------------

// The bytes of a message, or nothing at all when the payload would be past the
// cap its type is held to — because a frame over the cap is not a large frame,
// it is one the peer's decoder refuses, and a sender that learns so at the
// socket has already lost both the message and the connection.
//
// Encoding still cannot fail for any message built out of state a program
// legitimately holds, which is why the ordinary call is unchanged. `oversize`
// is how the one caller that CAN produce one — a server encoding a session's
// whole state — finds out, instead of handing a socket a frame that lies about
// its own length. An empty result is unambiguous: a real frame is never
// shorter than its eight-byte header.
std::string encode(const Message& message, bool* oversize = nullptr);

enum class DecodeError : std::uint8_t {
    None = 0,
    // Not an error: the bytes so far are a prefix of a frame. A stream reader
    // waits for more; anything that has all the bytes it is going to get
    // treats this as malformed.
    Incomplete,
    // The frame declared a payload larger than its type is allowed.
    PayloadTooLarge,
    // A type this version does not know. There is one version integer, so an
    // unknown type cannot legitimately occur (plans/05, invariant 4).
    UnknownType,
    // Reserved flags were not zero.
    ReservedFlags,
    // A field ran past the end of the payload, a length exceeded its cap, or
    // an enum carried a value it does not have.
    Malformed,
    // The payload was well-formed and complete, and bytes remained after it.
    // A frame that decodes to less than it carries is not a frame this
    // protocol produced.
    TrailingBytes,
};

std::string_view describe(DecodeError error);

struct DecodeResult {
    DecodeError error = DecodeError::None;
    // How many bytes the frame occupied, valid when `error` is None. Also set
    // for a frame whose header parsed but whose payload was rejected, so a
    // caller that wants to resynchronise knows where the next one would
    // start — though the protocol's answer to a bad frame is to drop the
    // connection, not to skip it.
    std::size_t consumed = 0;

    bool ok() const noexcept { return error == DecodeError::None; }
};

// Decodes one frame from the front of `bytes`. Never throws, never reads past
// the end, and leaves `message` untouched unless it returns success.
DecodeResult decode(std::string_view bytes, Message& message);

// Reassembles frames from a stream. A socket read gives whatever arrived: half
// a frame, three frames, or a frame and a half. This owns that problem so no
// caller has to solve it twice, and so the solution is testable without a
// socket.
class FrameReader {
public:
    // Appends what a read produced. Returns false when the buffer would grow
    // past what any single frame may be — a peer that sends a length it never
    // follows with bytes must not be able to make this grow without bound.
    bool append(std::string_view bytes);

    // Takes the next complete frame, or reports why not. `Incomplete` means
    // "call me again after the next read" and is the only error that is not
    // fatal; every other one means the connection is over.
    DecodeError next(Message& message);

    std::size_t buffered() const noexcept { return buffer_.size() - consumed_; }
    void clear() noexcept;

private:
    void compact();

    std::string buffer_;
    std::size_t consumed_ = 0;
};

}  // namespace ckm::proto
