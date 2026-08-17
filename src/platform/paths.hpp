// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// Where ckmux's files live. One place decides, so the answer cannot differ
// between the code that reads a file and the dialog that writes it back.
#pragma once

#include <filesystem>

namespace ckm::platform {

// The configuration file, following plans/08-configuration.md's precedence:
//
//   1. $CKMUX_CONFIG, used exactly as given — the explicit override, and
//      what a test or a parallel dev instance sets.
//   2. $XDG_CONFIG_HOME/ckmux/ckmux.conf when that variable is set.
//   3. $HOME/.config/ckmux/ckmux.conf.
//
// The XDG layout is the convention for a program of this kind on Linux and,
// in practice, on macOS too: tmux, git, neovim and their neighbours all keep
// their configuration there, and a reader who has their dotfiles in one tree
// expects ckmux's to be in it as well. Apple's own Application Support
// directory is for data an application manages, not for a file its user is
// meant to open and edit.
//
// The path is returned whether or not anything exists at it, because both
// callers need that: reading treats a missing file as "every default", and
// writing has to know where to create one.
std::filesystem::path config_file_path();

}  // namespace ckm::platform
