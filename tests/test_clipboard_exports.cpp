// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Public widget copy routes with an injected host service: no OS clipboard,
// shell, process, clock or filesystem lookup is needed for these assertions.
#include "client/client_app.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/input_line.hpp"
#include "cvision/widgets/static_text.hpp"
#include "cvision/widgets/window.hpp"

namespace {

class ControlledClipboardWriter final : public ckv::ClipboardWriter {
public:
    ckv::ClipboardWriteResult next{ckv::ClipboardWriteStatus::Unavailable, 17};
    unsigned attempts = 0;
    ckv::ClipboardWriteResult write_text(std::string_view) override {
        ++attempts;
        return next;
    }
};

ckm::client::ClientOptions options_without_child() {
    ckm::client::ClientOptions options;
    options.open_terminal_at_startup = false;
    options.settings.shell = "injected-unused-shell";
    // This must never be quoted for a host-only failure.
    options.clipboard_problem = [] { return "stale helper diagnostic"; };
    return options;
}

std::string static_text_in(ckv::ui::View& view) {
    std::string result;
    if (auto* text = dynamic_cast<ckv::widgets::StaticText*>(&view)) result = text->text();
    for (const auto& child : view.children()) result += "\n" + static_text_in(*child);
    return result;
}

struct Fixture {
    ckv::term::HeadlessTerminal terminal{ckv::Size{120, 40}};
    ckv::ManualClock clock;
    ControlledClipboardWriter writer;
    ckv::ui::Application app{terminal, clock, writer};
    ckm::client::ClientApp client{app, options_without_child()};
    ckv::widgets::InputLine* input = app.root().add(std::make_unique<ckv::widgets::InputLine>());

    void copy(std::string text, bool cut = false) {
        input->set_text(std::move(text));
        app.set_focus(input);
        app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::End, ckv::Modifier::None, ""}});
        app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Home, ckv::Modifier::Shift, ""}});
        CK_CHECK(input->has_selection());
        app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, cut ? "x" : "c"}});
        app.step(clock.now_nanos());
    }
    std::size_t warning_count() {
        std::size_t count = 0;
        for (auto* window : client.desktop().windows()) if (window->title() == "Copy") ++count;
        return count;
    }
    void dismiss() {
        app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}});
        app.step(clock.now_nanos());
        CK_CHECK(!app.is_modal());
    }
    std::string screen() {
        std::string result;
        const auto frame = app.current_frame();
        for (int y = 0; y < frame.size().height; ++y)
            for (int x = 0; x < frame.size().width; ++x)
                result += frame.at(ckv::Point{x, y}).grapheme();
        return result;
    }
};

}  // namespace

CK_TEST(widget_copy_reports_native_refusal_keeps_terminal_paste_text_and_recovers) {
    Fixture f;
    f.copy("retained copy");
    CK_CHECK(f.writer.attempts == 1);
    CK_CHECK(f.app.clipboard_text() == "retained copy");
    CK_CHECK(f.client.internal_clipboard() == "retained copy");
    CK_CHECK(f.warning_count() == 1 && f.app.is_modal());
    const auto message = static_text_in(f.app.root());
    CK_CHECK(message.find("currently unavailable") != std::string::npos);
    CK_CHECK(message.find("native error 17") != std::string::npos);
    CK_CHECK(message.find("ckmux kept it") != std::string::npos);
    CK_CHECK(message.find("stale helper diagnostic") == std::string::npos);
    CK_CHECK(f.screen().find("unavailable") != std::string::npos);
    f.dismiss();
    // A changing host service, under the same running application.
    f.writer.next = {ckv::ClipboardWriteStatus::Ok};
    f.copy("recovered copy");
    CK_CHECK(f.writer.attempts == 2);
    CK_CHECK(f.client.internal_clipboard() == "recovered copy");
    CK_CHECK(f.warning_count() == 0 && !f.app.is_modal());
}

CK_TEST(widget_cut_warns_of_possible_partial_host_change_without_losing_the_cut_text) {
    Fixture f;
    f.writer.next = {ckv::ClipboardWriteStatus::Error, 5, true};
    f.copy("cut text", /*cut=*/true);
    CK_CHECK(f.input->text().empty());
    CK_CHECK(f.client.internal_clipboard() == "cut text");
    CK_CHECK(f.warning_count() == 1);
    const auto message = static_text_in(f.app.root());
    CK_CHECK(message.find("native error 5") != std::string::npos);
    CK_CHECK(message.find("external clipboard may have changed") != std::string::npos);
}

CK_TEST(submitted_terminal_request_is_not_misreported_as_a_refused_widget_copy) {
    Fixture f;
    f.writer.next = {ckv::ClipboardWriteStatus::Submitted};
    f.copy("submitted, not confirmed");
    CK_CHECK(f.writer.attempts == 1);
    CK_CHECK(f.client.internal_clipboard() == "submitted, not confirmed");
    CK_CHECK(f.warning_count() == 0 && !f.app.is_modal());
}

CK_TEST(unsupported_and_invalid_widget_exports_are_reported_without_native_error_invention) {
    for (const auto status : {ckv::ClipboardWriteStatus::Unsupported, ckv::ClipboardWriteStatus::InvalidText}) {
        Fixture f;
        f.writer.next = {status};
        f.copy("still internal");
        CK_CHECK(f.warning_count() == 1);
        CK_CHECK(f.client.internal_clipboard() == "still internal");
        const auto message = static_text_in(f.app.root());
        CK_CHECK(message.find(status == ckv::ClipboardWriteStatus::Unsupported ?
                             "not supported" : "text rejected") != std::string::npos);
        CK_CHECK(message.find("native error") == std::string::npos);
    }
}

CK_TEST(expired_client_clipboard_observer_cannot_read_destroyed_client_state) {
    ckv::term::HeadlessTerminal terminal{ckv::Size{100, 30}};
    ckv::ManualClock clock;
    ControlledClipboardWriter writer;
    ckv::ui::Application app{terminal, clock, writer};
    {
        ckm::client::ClientApp client{app, options_without_child()};
    }
    CK_CHECK(app.set_clipboard_text("after client destruction") == writer.next);
    CK_CHECK(app.clipboard_text() == "after client destruction");
    CK_CHECK(!app.is_modal());
}
