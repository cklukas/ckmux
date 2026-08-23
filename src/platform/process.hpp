// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Becoming a detached server, starting one, and what a started program
// inherits (the architecture spec).
//
// The first two live here because they are two ends of one sequence: a client
// forks and execs `ckmux --server`, and that process detaches itself from
// everything it inherited. Splitting them across the client and the server
// would leave nobody owning the question of what happens in between — which is
// where a server that dies with the terminal that started it comes from. The
// third is here for the same reason: every fork in ckmux has to hand its child
// the same starting state, and a rule copied to each fork site is a rule that
// will one day differ between them.
#pragma once

#include <filesystem>
#include <string>

namespace ckm::platform {

// Detaches this process from the terminal and session that started it.
//
// `fork` → `setsid` → `fork` again → `chdir("/")` → stdio to `log`. The second
// fork is what makes it impossible to acquire a controlling terminal again: a
// session leader can, and a server that did would die with whoever's window it
// borrowed. `chdir("/")` so the server never holds a directory open — a
// detached process sitting in a mounted volume is a volume that cannot be
// ejected, which is a real thing readers hit and never diagnose.
//
// On success the calling process is the grandchild and both ancestors have
// exited, so "returned true" means "I am the server now".
//
// False is returned by whichever process found the failure, and that is not
// always the caller: the first fork fails in the caller, but `setsid`, the
// second fork and `chdir` fail in a child, by which time the process the
// starter was watching has already exited with status 0. So a false return
// means "say so and exit non-zero" — it does not mean the starter has been
// told, because there is nobody left for it to be told by. What it can never
// mean is "carry on": a process that got here is half-detached, and running a
// server in it would put the server back on the terminal it was leaving.
bool daemonize(const std::filesystem::path& log);

// Gives the child of a fork the signal state a program started from a shell is
// entitled to: every disposition back to SIG_DFL, and nothing blocked.
//
// Both are inherited across exec, and both are things ckmux sets for its own
// sake — the server ignores SIGPIPE so a client that vanishes mid-write cannot
// kill it. A shell that inherits an ignored SIGPIPE is a shell in which
// `yes | head -1` never ends: the writer is handed EPIPE forever instead of
// being killed, which is the mechanism the whole shape of Unix pipelines rests
// on. A child that inherits a blocked signal is a child whose ^C does nothing.
//
// Call between fork and exec and nowhere else: it is async-signal-safe, which
// is all that may run there, and it undoes dispositions this process still
// needs for itself.
void reset_signals_for_child() noexcept;

// Starts a server for `socket`, and does not wait for it to be ready.
//
// The child is reaped here — the intermediate process of the double fork exits
// immediately — so a client never accumulates zombies and never waits on the
// server itself. Readiness is a matter for the connect-and-retry loop, because
// "the process started" and "the socket is bound" are different facts and only
// the second one is worth waiting for.
bool start_server(const std::filesystem::path& executable, const std::filesystem::path& socket,
                  std::string& problem);

// This program's own path, for starting a server that is the same build. Falls
// back to `argv0` when the platform cannot say, which is why it is passed in.
std::filesystem::path executable_path(const char* argv0);

// Where a detached server's output goes. Beside the socket, so one directory
// holds everything about one server and a reader with a socket path can find
// the log without being told.
std::filesystem::path server_log_path(const std::filesystem::path& socket);

}  // namespace ckm::platform
