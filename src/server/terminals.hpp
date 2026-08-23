// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The terminals a ckmux server owns (the architecture spec, WP-3).
//
// One `PosixTerminalSubsession` per terminal — ckVision owns the PTY, the
// `forkpty`+`execve`, the non-blocking master, `TIOCSWINSZ` with its pixel
// fields, and the SIGHUP→SIGTERM→SIGKILL close policy on the process *group*.
// What this file owns is everything a multiplexer adds around that: identity
// that is never recycled, the collection, draining under a byte budget so one
// noisy child cannot starve the others, and the guarantee that a child never
// outlives an explicit close.
//
// It includes only ckVision's `core` and `term` headers, never `ui`,
// `widgets` or `scene` (the engineering standard), so the server builds and runs with no host
// terminal — which is the whole point of the split: the thing that owns the
// programs must not need a screen.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/config.hpp"
#include "common/proto.hpp"
#include "cvision/term/posix_terminal_subsession.hpp"

namespace ckm::server {

// Where a terminal's WINDOW is, as the client watching its session last
// reported it (the session model's Terminal `geometry` row; a session's `layout` is this
// for every terminal in it, plus the focused id the session itself holds).
//
// Kept in the wire's own `Rect` rather than ckVision's, because this server
// neither draws a window nor computes with one: what it does with a layout is
// remember it and say it again, and a value converted on the way in and back on
// the way out is a value that can differ from what the reader arranged. The
// clamping that `MoveResize` does is deliberately absent for the same reason —
// see `Terminal::set_layout`.
struct WindowLayout {
    proto::Rect rect;
    // From the bottom of the stack up. Only meaningful against the other
    // windows of the same session, which is why a layout report states them all
    // at once (the protocol spec `SetLayout`).
    std::uint16_t z_order = 0;
    bool zoomed = false;
    // And what share of a filled tiling the window held, if the arrangement it
    // was reported in was one (WP-30). Stored beside the rect rather than
    // instead of it: the two answer different questions — where the window was,
    // and what fraction of the desktop it was — and which of them a reattaching
    // client uses is that client's policy, not this server's. Zero extent is a
    // window that was simply floating.
    proto::TileFraction tile;

    friend bool operator==(const WindowLayout&, const WindowLayout&) = default;
};

// Never recycled within a server's lifetime (the protocol spec, invariant 3). A client
// that names a terminal after it closed must be told it is gone, not handed a
// different one — and a reused id is how a paste lands in somebody else's
// shell.
using TerminalId = std::uint64_t;

// What a terminal is opened with. Everything is stated: a server that read the
// ambient environment would give its children a machine other than the one
// their owner configured, and the failures look like the program is broken.
struct TerminalSpec {
    // Empty runs the configured shell (`[general] shell`, or $SHELL).
    std::string command;
    std::string working_directory;
    std::vector<std::pair<std::string, std::string>> environment;
    int columns = 80;
    int rows = 24;
    // The size of the client's whole text area in pixels, which is what the
    // protocol carries (the protocol spec `Attach`) because it is what a client can
    // actually measure. ckVision wants the size of ONE CELL, so the conversion
    // happens here — see `Terminal::resize`. Zero is honest ignorance rather
    // than a size.
    int pixel_width = 0;
    int pixel_height = 0;
    // Whether the opening client's OUTER terminal reported Sixel (WP-16).
    // Folded into the child's advertisement together with `[terminal] sixel`:
    // a child told "yes" draws a picture the reader can actually see, and a
    // child told "no" draws its text fallback instead of pixels nobody could.
    // True when no client said otherwise, so a terminal opened without a UI
    // client (a respawn, a future CLI) follows the setting alone.
    bool host_sixel = true;
};

// A printer policy, as one scope states it. Every field is optional because a
// scope may override one thing and defer the rest — a reader who sets "always
// capture for this session" has said nothing about how big its spool may be,
// and inheriting the rest is the whole point of having scopes (the interface spec:
// terminal → session → global → built-in).
struct PrinterOverride {
    std::optional<PrinterMode> mode;
    std::optional<std::uint32_t> ask_cache;
    std::optional<std::uint32_t> spool_limit;

