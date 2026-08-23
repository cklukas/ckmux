// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The CLI subcommands (WP-11): `ls`, `new`, `kill-session` and `check-config`,
// over the same socket protocol the UI client speaks, with
// `client_kind = cli` (the architecture spec, the protocol spec).
//
// These live beside the client rather than in `main.cpp` because they ARE
// clients — they connect, handshake, ask one thing and leave — and because a
// function that returns an exit status can be tested, while a branch of
// `main()` can only be run. `main.cpp` stays what it is: the place that
// decides which of the program's roles an invocation is.
//
// Every one of them is a request and its answer and nothing else. None of
// them attaches, so none of them is ever sent a delta, and none of them holds
// a terminal — which is why `ckmux ls` on a busy server costs the server one
// message rather than a snapshot.
//
// `attach` is deliberately NOT here: it is the ordinary UI client with a
// session chosen in advance, so it belongs to `run_client.cpp` and arrives as
// `RunOptions::preselected_session`.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ckm::client {

// What a subcommand was asked to do, parsed from argv before anything
// connects. Parsing and doing are separated so the refusals — an unknown
// flag, a missing name — are answerable without a socket, and so a test can
// state an argument vector and read back what it meant.
struct CliRequest {
    // Empty when the arguments were not a subcommand at all.
    std::string subcommand;
    // `-s <name>` for `new`, or the positional name/id for `kill-session`.
    std::string name;
    // The trailing command words for `new`, joined as typed.
    std::string command;
    // Set when the arguments themselves were wrong; the text is the whole of
    // what a reader is told, and `usage` names the one-line form to follow it.
    std::string problem;
    std::string usage;
    // `ckmux attach --share`: join a session another reader may already be
    // watching instead of taking it over (WP-44). Opt-in because taking over
    // is what every reader before this flag got, and a shared session is a
    // different thing to be in — what one reader types, both see.
    bool share = false;
    // `ckmux attach --adopt-size`: make the session's desktop this client's
    // screen, once, on arrival (WP-40). Opt-in for the same reason the menu
    // item has no chord: it reflows every window and resizes every child, for
    // every reader watching, not only for whoever asked.
    bool adopt_size = false;

    bool ok() const noexcept { return problem.empty(); }
};

// Parses the argument vector for one subcommand. `arguments` excludes argv[0]
// and the subcommand itself is `arguments[0]`.
CliRequest parse_cli(const std::vector<std::string>& arguments);

// True for the words this build answers as CLI subcommands. `main()` asks
// before it decides to start a client, so a typo stays a refusal rather than
// opening a window (WP-11's whole reason for the unknown-argument path).
bool is_cli_subcommand(const std::string& word);

// `ckmux ls` — the sessions a server holds, one per line, or a sentence
// saying there is no server. Never starts one: asking what exists must not
// bring something into existence.
int run_ls(const std::filesystem::path& socket);

// `ckmux new [-s name] [command]` — a session, made without attaching to it.
int run_new(const std::filesystem::path& socket, const std::filesystem::path& executable,
            const CliRequest& request);

// `ckmux kill-session <name|id>` — every program in it is asked to end, on the
// configured grace. Refuses an ambiguous name rather than guessing which of
// two sessions a reader meant.
int run_kill_session(const std::filesystem::path& socket, const CliRequest& request);

// `ckmux attach <name|id>` — the id to go to, looked up before any window
// opens. Resolved out here rather than inside the running client so that "no
// such session" is a sentence on stderr and an exit status, rather than a
// dialog over a desktop the reader did not ask for.
//
// Returns 0 when there is no such session, having already said why. Does not
// start a server: attaching to a session that cannot exist yet is a typo, not
// a request for an empty desktop.
std::uint64_t resolve_attach_target(const std::filesystem::path& socket,
                                    const CliRequest& request);

// `ckmux check-config` — parses the configuration and reports, touching no
// socket at all (the configuration spec). Two halves, both of which
// already exist as return values: the warnings a load produced, and the keys
// that are read but not yet honoured by any landed package.
//
// Exit status is 1 when the file had warnings, so a script can gate on it; a
// key that is merely not-yet-honoured is not a defect in the file and does
// not fail it.
int run_check_config(const std::filesystem::path& config);

}  // namespace ckm::client
