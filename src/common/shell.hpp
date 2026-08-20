// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Which shell a new terminal runs, and how it is started.
//
// Both answers are tmux's, deliberately: a reader coming to ckmux from tmux
// has tmux's behaviour in their fingers and their dotfiles written against
// it, and a multiplexer that resolved the shell differently would be wrong in
// a way that only shows up as "my PATH is missing" three commands later. The
// behaviour below was read off tmux 3.6b rather than off its manual.
#pragma once

#include <string>
#include <vector>

namespace ckm {

// The three parts of an exec: what to run, what to call it, and what to hand
// it. argv0 is separate because a login shell is one only by virtue of its
// own argv[0] beginning with '-'.
struct ShellLaunch {
    std::string executable;
    std::string argv0;  // empty means "the executable path", as usual
    std::vector<std::string> arguments;
};

// tmux's `default-shell`: the first of $SHELL, the passwd entry's shell, and
// /bin/sh that is an absolute path to something executable which is not
// ckmux itself. That last guard is not hypothetical — a reader who has set
// ckmux as their login shell would otherwise get a terminal containing a
// multiplexer containing a terminal, without end.
std::string resolve_shell();

// How that shell is started. `login` is tmux's own default (its
// `default-command` is empty, which is what selects the login shell) and
// ckmux's:
//
//   login = true   argv[0] becomes "-zsh". The shell reads its profile
//                  files, which is where a reader's PATH, their Homebrew
//                  setup and their language runtimes usually are. This is
//                  what login(1) does, what Terminal.app does, and what a
//                  terminal window everywhere else on the machine does.
//   login = false  the shell with "-i": interactive, but profile files are
//                  skipped. Right when ckmux itself was started from a
//                  shell that already ran them, and the reader would rather
//                  not pay for them again in every window.
// `shell` empty means the reader's own shell, resolved by `resolve_shell()`:
// that is what `[general] shell` defaults to, and doing it here rather than at
// each call site is what keeps a caller from producing a launch spec with no
// program in it.
ShellLaunch shell_launch(const std::string& shell, bool login);

}  // namespace ckm