    bool empty() const noexcept { return !mode && !ask_cache && !spool_limit; }
};

// The three numbers in force somewhere, with the scope each came from — which
// is not decoration: the interface spec requires the Printer Settings dialog to show the
// effective values *and where they came from*, so that a per-session override
// a reader forgot about is never invisible to them.
struct EffectivePrinterPolicy {
    PrinterMode mode = PrinterMode::Ask;
    std::uint32_t ask_cache = 0;
    std::uint32_t spool_limit = 0;
    proto::PrinterScope mode_from = proto::PrinterScope::Global;
    proto::PrinterScope ask_cache_from = proto::PrinterScope::Global;
    proto::PrinterScope spool_limit_from = proto::PrinterScope::Global;
};

// One terminal: a PTY, the program in it, and the emulator that reads it.
//
// The reads a diff engine needs are forwarded rather than exposed as the
// subsession, because U0-b's whole point is that a host at tick rate must not
// copy what a terminal remembers, and a caller handed the subsession would
// have to know which of its several read paths is the cheap one.
class Terminal {
public:
    Terminal(TerminalId id, std::unique_ptr<ckv::term::PosixTerminalSubsession> session,
             const TerminalSpec& spec);

    TerminalId id() const noexcept { return id_; }

    // What the child has said. `damage()` is what changed since the host last
    // said it had caught up; `cells()` and `scrollback()` borrow.
    const ckv::term::TerminalDamage& damage() const noexcept { return session_->damage(); }
    void clear_damage() noexcept { session_->clear_damage(); }
    std::span<const ckv::Cell> cells() const noexcept { return session_->cells(); }
    std::span<const ckv::Cell> scrollback() const noexcept { return session_->scrollback(); }
    // The scalar half — cursor, modes, title, the bell and printer counts —
    // which a diff engine needs on every tick and which is not worth a grid
    // copy to learn (ckVision's `status()`; the ckVision integration spec L-51).
    ckv::term::TerminalStatus status() const { return session_->status(); }
    // The ckVision seam this terminal owns, for the parts of the server that
    // read a terminal rather than run one — the diff engine reads damage,
    // cells, history and scalars through it and needs to know nothing about
    // PTYs, which is why it is handed out rather than wrapped.
    ckv::core::TerminalSubsession& session() noexcept { return *session_; }
    // The child's pid, for OBSERVATION — where the stats sampler roots this
    // terminal's process tree (the work queue WP-38) — and -1 once the exit has been
    // observed. Ending a child stays `request_termination()`'s and never goes
    // by pid; ckVision's accessor says the same where it is declared (U5-a).
    int process_id() const noexcept { return session_->process_id(); }
    const ckv::core::TerminalSubsession& session() const noexcept { return *session_; }
    ckv::term::TerminalSnapshot snapshot() const { return session_->snapshot(); }
    ckv::term::TerminalSnapshot snapshot(ckv::core::TerminalSnapshotOptions options) const {
        return session_->snapshot(options);
    }

    // Child bytes in, child bytes out. Input is already encoded by the client
    // against mirrored mode state (the terminal-emulation spec), so this writes it without
    // understanding it.
    void send_input(std::string_view bytes) { session_->send_input(bytes); }
    std::string take_pending_input() { return session_->take_pending_input(); }
    // Replies the emulator owes its child — DA, DSR, mouse reports — which the
    // server must write back to the PTY rather than to a client.
    void flush_replies_to_child();

    // Reads at most `byte_budget` from the PTY. Bounded because one child
    // running `yes` must not starve every other terminal on the server, which
    // is a fairness property and not an optimisation.
    bool drain(std::size_t byte_budget) { return session_->drain(byte_budget); }

    // The client's geometry. `pixel_width`/`pixel_height` are the whole text
    // area, as the protocol carries them; the cell metric ckVision wants is
    // derived here.
    //
    // The two are one division apart and easy to confuse, and confusing them
    // is silent: passing a window size where a cell size belongs makes
    // `TIOCSWINSZ` quote a text area a hundred times too large, and every
    // Sixel a child then draws comes out the wrong size because XTWINOPS 14
    // and 16 both derive from it. A PTY-level test pins the arithmetic.
    void resize(int columns, int rows, int pixel_width, int pixel_height);
    int columns() const noexcept { return columns_; }
    int rows() const noexcept { return rows_; }
    // What one cell measures, derived from the text area and the grid.
    ckv::Size cell_pixels() const noexcept;

