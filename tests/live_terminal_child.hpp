// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#if defined(_WIN32)
#include <chrono>
#include <thread>
#endif

#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/desktop.hpp"
#include "cvision/widgets/terminal_view.hpp"
#if defined(_WIN32)
#include "cvision/term/terminal_subsession.hpp"
#endif

namespace ckmtest {

// These UI fixtures need an actual live child, with no prompt or unsolicited
// output. Both programs consume input until EOF; Windows uses the exact helper
// from this build, not an installed program or an emulated terminal source.
inline std::string live_terminal_child() {
#if defined(_WIN32)
    return CKMUX_TEST_CHILD_PATH;
#else
    return "/bin/cat";
#endif
}

// A window around a failed launch can pass purely visual assertions. Require
// the intended number of actual live children before exercising their UI.
inline void check_live_terminal_children(const ckv::widgets::Desktop& desktop,
                                         std::size_t expected = 1) {
    std::size_t seen = 0;
    for (const auto* window : desktop.windows()) {
        const auto* view = dynamic_cast<const ckv::widgets::TerminalView*>(window->content());
        if (view == nullptr) continue;
        ++seen;
        CK_CHECK(view->session().process_id() > 0);
        CK_CHECK(view->session().state() != ckv::core::TerminalSubsessionState::Failed);
        CK_CHECK(view->session().state() != ckv::core::TerminalSubsessionState::Exited);
    }
    CK_CHECK(seen == expected);
}

// Caption tests inject OSC bytes only after the real Windows child's response
// has traversed ConPTY. Its startup title can otherwise arrive after the
// injected title and legitimately replace it. The upper-case response proves
// the child ran (lower-case console input echo cannot satisfy this boundary).
// This is fixture synchronization, not suppression of application output.
inline void settle_live_terminal_children(const ckv::widgets::Desktop& desktop) {
#if defined(_WIN32)
    for (const auto* window : desktop.windows()) {
        auto* view = dynamic_cast<ckv::widgets::TerminalView*>(window->content());
        if (view == nullptr) continue;
        auto& session = view->session();
        auto* process = dynamic_cast<ckv::term::TerminalSubsession*>(&session);
        CK_CHECK(process != nullptr);
        if (process == nullptr) continue;
        const std::string identity = std::to_string(session.process_id());
        const std::string response = "CKMUX-READY-" + identity;
        session.send_input("ckmux-ready-" + identity + "\r");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool ready = false;
        do {
            (void)process->drain(64 * 1024);
            std::string text;
            for (const auto& cell : session.snapshot().cell_buffer)
                if (!cell.is_continuation()) text += cell.grapheme();
            ready = text.find(response) != std::string::npos;
            if (ready || session.state() == ckv::core::TerminalSubsessionState::Failed ||
                session.state() == ckv::core::TerminalSubsessionState::Exited) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        CK_CHECK(ready);
        if (ready) session.feed_output("\x1b]2;\a\x1b[2J\x1b[H");
    }
#else
    (void)desktop;
#endif
}

}  // namespace ckmtest
