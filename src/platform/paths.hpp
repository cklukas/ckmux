// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Where ckmux's files live. One place decides, so the answer cannot differ
// between the code that reads a file and the dialog that writes it back.
#pragma once

#include <filesystem>
#include <string_view>

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

// The configuration file, following the configuration spec's precedence:
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

// A path a reader wrote, with a leading `~` resolved — the one place that
// decides what `~` means.
//
// Needed because a configured path is something a person typed, and `~` is
// shell notation the kernel has never heard of. `[printer] save-folder`
// defaults to `~/Documents`, and with nothing expanding it the first working
// save landed in `<cwd>/~/Documents/` — a directory literally named `~`,
// holding a byte-perfect file the reader would never have found.
//
// What is expanded, stated rather than left to be inferred from the code:
//
//   * `~`         -> the home directory itself.
//   * `~/rest`    -> home / rest.
//   * `~other`    -> UNCHANGED. Expanding another user's home from a
//                    configuration string is a different and more dangerous
//                    feature, and refusing it is a decision, not an omission.
//   * anything else, including the empty string -> unchanged.
//
// `$HOME` wins over the passwd database here, which is the opposite of
// `home_directory()`'s precedence and is deliberate: `~` is shell notation and
// a shell expands it from `$HOME`, so a deliberately overridden `HOME` — a
// test harness, a sandbox, a service account — must be honoured rather than
// silently replaced by the account's real home. `home_directory()` is the
// fallback, so the answer is still never empty and always absolute even when
// `HOME` is unset or relative; a relative `HOME` is rejected rather than used,
// because a save resolved against the working directory is the same
// unfindable-file defect one notch milder.
std::filesystem::path expand_user_path(std::string_view path);

}  // namespace ckm::platform
