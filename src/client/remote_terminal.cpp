// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/remote_terminal.hpp"

#include <algorithm>
#include <utility>

namespace ckm::client {
namespace {

ckv::core::TerminalMouseEncoding encoding_of(std::uint32_t modes) {
    const bool reporting = (modes & static_cast<std::uint32_t>(proto::ModeBit::MouseReporting)) != 0;
    if (!reporting) return ckv::core::TerminalMouseEncoding::None;
    return (modes & static_cast<std::uint32_t>(proto::ModeBit::MouseEncodingSgr)) != 0
               ? ckv::core::TerminalMouseEncoding::Sgr
               : ckv::core::TerminalMouseEncoding::X10;
}

bool has(std::uint32_t modes, proto::ModeBit bit) {
    return (modes & static_cast<std::uint32_t>(bit)) != 0;
}

}  // namespace

RemoteTerminalSubsession::RemoteTerminalSubsession(std::uint64_t terminal,
                                                   ckv::term::TerminalCapabilityProfile profile,
                                                   Send send)
    : terminal_(terminal), profile_(std::move(profile)), send_(std::move(send)) {}

ckv::core::TerminalStatus RemoteTerminalSubsession::status() const {
    const GridState& state = mirror_.state();
    ckv::core::TerminalStatus status;
    status.cells = state.cells;
    status.cursor = ckv::CursorState{state.cursor.visible != 0,
                                     ckv::Point{state.cursor.column, state.cursor.row},
                                     static_cast<ckv::CursorShape>(state.cursor.style),
                                     state.cursor.blink != 0};
    status.alternate_buffer = has(state.modes, proto::ModeBit::AlternateBuffer);
    status.title = state.title;
    status.state = this->state();
    status.bracketed_paste_enabled = has(state.modes, proto::ModeBit::BracketedPaste);
    status.mouse_reporting_enabled = has(state.modes, proto::ModeBit::MouseReporting);
    status.mouse_encoding = encoding_of(state.modes);
    // The level the child asked for, not merely that it asked. Two bits hold
    // all four values `TerminalMouseTracking` has, so nothing that comes out of
    // this mask can be a level that does not exist — and a view deciding
    // whether to forward a motion reads this rather than guessing from
    // `mouse_reporting_enabled` (ckVision D-054).
    status.mouse_tracking = static_cast<ckv::core::TerminalMouseTracking>(
        (state.modes & proto::kMouseTrackingMask) >> proto::kMouseTrackingShift);
    status.application_cursor_keys = has(state.modes, proto::ModeBit::ApplicationCursorKeys);
    status.focus_reporting_enabled = has(state.modes, proto::ModeBit::FocusReporting);
    status.alternate_scroll_enabled = has(state.modes, proto::ModeBit::AlternateScroll);
    // The kitty enhancements the child has on, as the server reported them.
    // This is what `TerminalView` reads before it encodes a key, so a mirror
    // that answered None would send the legacy encoding to a program that had
    // just switched the legacy fallback off (M-R2). Five bits hold every flag
    // the protocol defines, so nothing out of this mask can be a flag that does
    // not exist.
    status.keyboard_flags = static_cast<ckv::core::TerminalKeyboardFlags>(
        (state.modes & proto::kKeyboardFlagsMask) >> proto::kKeyboardFlagsShift);
    // The clipboard serial, which is what a consumer compares against the one
    // it last acted on before asking for the text (ckVision's TerminalView).
    status.clipboard_serial = mirror_.clipboard_serial();
    // The printer, so a reattached client says what a local one would: while
    // the controller is on, the child's output is going to the printer and NOT
    // to the screen, and a terminal that did not say so reads as one that has
    // stopped responding.
    status.printer_controller_active = mirror_.printer_active();
    status.printer_pending_bytes = mirror_.printer_bytes();
    status.printer_jobs_ready = mirror_.printer_jobs();
    // `bell_serial` stays at zero, and that is honest rather than lazy: the
    // wire carries a MARK — has this terminal rung since the reader was last in
    // it — and not the count ckVision keeps for a host that flashes every bell
    // as it happens. A number invented here would be a count of nothing.
    status.exit_code = mirror_.exit_status();
    return status;
}

ckv::core::TerminalSnapshot RemoteTerminalSubsession::snapshot() const {
    // Built from the same scalars, plus copies of what a snapshot is for. Nothing
    // on the hot path uses this — a view reads status() and the borrowed spans
    // (ckVision L-53) — and the one caller that still needs it wants the whole
    // terminal as a value.
    const ckv::core::TerminalStatus scalars = status();
    const GridState& state = mirror_.state();
    ckv::core::TerminalSnapshot snapshot;
    snapshot.cells = scalars.cells;
    snapshot.cell_buffer = state.grid;
    snapshot.cursor = scalars.cursor;
    snapshot.alternate_buffer = scalars.alternate_buffer;
    snapshot.title = scalars.title;
    snapshot.state = scalars.state;
    const std::span<const ckv::Cell> history = mirror_.history();
    snapshot.scrollback.assign(history.begin(), history.end());
    snapshot.bracketed_paste_enabled = scalars.bracketed_paste_enabled;
    snapshot.mouse_reporting_enabled = scalars.mouse_reporting_enabled;
    snapshot.mouse_encoding = scalars.mouse_encoding;
    snapshot.mouse_tracking = scalars.mouse_tracking;
    snapshot.application_cursor_keys = scalars.application_cursor_keys;
    snapshot.focus_reporting_enabled = scalars.focus_reporting_enabled;
    snapshot.alternate_scroll_enabled = scalars.alternate_scroll_enabled;
    snapshot.keyboard_flags = scalars.keyboard_flags;
    // The clipboard text lives here rather than in the scalars, exactly as it
    // does upstream: a consumer sees the serial move in `status()` and comes
    // here for the payload, which is the one read this pair exists to make
    // rare. Empty after a snapshot, because a snapshot carries the watermark
    // and not the text (mirror.cpp).
    snapshot.clipboard_text = mirror_.clipboard_text();
    snapshot.clipboard_serial = scalars.clipboard_serial;
    const std::span<const ckv::core::TerminalDiagnostic> complaints = mirror_.diagnostics();
    snapshot.diagnostics.assign(complaints.begin(), complaints.end());
    snapshot.printer_controller_active = scalars.printer_controller_active;
    snapshot.printer_pending_bytes = scalars.printer_pending_bytes;
    snapshot.printer_jobs_ready = scalars.printer_jobs_ready;
    return snapshot;
}

ckv::core::TerminalSubsessionState RemoteTerminalSubsession::state() const noexcept {
    using State = ckv::core::TerminalSubsessionState;
    if (closed_) return State::Closed;
    if (mirror_.exited()) return State::Exited;
    // Ready until the server has said anything about it, for the same reason a
    // local terminal is: a program that has printed nothing yet is alive.
    return mirror_.sequence() == 0 && mirror_.cells().width == 0 ? State::Ready : State::Running;
}

void RemoteTerminalSubsession::feed_output(std::string_view bytes) {
    (void)bytes;
    // A routing mistake, not data. Counted rather than ignored: a terminal whose
    // output silently went nowhere is the hardest kind of fault to find, and a
    // test can now say this never happens.
    ++stray_output_;
}

void RemoteTerminalSubsession::resize(ckv::Size cells, ckv::Size cell_pixels) {
    // The profile's cell metric is the client's own measurement and is used for
    // drawing pictures at the right size, so it is kept locally even though the
    // grid size is the server's to confirm.
    if (cell_pixels.width > 0 && cell_pixels.height > 0) profile_.cell_pixels = cell_pixels;
    if (!send_) return;
    // THIS terminal's size, not the client's desktop.
    //
    // They are different numbers, and confusing them is invisible until somebody
    // looks at the screen: a terminal sized to the whole desktop, shown in a
    // window that is smaller by its frame, has only its bottom rows on view — so
    // a shell's prompt, printed at the top, sits above the window and the
    // terminal looks empty. That is precisely what it did, and every layer
    // underneath was working perfectly while it happened.
    proto::MoveResize resize;
    resize.term = terminal_;
    resize.rect.width = static_cast<std::uint16_t>(std::max(0, cells.width));
    resize.rect.height = static_cast<std::uint16_t>(std::max(0, cells.height));
    send_(resize);
}

void RemoteTerminalSubsession::send_input(std::string_view bytes) {
    if (bytes.empty() || !send_) return;
    proto::Input input;
    input.term = terminal_;
    input.bytes = std::string(bytes);
    send_(input);
}

bool RemoteTerminalSubsession::drain(std::size_t byte_budget) {
    (void)byte_budget;
    return false;
}

void RemoteTerminalSubsession::close() noexcept {
    if (closed_) return;
    closed_ = true;
    if (!send_) return;
    proto::CloseTerminal close_it;
    close_it.term = terminal_;
    send_(close_it);
}

}  // namespace ckm::client
