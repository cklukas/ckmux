// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Handing copied text to a helper program — `pbcopy`, `xclip`, or whatever a
// reader named in `[terminal] clipboard` (the configuration spec).
//
// It is here rather than in the client because it forks: the client is
// deterministic and testable precisely because it does not, and a test that
// exercised copy mode would otherwise depend on which clipboard helpers the
// machine running it happens to have. `ClientOptions::clipboard_writer` is
// the seam — the real host passes this function, a test passes a recorder.
#pragma once

#include <string>
#include <string_view>

namespace ckm::platform {

// How long a helper may make no progress at all before ckmux stops waiting for
// it. The budget starts again on every byte written or read, so a slow helper
// is never cut off for being slow — only for being stuck.
//
// The client is one thread around one loop: every millisecond spent in here is
// a millisecond in which nothing is drawn, no key is read, and no message from
// the server is applied. A helper that has taken nothing and said nothing for
// two seconds is not working — `ssh elsewhere pbcopy` to a machine that has
// gone away is the case that made this a deadline instead of a hope — and a
// frozen multiplexer is a worse answer than a failed copy.
inline constexpr int kClipboardIdleBudgetMs = 2000;

// Runs `command` through `/bin/sh -c` and writes `text` to its standard
// input. Returns whether the helper both started and exited successfully.
//
// The text goes on stdin and never into the command line: an argument is
// visible to every process on the machine through `ps`, and what is being
// copied is, by definition, something the reader selected — a password as
// often as a filename.
//
// The helper's own stdout and stderr are captured, never inherited. ckmux is
// drawing a screen on those descriptors: a helper that prints "command not
// found" would print it into the frame, where it corrupts whatever cells it
// lands on and survives until something else redraws them. `diagnostics`, when
// given, receives what the helper said (bounded — enough to name the problem,
// not enough for a runaway helper to be a memory leak) so the failure can be
// shown where a reader will actually read it. Nothing is written to it on
// success worth showing unprompted.
//
// `idle_budget_ms` is the deadline above, as a parameter so a test does not
// have to wait out the real one.
bool write_to_command(const std::string& command, std::string_view text,
                      std::string* diagnostics = nullptr,
                      int idle_budget_ms = kClipboardIdleBudgetMs);

}  // namespace ckm::platform
