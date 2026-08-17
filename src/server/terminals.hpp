// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The terminals a ckmux server owns (plans/02-architecture.md, WP-3).
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
// `widgets` or `scene` (AGENTS.md), so the server builds and runs with no host
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
#include "cvision/term/posix_terminal_subsession.hpp"

namespace ckm::server {

// Never recycled within a server's lifetime (plans/05, invariant 3). A client
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
    // protocol carries (plans/05 `Attach`) because it is what a client can
    // actually measure. ckVision wants the size of ONE CELL, so the conversion
    // happens here — see `Terminal::resize`. Zero is honest ignorance rather
    // than a size.
    int pixel_width = 0;
    int pixel_height = 0;
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
    ckv::term::TerminalSnapshot snapshot() const { return session_->snapshot(); }
    ckv::term::TerminalSnapshot snapshot(ckv::core::TerminalSnapshotOptions options) const {
        return session_->snapshot(options);
    }

    // Child bytes in, child bytes out. Input is already encoded by the client
    // against mirrored mode state (plans/04), so this writes it without
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

    std::span<const ckv::term::WaitHandle> wait_handles() const noexcept {
        return session_->wait_handles();
    }
    int file_descriptor() const noexcept { return session_->file_descriptor(); }

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
    std::optional<int> exit_status_;
    bool closed_ = false;
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
    // (plans/08); a client cannot be trusted with them and should not have to
    // repeat them per terminal.
    explicit Terminals(Settings settings);

    // Opens a terminal. A launch failure is a terminal in `Failed` state
    // rather than a null: the client has a window for it and a reader needs to
    // be told what went wrong in that window, not left with a gap where one
    // was about to be.
    Terminal& open(const TerminalSpec& spec);

    Terminal* find(TerminalId id) noexcept;
    const Terminal* find(TerminalId id) const noexcept;
    std::size_t size() const noexcept { return terminals_.size(); }
    std::vector<TerminalId> ids() const;

    // Drains every terminal, each under its own budget, and returns how many
    // produced output. Exit is observed here too, because the PTY reaching its
    // read boundary IS the child exiting and the two cannot be separated.
    std::size_t drain(std::size_t byte_budget_per_terminal);

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
};

// The launch spec a terminal is built from, as the server composes it out of
// configuration and a request. Exposed because it is worth testing on its own:
// what a child is told about its terminal is a decision, and the profile it
// gets decides what it will try to draw.
ckv::term::TerminalLaunchSpec launch_spec_for(const Settings& settings, const TerminalSpec& spec);
ckv::term::TerminalSubsessionOptions subsession_options_for(const Settings& settings);

}  // namespace ckm::server