    ckv::core::TerminalSubsessionState state() const noexcept { return session_->state(); }
    bool live() const noexcept;
    // The status the child exited with, once it has. Nothing until then —
    // "still running" and "exited with 0" are different answers and a caller
    // that cannot tell them apart will show a banner for a program that is
    // still working.
    std::optional<int> exit_status() const noexcept { return exit_status_; }
    // Read once per drain: a child's exit is observed by the PTY reaching its
    // read boundary, and the status has to be picked up when that happens
    // rather than polled for later.
    void observe_exit();
    // Whether the clients have already been told this child ended. The exit is
    // looked for on every tick and `live()` stays false forever afterwards, so
    // without this a held window would be announced dead once a frame, for as
    // long as the reader left it open.
    bool exit_announced() const noexcept { return exit_announced_; }
    void mark_exit_announced() noexcept { exit_announced_ = true; }

    // The launch to make this terminal again. WP-13's respawn is "the same
    // command in the same window" (the interface spec) — a build loop a reader
    // restarts with Enter — and "the same command" is only knowable if the
    // spec outlives the child that ran it, which is why one is kept.
    //
    // The GEOMETRY, though, is this terminal's current size rather than the
    // spec's: a reader who resized the window before pressing Enter meant the
    // new child to fill it, and the stored spec remembers the size the dead
    // one started at. Answered here rather than by handing out the fields,
    // so there is one place that knows which half of the spec is stale.
    TerminalSpec respawn_spec() const;

    // Puts a fresh child behind this same terminal, discarding the exited
    // one's bookkeeping. The id does not change, which is the whole point:
    // the client keeps the window it already has, and WP-3's "an id is never
    // reused" is not bent by it — this is the same terminal running again,
    // not a second terminal wearing the first one's number.
    void relaunch(std::unique_ptr<ckv::term::PosixTerminalSubsession> session) noexcept;

    // What a reader who is not looking at this terminal has missed: a bell it
    // rang, and output it produced (the protocol spec's `TermMeta` flags).
    //
    // Marks rather than counts, and they live HERE rather than in a client
    // because a session outlives its clients: a bell rung into a session
    // nobody was attached to is exactly the one a reader wants marked when they
    // come back. Cleared when this terminal becomes the focused one, which is
    // the moment a reader has seen what it said.
    bool bell_marked() const noexcept { return bell_marked_; }
    bool activity_marked() const noexcept { return activity_marked_; }
    void mark_bell() noexcept;
    void mark_activity() noexcept;
    void clear_marks() noexcept;
    // Whether the watching client has been told the marks AS THEY NOW STAND.
    // Set by the server when it sends them and cleared by any change above, so
    // an unchanged pair costs no message and a changed one costs exactly one —
    // which is what keeps a terminal printing steadily from putting a `TermMeta`
    // on the wire every tick for the whole of a build.
    // The counts behind the marks (WP-41/WP-44). Incremented every time the
    // child rings or writes, whoever is watching and whether or not anybody
    // is — a fact about the terminal, not about a reader — and never reset,
    // so a client that remembers the number it last answered can tell a
    // second bell from the first one it already dismissed.
    std::uint32_t bell_serial() const noexcept { return bell_serial_; }
    std::uint32_t activity_serial() const noexcept { return activity_serial_; }
    // A BELL's count is also an announcement; an activity count is not, and
    // the asymmetry is the point rather than an oversight.
    //
    // A reader needs bells at full resolution — "it rang AGAIN" is the whole
    // reason the serial exists, and a second ring leaves the level already up,
    // so `mark_bell()` returns early and nothing else would clear
    // `marks_announced_`. Without this the count increments on the server and
    // never reaches the wire: produced, and unreachable, in exactly the case
    // it was added for.
    //
    // Activity is different in kind. It is counted on every tick a terminal
    // writes, so announcing each count would put a `TermMeta` on the wire per
    // tick for the whole of a build — which is the cost `marks_announced_`
    // exists to avoid, and it broke three suites when tried. A reader does not
    // need "wrote 400 times", only "wrote since you last looked", so the level
    // still decides when to speak and the current count rides along with
    // whatever message that produces.
    void count_bell() noexcept {
        ++bell_serial_;
        marks_announced_ = false;
    }
    void count_activity() noexcept { ++activity_serial_; }

