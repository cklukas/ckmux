// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// Becoming a detached server, and starting one (plans/02-architecture.md).
//
// Both halves live here because they are two ends of one sequence: a client
// forks and execs `ckmux --server`, and that process detaches itself from
// everything it inherited. Splitting them across the client and the server
// would leave nobody owning the question of what happens in between — which is
// where a server that dies with the terminal that started it comes from.
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
// Returns false only in the parent's failure paths; on success the calling
// process is the grandchild and the two ancestors have exited. So a caller
// should treat "returned true" as "I am the server now".
bool daemonize(const std::filesystem::path& log);

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
