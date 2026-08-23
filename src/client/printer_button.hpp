// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The frame button that says a program in this window is printing (PRINT-3,
// The interface spec "Captured print output").
//
// A one-row overlay on the window's bottom border, left-aligned. It exists
// because printer-controller mode **freezes the screen by design** — every
// byte the child sends goes to the spool instead of the grid — so a reader is
// left looking at a motionless window with no way to tell a busy program from
// a wedged one. The live byte counter is the answer to that, and it is the
// reason this is a counter rather than a static badge.
//
// Four states, because the reader's next action differs in each and a button
// that could not tell them apart would send them to the wrong place:
//
//     ask, unanswered      [ PRINT? · 12.4 KB ]     -> the Ask popup
//     answered / capture   [ PRINT · 3 · 1.2 MB ]   -> Print Output
//     ask cache overflowed [ Print settings ]       -> Printer Settings
//     capture spool full   [ PRINT · 2 · 1 MB — full ]  -> Print Output
//
// The two that matter most are the two a boolean cannot separate: "a document
// is being kept" and "a document is being thrown away". A button that said
// "capturing" while the terminal was sinking would be promising a reader
// something it is not keeping.
//
// It draws nothing at all when there is nothing to say, so an ordinary window
// carries no extra row and no reader is asked to ignore a permanent badge.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "common/proto.hpp"
#include "cvision/ui/view.hpp"

namespace ckm::client {

// What the button is currently about, derived once from the terminal's
// printer state rather than re-decided at each place that asks. The order is
// the order the states are checked in, and it is deliberate: sinking outranks
// capturing, because a reader must never be told a document is being kept
// while it is being discarded.
enum class PrinterButtonState {
    Hidden,     // nothing captured, nothing capturing: no button at all
    Asking,     // `ask` and unanswered — the counter is live
    Sunk,       // a job went over the ask cache; the capture is gone
    Holding,    // answered or `capture`: jobs are being kept
    Full,       // holding, and the spool is at its limit
};

// What one terminal's printer looks like right now, as the button reads it.
// A value rather than a pointer into the mirror, so the button can be built
// and asserted on in a test with no session behind it.
struct PrinterButtonModel {
    proto::PrinterMode mode = proto::PrinterMode::Ask;
    proto::PrinterState state = proto::PrinterState::Idle;
    // Bytes the job in progress has collected — the number that moves.
    std::uint64_t pending_bytes = 0;
    // Completed jobs this terminal is holding.
    std::size_t jobs = 0;
    // Whether the reader has answered the ask for this terminal. Held by the
    // client rather than the server: it is a fact about what this reader has
    // been shown, and a second client attaching has not been shown anything.
    bool answered = false;
};

PrinterButtonState printer_button_state(const PrinterButtonModel& model);

// The label, with byte counts through `stats_format`'s shared formatter —
// The interface spec requires one formatter everywhere, and a second one that agreed
// today would drift tomorrow.
std::string printer_button_label(const PrinterButtonModel& model);

class PrinterButton final : public ckv::ui::View {
public:
    PrinterButton();

    // Re-reads the model and repaints if the label changed. Called from the
    // client's own printer poll; the button never reaches for a terminal
    // itself, which is what lets a test drive it directly.
    void set_model(const PrinterButtonModel& model);
    const PrinterButtonModel& model() const noexcept { return model_; }
    PrinterButtonState state() const noexcept { return printer_button_state(model_); }
    const std::string& label() const noexcept { return label_; }

    // What a click means, which is the state's business rather than the
    // caller's: the four states open three different surfaces.
    std::function<void(PrinterButtonState)> on_activate;

    ckv::ui::SizeHint horizontal_size_hint() const override;
    ckv::ui::SizeHint vertical_size_hint() const override;
    void draw(ckv::scene::Painter& painter) override;
    bool on_mouse(const ckv::MouseEvent& event) override;
    std::optional<ckv::PointerShape> pointer_shape_at(ckv::Point local) const override;
    void on_attached() override;

private:
    PrinterButtonModel model_;
    std::string label_;
    ckv::ui::RoleId role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId pressed_role_ = ckv::ui::kInvalidRole;
    bool pressed_ = false;
};

}  // namespace ckm::client