    bool marks_announced() const noexcept { return marks_announced_; }
    void note_marks_announced() noexcept { marks_announced_ = true; }
    // Where this terminal's window is, and whether the watching client has been
    // told the arrangement AS IT NOW STANDS.
    //
    // Here rather than on the client for the same reason the marks above are: a
    // session outlives its clients, and a reader who arranged their windows and
    // then closed the lid expects to find them where they left them — which is
    // the whole of what the session model decided when it made window layout session
    // state owned by the server. Here rather than beside the session's terminal
    // list for the reason the session model's own Terminal table gives it a `geometry`
    // row: it is a fact about one terminal, and a parallel list keyed by id
    // would need pruning at every place a terminal leaves a session — three of
    // them today, and the one that got missed would state a window for a
    // terminal that no longer exists.
    //
    // `set_layout` answers whether anything actually moved, and only a move
    // clears the announcement: a client that reports the same arrangement twice
    // costs one message and not two, and — the failure that matters — a
    // producer that restated an unchanged layout would ping-pong forever with
    // the client that reported it, since applying what it is told is what a
    // mirror does.
    const WindowLayout& layout() const noexcept { return layout_; }
    bool set_layout(const WindowLayout& layout) noexcept;
    // Forgets where the window was, which is what a terminal entering a session
    // it has never been arranged in has to do: a `z_order` is a position among
    // windows, and one carried into a stack it was never in names a place
    // nobody put it. Zero size is "no layout reported" on the wire, which is
    // the same thing a terminal nobody has moved yet says.
    bool clear_layout() noexcept { return set_layout(WindowLayout{}); }
    bool layout_announced() const noexcept { return layout_announced_; }
    void note_layout_announced() noexcept { layout_announced_ = true; }

    // The name the READER gave this terminal, or empty for none.
    //
    // Here rather than on the client for exactly the reason the layout above
    // is here: a session outlives its clients, and a reader who named a
    // terminal expects to find that name when they come back. It is also the
    // only place two clients watching the same session can agree on it.
    //
    // It does not replace what the child says. `status().title` goes on
    // recording whatever the program claims with OSC 0/2 underneath this, so
    // "use the default title again" has a CURRENT answer to hand back to
    // rather than whatever the caption happened to say when the reader pinned
    // it — which is the whole difference between an override and one more
    // writer of the same string.
    //
    // `set_custom_title` answers whether anything actually changed, and only a
    // change clears the announcement — the same edge trigger the marks and the
    // layout use, and for the same two reasons: an unchanged name costs no
    // message, and a producer that restated one would ping-pong forever with
    // the client that applies what it is told.
    const std::string& custom_title() const noexcept { return custom_title_; }
    bool set_custom_title(std::string title);
    bool custom_title_announced() const noexcept { return custom_title_announced_; }
    void note_custom_title_announced() noexcept { custom_title_announced_ = true; }

    // --- The virtual printer's spool (PRINT-1…6) ---------------------
    //
    // What this terminal has captured and not yet been told to let go of.
    //
    // Here rather than in the emulator because the emulator DRAINS: ckVision
    // hands each finished job over once through `take_printer_jobs()` and
    // forgets it, which is what keeps a snapshot from carrying a megabyte of
    // captured text (the terminal-emulation spec's `take_printer_jobs` note). Something has to
    // hold what it hands over, and it has to be the same place the layout and
    // the reader's name for this terminal live — a session outlives its
    // clients, and a print a reader has not answered yet is exactly the thing
    // they come back to.
    //
    // The TEXT is held and never volunteered. A job travels as metadata on
    // `PrintJobAdded` and in the attach snapshot; the payload moves only when
    // a client asks for it with `PrintJobFetch`, because a reader who never
    // opens the preview should not have paid to ship it.
    struct HeldJob {
        std::uint64_t id = 0;
        proto::PrintJobKind kind = proto::PrintJobKind::Screen;
        std::string text;
        // Lines, counted once when the job arrives rather than on every
        // listing: the metadata is sent on the tick and again in every
        // snapshot, and counting newlines through a megabyte each time would
        // be work proportional to what a reader has NOT looked at.
        std::uint32_t lines = 0;
        std::int64_t at = 0;
        // The emulator freed this job's buffer rather than truncating it. The
        // text is empty on purpose — a host that showed the first megabyte of
        // a longer document as though it were the document would be lying
        // about what was printed — and a reader is told so rather than shown
        // a short job.
        bool overflowed = false;
    };
    // What THIS terminal says about printing, overriding its session and the
    // global setting for whichever of the three numbers it names. Stored per
    // terminal rather than resolved once and cached, because the answer
    // depends on scopes above it that a reader can change at any time — a
    // cached copy would be a fourth place the policy lives.
    const PrinterOverride& printer_override() const noexcept { return printer_; }
    void set_printer_override(const PrinterOverride& printer) { printer_ = printer; }

