// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// Handing copied text to a helper program — `pbcopy`, `xclip`, or whatever a
// reader named in `[terminal] clipboard` (plans/08-configuration.md).
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

// Runs `command` through `/bin/sh -c` and writes `text` to its standard
// input. Returns whether the helper both started and exited successfully.
//
// The text goes on stdin and never into the command line: an argument is
// visible to every process on the machine through `ps`, and what is being
// copied is, by definition, something the reader selected — a password as
// often as a filename.
bool write_to_command(const std::string& command, std::string_view text);

}  // namespace ckm::platform
