// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>

#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/desktop.hpp"
#include "cvision/widgets/terminal_view.hpp"

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

}  // namespace ckmtest