    std::span<const HeldJob> print_jobs() const noexcept { return jobs_; }
    // Takes whatever the emulator has finished and adds it to the spool,
    // stamping each with `now`. Returns the ids added, so the caller announces
    // exactly what arrived rather than diffing the list.
    std::vector<std::uint64_t> collect_print_jobs(std::int64_t now);
    const HeldJob* print_job(std::uint64_t id) const noexcept;
    // Forgets one job, or — with id 0 — every job this terminal holds, which
    // is what the frame button's "discard" means with nothing selected.
    // Answers whether anything was actually forgotten.
    bool discard_print_jobs(std::uint64_t id) noexcept;
    // Says the printer's numbers moved for a reason the emulator does not know
    // about — a job discarded, a policy changed. The tick sends `PrintState`
    // off the emulator's own damage flag, which nothing outside it can set.
    // The printer policy, changed on a terminal that is already running —
    // ckVision's own setter, forwarded, because a reader changing a preference
    // must not have to lose their shell to it.
    void set_printer_policy(ckv::term::TerminalPrinterPolicy policy);
    void set_printer_spool_limit(std::size_t bytes);
    void mark_printer_news() noexcept { printer_news_ = true; }
    bool take_printer_news() noexcept {
        const bool news = printer_news_;
        printer_news_ = false;
        return news;
    }

    // Whether the last thing to change this terminal's screen was a client
    // resizing it rather than its child saying something. Read once and
    // cleared, on the tick.
    //
    // A resize damages every row — the emulator has no way to tell a reader's
    // window from a program's output, and after a resize nothing on the old
    // screen can be relied on — so without this, every window a reattaching
    // client lays out would mark itself as having news the moment it appeared,
    // and the activity marker would say "something happened here" about all of
    // them at once.
    bool take_client_resize() noexcept;

    std::span<const ckv::term::WaitHandle> wait_handles() const noexcept {
        return session_->wait_handles();
    }
    int file_descriptor() const noexcept { return session_->file_descriptor(); }

    // Asks the child to end and returns at once — SIGHUP then SIGTERM, sent by
    // ckVision, which is the only thing here that knows the process group. A
    // server killing a session asks every terminal, keeps serving, and decides
    // for itself how long to wait; the waiting is a reader's setting, not a
    // library's constant.
    void request_termination() noexcept { session_->request_termination(); }
    // Ends the child now and returns at once — SIGKILL on the process group,
    // again sent by ckVision. What a server does at the end of a grace it timed
    // itself, so that the escalation costs a signal rather than the seconds
    // `close()` spends reaping inside a loop with other terminals to serve.
    void request_kill() noexcept { session_->request_kill(); }
    // SIGHUP → SIGTERM → grace → SIGKILL, on the process group, in ckVision.
    // Idempotent: a client that asks twice is not an error.
    void close() noexcept;
    bool closed() const noexcept { return closed_; }

private:
    TerminalId id_;
    std::unique_ptr<ckv::term::PosixTerminalSubsession> session_;
    int columns_ = 0;
    int rows_ = 0;
    int pixel_width_ = 0;
    int pixel_height_ = 0;
    TerminalSpec spec_;
    std::optional<int> exit_status_;
    bool closed_ = false;
    bool exit_announced_ = false;
    bool bell_marked_ = false;
    bool activity_marked_ = false;
    bool resized_by_a_client_ = false;
    // True until a mark moves. A terminal nobody has ever marked has nothing to
    // say, so the announcement starts as already made.
    bool marks_announced_ = true;
    std::uint32_t bell_serial_ = 0;
    std::uint32_t activity_serial_ = 0;
    WindowLayout layout_;
    // And true until a window moves, for the same reason: a terminal whose
    // place nobody has stated has no place to state.
    bool layout_announced_ = true;
    std::string custom_title_;
    PrinterOverride printer_;
    bool printer_news_ = false;
    std::vector<HeldJob> jobs_;
    // Never reused within this terminal's life, for the reason a terminal id
    // is not (the protocol spec, invariant 3): a client that asks for a job after it
    // was discarded must be told there is no such job, not handed a different
    // one — and a preview showing somebody else's captured output is a worse
    // failure than an error.
    std::uint64_t next_job_id_ = 1;
    // True until a reader names it, once more for the same reason: a terminal
    // nobody has named has no name to announce.
    bool custom_title_announced_ = true;
};

// Every terminal this server owns.
//
// Sessions come later (WP-8); this is the layer under them, and it is separate
// because "who owns the programs" and "how they are grouped for a reader" are
// different questions. A terminal here belongs to the server, not to a client:
// that is the promise of the whole project, and it is why closing a client
// does nothing to this collection.
class Terminals {
public:
    // The configuration a terminal is opened with — scrollback, mouse
    // reporting, alternate scroll, Sixel and its bound, OSC 52, the printer
    // policy. The server owns the emulator, so the server owns those
    // (the configuration spec); a client cannot be trusted with them and should not have to
    // repeat them per terminal.
    explicit Terminals(Settings settings);

