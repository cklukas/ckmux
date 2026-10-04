// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

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

}  // namespace ckmtest
