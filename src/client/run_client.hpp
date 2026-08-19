// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The client, attached to a server (WP-7, the M2 acceptance).
//
// This is where every earlier package meets: WP-2's lazy start, WP-5's mirror
// behind ckVision's seam, WP-6's attachment and healing, and M1's whole user
// interface — unchanged, because the only thing it was ever told about a
// terminal is that it is a `core::TerminalSubsession`.
//
// The loop is the one plans/02 specifies and ckVision sanctions (D-021): poll
// the application's own sources together with the server socket, deadline-capped
// by the application's next timer, then step. No threads: what would they do —
// a socket and a keyboard are both descriptors, and one loop that waits on both
// is simpler to reason about than two that have to agree.
#pragma once

#include <filesystem>
#include <string>

#include "client/client_app.hpp"

namespace ckv {
class Clock;
namespace term {
class Terminal;
}
}  // namespace ckv

namespace ckm::client {

struct RunOptions {
    // Where the server is, and what to start if none is listening.
    std::filesystem::path socket;
    std::filesystem::path executable;
    // Everything the client already needed. `terminal_source` is filled in
    // here — that is the whole substitution — and anything else the host set is
    // left alone.
    ClientOptions client;
    // `ckmux attach <name>`: the session to go to at startup, already resolved
    // to an id by the CLI (client/cli.hpp) so that a name naming nothing is a
    // refusal at the shell rather than a dialog over a desktop. Zero is the
    // ordinary launch, where the picker or the sole session decides (WP-11,
    // plans/06:401 — the CLI bypasses the picker for scripting).
    std::uint64_t preselected_session = 0;
};

// Runs until the reader quits or the server goes away. Returns a process exit
// status: 0 for an ordinary quit or detach, non-zero when the server could not
// be reached at all, with the reason already on stderr.
int run_attached_client(ckv::term::Terminal& terminal, ckv::Clock& clock, RunOptions options);

}  // namespace ckm::client
