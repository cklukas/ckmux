// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Getting a client connected: connect, or start a server and connect
// (the architecture spec, WP-2).
//
// Lazy start is the behaviour a reader actually meets — they type `ckmux` and a
// server exists afterwards, whether or not one did before — and the whole of it
// is one loop with three answers: connected, somebody else is starting one, or
// this is not going to work. The retry is what makes the race between two
// clients uninteresting: the loser of the bind race simply keeps connecting
// until the winner is listening.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "common/proto.hpp"
#include "platform/socket.hpp"

namespace ckm::client {

struct ServerConnection {
    enum class Status {
        Connected,
        // No server, and starting one did not work — the executable could not
        // be found or run.
        CannotStart,
        // A server was starting, or is running, and never became reachable
        // within the budget — including a handshake that could not be written
        // out or answered in time. Distinct from CannotStart because the
        // remedies are different: this one is worth retrying or looking at a
        // log for, and `problem` names which half of it happened.
        TimedOut,
        // The socket is there and this user may not use it, or its directory is
        // not safe to use.
        Refused,
        // A server answered and will not talk to this client: the protocol
        // versions differ. `problem` carries what it said, which is written to
        // be shown to a reader as-is.
        VersionMismatch,
    };

    Status status = Status::TimedOut;
    platform::Stream stream;
    // The server's build string from `HelloAck`, for a log line that names both
    // ends when something later goes wrong.
    std::string server_build;
    std::string problem;

    bool ok() const noexcept { return status == Status::Connected; }
};

// Connects to the server at `socket`, starting one with `executable` if
// nothing is listening, and completes the handshake.
//
// `budget_ms` bounds the whole sequence — the architecture spec's "~2 s cap". A budget and
// not an attempt count: what is being waited for is a socket appearing, and how
// many times a loop gets round in two seconds is a fact about the machine.
ServerConnection connect_to_server(const std::filesystem::path& socket,
                                   const std::filesystem::path& executable,
                                   proto::ClientKind kind = proto::ClientKind::Ui,
                                   int budget_ms = 2000);

// Reads one message, waiting up to `budget_ms`. For the CLI paths, which are a
// request and its answer and nothing else; a UI client has its own loop and
// reads through that instead.
bool await_message(platform::Stream& stream, proto::FrameReader& reader, proto::Message& message,
                   int budget_ms = 2000);

}  // namespace ckm::client
