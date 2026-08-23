// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/printer_button.hpp"

#include <algorithm>

#include "client/stats_format.hpp"
#include "cvision/core/text.hpp"
#include "cvision/scene/painter.hpp"
#include "cvision/ui/theme.hpp"

namespace ckm::client {
namespace {

namespace u = ckv::ui;

}  // namespace

PrinterButtonState printer_button_state(const PrinterButtonModel& model) {
    // `off` is not a state of this button, it is the absence of one: a reader
    // who turned capture off has said they do not want to be told about
    // printing, and a badge saying "nothing is being captured" would be the
    // notification they declined.
    if (model.mode == proto::PrinterMode::Off) return PrinterButtonState::Hidden;

    // Sinking first, and deliberately ahead of capturing. The two look
    // identical from outside — bytes arriving, controller on — and only one of
    // them is keeping anything. A reader told "capturing" through a sink would
    // be promised a document that no longer exists.
    if (model.state == proto::PrinterState::Sunk) return PrinterButtonState::Sunk;

    const bool holding = model.jobs > 0;
    const bool collecting = model.state == proto::PrinterState::Capturing;
    if (!holding && !collecting) return PrinterButtonState::Hidden;

    // Unanswered ask, while something is actually happening. The question is
    // about THIS reader: `answered` is client-side because a second client
    // attaching has not been asked anything yet.
    if (model.mode == proto::PrinterMode::Ask && !model.answered)
        return PrinterButtonState::Asking;

    if (model.state == proto::PrinterState::Full) return PrinterButtonState::Full;
    return PrinterButtonState::Holding;
}

std::string printer_button_label(const PrinterButtonModel& model) {
    switch (printer_button_state(model)) {
        case PrinterButtonState::Hidden:
            return {};
        case PrinterButtonState::Asking:
            // The counter, because a frozen screen with a moving number is a
            // program working and a frozen screen with a still one is not.
            return "[ PRINT? · " + format_bytes(model.pending_bytes) + " ]";
        case PrinterButtonState::Sunk:
            // Not a count and not a size: there is nothing to show and the
            // only useful next move is the settings that would have let it
            // through. Saying "0 KB" here would read as "nothing printed".
            return "[ Print settings ]";
        case PrinterButtonState::Holding:
            return "[ PRINT · " + std::to_string(model.jobs) + " · " +
                   format_bytes(model.pending_bytes) + " ]";
        case PrinterButtonState::Full:
            // The count is still true and still worth opening; what is added
            // is that nothing more will be kept.
            return "[ PRINT · " + std::to_string(model.jobs) + " · " +
                   format_bytes(model.pending_bytes) + " — full ]";
    }
    return {};
}

PrinterButton::PrinterButton() { set_focus_policy(u::FocusPolicy::None); }

void PrinterButton::set_model(const PrinterButtonModel& model) {
    std::string wanted = printer_button_label(model);
    const bool width_moved = ckv::text::text_width(wanted) != ckv::text::text_width(label_);
    if (wanted == label_ && model.answered == model_.answered) {
        model_ = model;
        return;
    }
    model_ = model;
    label_ = std::move(wanted);
    // A label that changed WIDTH changes what this overlay asks its frame for,
    // and a frame that was not told would go on placing the old width — the
    // same defect ckVision's ClockView fixed for the menu bar's trailing view.
    if (width_moved) size_hint_changed();
    invalidate();
}

u::SizeHint PrinterButton::horizontal_size_hint() const {
    const int width = ckv::text::text_width(label_);
    return u::SizeHint{width, width, width};
}

u::SizeHint PrinterButton::vertical_size_hint() const { return u::SizeHint{1, 1, 1}; }

void PrinterButton::on_attached() {
    // The window frame's own roles: this sits ON the frame, and a theme that
    // retinted frames would otherwise leave one cell of the border in another
    // family's colours.
    if (role_ == u::kInvalidRole) role_ = context().roles->find("ckv.window.frame.active");
    if (pressed_role_ == u::kInvalidRole)
        pressed_role_ = context().roles->find("ckv.window.control.pressed");
}

void PrinterButton::draw(ckv::scene::Painter& painter) {
    if (label_.empty()) return;
    const ckv::Style style = context().theme->resolve(pressed_ ? pressed_role_ : role_);
    painter.draw_text(ckv::Point{0, 0}, label_, style);
}

bool PrinterButton::on_mouse(const ckv::MouseEvent& event) {
    if (label_.empty()) return false;
    if (event.button != ckv::MouseButton::Left) return false;
    if (event.action == ckv::MouseAction::Down) {
        // Shown before it acts, like every other pressable thing in this
        // toolkit: a control that acts with no acknowledgement leaves a reader
        // unsure it was hit.
        pressed_ = true;
        invalidate();
        return true;
    }
    if (event.action != ckv::MouseAction::Up) return false;
    const bool was_pressed = pressed_;
    pressed_ = false;
    invalidate();
    if (!was_pressed) return false;
    // Released away from the button takes the click back — the press was
    // claimed, so the release is consumed either way.
    const ckv::Rect abs = absolute_bounds();
    const bool over = event.cell.y == abs.y && event.cell.x >= abs.x &&
                      event.cell.x < abs.x + ckv::text::text_width(label_);
    if (over && on_activate) on_activate(state());
    return true;
}

std::optional<ckv::PointerShape> PrinterButton::pointer_shape_at(ckv::Point local) const {
    if (label_.empty()) return std::nullopt;
    if (local.y != 0 || local.x < 0 || local.x >= ckv::text::text_width(label_))
        return std::nullopt;
    return ckv::PointerShape::Pointer;
}

}  // namespace ckm::client
