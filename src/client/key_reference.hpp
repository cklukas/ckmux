// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Documentation views of the default key registry. The interactive help,
// footer and dispatcher already read Keymap directly; these two renderings
// let build artifacts do the same instead of copying the table into a man
// page or browser guide.
#pragma once

#include <string>

namespace ckm::client {

enum class KeyAppendixFormat : unsigned char { Markdown, Roff };

// A complete default-key appendix, including commands reached only through a
// menu. The default prefix and Send Prefix chord come from Settings, so a
// changed built-in default changes every generated document in the same build.
std::string render_default_key_appendix(KeyAppendixFormat format);

}  // namespace ckm::client
