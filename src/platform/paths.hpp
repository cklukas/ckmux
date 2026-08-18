// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// Where ckmux's files live. One place decides, so the answer cannot differ
// between the code that reads a file and the dialog that writes it back.
#pragma once

#include <filesystem>

namespace ckm::platform {

// The value of an environment variable, or nullptr when it is unset OR set to
// the empty string.
//
// Set-but-empty is how a shell says nothing — `XDG_CONFIG_HOME= ckmux` — and
// reading it as a value is how a program comes to look for its configuration
// in "/ckmux/ckmux.conf" and to listen on "/ckmux-501/default.sock". It is one
// function rather than one per file that needs it because the places that turn
// the environment into a path have to agree, and two copies of a rule are a
// rule that will one day differ between them.
const char* environment_value(const char* name);

// The same, for a variable naming a directory to build a path under: nullptr
// as well when the value is not an absolute path.
//
// The XDG base-directory specification requires those variables to be absolute
// and says a relative one must be ignored. The consequence of honouring one is
// a configuration file — or a socket — resolved against whatever directory
// ckmux happened to be started in, which is a different file every time and a
// server nobody can find twice.
const char* environment_directory(const char* name);

// The configuration file, following plans/08-configuration.md's precedence:
//
//   1. $CKMUX_CONFIG, used exactly as given — the explicit override, and
//      what a test or a parallel dev instance sets.
//   2. $XDG_CONFIG_HOME/ckmux/ckmux.conf when that variable holds an absolute
//      path.
//   3. $HOME/.config/ckmux/ckmux.conf, on the same condition.
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

// The user's home directory: what the password database says, else `$HOME`
// when it is an absolute path, else `/`.
//
// The password database comes first on purpose. `$HOME` describes the
// environment a process was started in, and a server is a process some client
// happened to start — from a cron job, from another user's `su`, from a shell
// whose HOME was set to a build directory. The passwd entry describes the
// USER, which is what "where does a terminal open" is actually asking about,
// and it is the same answer whichever client started the server.
//
// Never empty, and always absolute: a relative working directory is resolved
// against wherever the server happened to be started, which is a different
// directory every time.
std::filesystem::path home_directory();

}  // namespace ckm::platform
