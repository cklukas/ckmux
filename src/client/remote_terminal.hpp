// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// A terminal that lives in another process, presented as one of ckVision's
// (WP-5, the ckVision integration spec seam 2).
//
// This is the junction the whole M2 design turns on. `TerminalView` is reused
// **verbatim** — its key encoding, its mouse translation against the child's
// chosen protocol, its selection, its scrollback paging, its Sixel drawing, its
// exited banner — because everything it needs is behind
// `core::TerminalSubsession`, and this implements that seam over mirror state
// instead of over an emulator. Nothing in ckmux's window, footer or prefix code
// knows the difference, which is the acceptance criterion for this package and
// also the reason it was worth designing the client this way in M1.
//
// Two directions, and they are not symmetrical:
//
//   * **Out**: `send_input` becomes an `Input` message and nothing else. The
//     bytes were encoded here, against mirrored mode state, so the server writes
//     them to the PTY without understanding them (the terminal-emulation spec). That is what keeps
//     the child's dialect a matter between the child and the view that mirrors
//     it, rather than a third thing the server has to agree about.
//   * **In**: nothing. Output arrives as protocol messages, which the client
//     routes into the mirror; `feed_output` on a remote terminal is meaningless
//     and says so.
#pragma once

#include <functional>
#include <string>

#include "client/mirror.hpp"
#include "common/proto.hpp"
#include "cvision/term/terminal_subsession.hpp"

namespace ckm::client {

class RemoteTerminalSubsession final : public ckv::term::TerminalSubsession {
public:
    // Everything this needs from the client: which terminal it is, and how to
    // send a message. A callback rather than a connection, because the client
    // owns the socket and its queue, and because a test then needs no socket at
    // all to say what a keystroke turns into.
    using Send = std::function<void(const proto::Message&)>;

    RemoteTerminalSubsession(std::uint64_t terminal, ckv::term::TerminalCapabilityProfile profile,
                             Send send);

    std::uint64_t terminal_id() const noexcept { return terminal_; }
    TerminalMirror& mirror() noexcept { return mirror_; }
    const TerminalMirror& mirror() const noexcept { return mirror_; }

    // --- The seam ---------------------------------------------------------

    ckv::core::TerminalSnapshot snapshot() const override;
    ckv::core::TerminalStatus status() const override;
    const ckv::core::TerminalDamage& damage() const noexcept override { return mirror_.damage(); }
    void clear_damage() noexcept override { mirror_.clear_damage(); }
    // The server is the one holding a child's open frame (WP-16's own diff
    // engine gates its flush on this); by the time this mirror sees a delta
    // at all, the frame it came from has already closed.
    bool synchronized_output_active() const noexcept override { return false; }
    std::span<const ckv::Cell> cells() const noexcept override { return mirror_.grid(); }
    std::span<const ckv::Cell> scrollback() const noexcept override { return mirror_.history(); }
    // Pictures arrive as their own messages and are placed by id (WP-16);
    // the mirror holds them exactly as it holds the grid, and the view draws
    // them the same way it draws a local terminal's.
    std::span<const ckv::core::TerminalRaster> rasters() const noexcept override {
        return mirror_.rasters();
    }
    // What this terminal last had to complain about, as the server said it.
    // One entry rather than the emulator's ring: what a view paints is the most
    // recent complaint, and a transport that shipped the whole ring would
    // re-send entries the client already holds every time one arrived.
    std::span<const ckv::core::TerminalDiagnostic> diagnostics() const noexcept override {
        return mirror_.diagnostics();
    }
    const ckv::core::TerminalCapabilityProfile& profile() const noexcept override {
        return profile_;
    }

    // A mirror is fed deltas, not child bytes. Nothing calls this on a remote
    // terminal; if something does, it is a routing mistake worth finding rather
    // than bytes worth swallowing, so it is counted.
    void feed_output(std::string_view bytes) override;
    std::size_t stray_output_calls() const noexcept { return stray_output_; }

    // The client's own geometry, on its way to the server. The server owns the
    // PTY, so a resize here is a request: what makes the child's `TIOCSWINSZ`
    // change is the server acting on it (WP-3), and what makes this mirror
    // change size is the snapshot or delta that comes back.
    void resize(ckv::Size cells, ckv::Size cell_pixels) override;

    void send_input(std::string_view bytes) override;
    // A mirror answers no queries: DA, DSR and XTWINOPS are answered by the
    // server's emulator, to the child, and never travel to a client (WP-3).
    std::string take_pending_input() override { return {}; }

    ckv::core::TerminalSubsessionState state() const noexcept override;

    // Nothing to drain: a client reads one socket, not one descriptor per
    // terminal, and the loop that reads it is the client's own.
    bool drain(std::size_t byte_budget) override;
    std::span<const ckv::term::WaitHandle> wait_handles() const noexcept override { return {}; }

    // Closing a window asks the server to close the terminal. The window's own
    // confirmation and banner rules are the client's (the session model), and unchanged:
    // this is only the message.
    void close() noexcept override;

    void set_raster_identity(int identity) noexcept override {
        raster_identity_ = identity;
        // Into the mirror, because the mirror is what builds the rasters the
        // view reads — an identity held only here would leave them at 0, and
        // TerminalView drops a raster whose id is 0.
        mirror_.set_raster_identity(identity);
    }
    int raster_identity() const noexcept { return raster_identity_; }

private:
    std::uint64_t terminal_;
    ckv::term::TerminalCapabilityProfile profile_;
    Send send_;
    TerminalMirror mirror_;
    int raster_identity_ = 0;
    std::size_t stray_output_ = 0;
    bool closed_ = false;
};

}  // namespace ckm::client