    // Opens a terminal. A launch failure is a terminal in `Failed` state
    // rather than a null: the client has a window for it and a reader needs to
    // be told what went wrong in that window, not left with a gap where one
    // was about to be.
    Terminal& open(const TerminalSpec& spec);

    // Runs a terminal's command again, in place. False when there is no such
    // terminal, or when its child is still alive — respawning a running
    // program would leave the old child with nobody holding it, which is the
    // one outcome a multiplexer must never produce.
    bool respawn(TerminalId id);

    Terminal* find(TerminalId id) noexcept;
    const Terminal* find(TerminalId id) const noexcept;
    std::size_t size() const noexcept { return terminals_.size(); }
    std::vector<TerminalId> ids() const;

    // Drains every terminal and returns how many produced output. Exit is
    // observed here too, because the PTY reaching its read boundary IS the child
    // exiting and the two cannot be separated.
    //
    // The budget is for the whole PASS, shared out, not one budget each.
    // Parsing is what a drain costs — 64 KiB of a flooding child is 22 ms of
    // real work, two thirds of a frame at 30 fps — so a per-terminal budget
    // means the pass costs that much again for every flooding child, and the
    // loop stops answering anything else. Each terminal is guaranteed a floor
    // so that many quiet terminals cannot starve one busy one, and the walk
    // starts where the last one left off so the same terminal is not always
    // served first.
    std::size_t drain(std::size_t byte_budget_for_the_pass);
    // The smallest share a terminal gets, however many there are.
    static constexpr std::size_t kMinimumDrainShare = 4u * 1024u;

    // Every PTY this server is waiting on, for one `poll()` over all of them.
    std::vector<ckv::term::WaitHandle> wait_handles() const;

    // Closes a terminal and forgets it. Returns false if there is no such
    // terminal — a client naming one that has already gone is stale rather
    // than wrong, and it happens every time a reattach races a close.
    bool close(TerminalId id);
    // Closes everything, in the order it was opened. Called on shutdown, and
    // by the destructor, because a child outliving the server that owns it is
    // the one outcome this whole file exists to prevent.
    void close_all();

    ~Terminals();
    Terminals(const Terminals&) = delete;
    Terminals& operator=(const Terminals&) = delete;

private:
    Settings settings_;
    std::vector<std::unique_ptr<Terminal>> terminals_;
    // Monotonic, never reused. Starts at 1 so that 0 can mean "no terminal"
    // on the wire without a separate flag.
    TerminalId next_id_ = 1;
    // Where the next pass starts its round robin.
    std::size_t drain_cursor_ = 0;
};

// The launch spec a terminal is built from, as the server composes it out of
// configuration and a request. Exposed because it is worth testing on its own:
// what a child is told about its terminal is a decision, and the profile it
// gets decides what it will try to draw.
ckv::term::TerminalLaunchSpec launch_spec_for(const Settings& settings, const TerminalSpec& spec);
ckv::term::TerminalSubsessionOptions subsession_options_for(const Settings& settings);

}  // namespace ckm::server
