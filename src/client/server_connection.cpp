// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/server_connection.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <string>
#include <utility>
#include <variant>

#include "platform/process.hpp"
#include "platform/poller.hpp"

namespace ckm::client {
namespace {

using clock_type = std::chrono::steady_clock;

int millis_left(const clock_type::time_point& deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now());
    return static_cast<int>(left.count());
}

// Gets everything queued onto the socket, or says it could not within the
// budget.
//
// `Stream::send` is a queue over a non-blocking descriptor: it writes what the
// socket will take and keeps the rest. Nothing here reads the queue afterwards,
// so a handshake the kernel would not take whole stayed half-written while this
// client waited for the answer to it — and the message a reader was shown
// blamed the server for not answering a question it had never been asked.
bool flush_fully(platform::Stream& stream, const clock_type::time_point& deadline) {
    if (!stream.open()) return false;
    platform::Poller waiting;
    while (stream.wants_write()) {
        if (!stream.flush()) return false;  // the peer is gone
        if (!stream.wants_write()) break;
        const int remaining = millis_left(deadline);
        if (remaining <= 0) return false;
        waiting.clear();
        for (const auto source : stream.wait_sources())
            if (platform::has(source.interest, platform::Interest::Write))
                waiting.watch(source.handle, platform::Interest::Write);
        (void)waiting.wait(remaining);
        if (waiting.outcome() == platform::Poller::Outcome::Interrupted) continue;
        if (waiting.outcome() != platform::Poller::Outcome::Ready) return false;
    }
    return true;
}

}  // namespace

bool await_message(platform::Stream& stream, proto::FrameReader& reader, proto::Message& message,
                   int budget_ms) {
    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(budget_ms);
    platform::Poller waiting;
    for (;;) {
        const proto::DecodeError error = reader.next(message);
        if (error == proto::DecodeError::None) return true;
        // Anything but Incomplete is fatal: a stream whose framing is wrong
        // cannot be resynchronised, and guessing means acting on bytes of
        // unknown provenance.
        if (error != proto::DecodeError::Incomplete) return false;
        if (!stream.open()) return false;
        // A failed outgoing request does not erase a reply already received.
        // The same read-until-EOF contract applies to CLI and UI consumers.
        (void)stream.flush();
        const int remaining = millis_left(deadline);
        if (remaining <= 0) return false;
        waiting.clear();
        for (const auto source : stream.wait_sources()) waiting.watch(source);
        (void)waiting.wait(remaining);
        // A signal is not an answer. `SIGWINCH` arrives whenever the reader
        // resizes the window they started ckmux in, and interrupting the wait
        // used to end it — `ckmux` reported that the server never answered the
        // handshake, because somebody had dragged a window corner. The deadline
        // still bounds the loop, so retrying cannot spin.
        if (waiting.outcome() == platform::Poller::Outcome::Interrupted) continue;
        if (waiting.outcome() != platform::Poller::Outcome::Ready) return false;
        (void)stream.flush();
        std::string arrived;
        const bool alive = stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) return false;
        if (!alive && arrived.empty()) return false;
    }
}

ServerConnection connect_to_server(const std::filesystem::path& socket,
                                   const std::filesystem::path& executable, proto::ClientKind kind,
                                   int budget_ms) {
    ServerConnection result;
    if (!platform::socket_path_fits(socket)) {
        result.status = ServerConnection::Status::Refused;
        result.problem = "the socket path is too long to be a Unix socket address: " + socket.string();
        return result;
    }

    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(budget_ms);
    bool started = false;
    platform::ConnectResult connected;
    for (;;) {
        connected = platform::connect_to_server(socket);
        if (connected.status == platform::ConnectStatus::Connected) break;
        if (connected.status == platform::ConnectStatus::Denied) {
            result.status = ServerConnection::Status::Refused;
            result.problem = connected.problem;
            return result;
        }
        if (connected.status == platform::ConnectStatus::Unusable) {
            result.status = ServerConnection::Status::CannotStart;
            result.problem = connected.problem;
            return result;
        }
        // Nothing listening. Start one — once. A second start is never the
        // answer: either the first is coming up, or somebody else's is, and both
        // cases are cured by continuing to connect.
        if (!started && connected.status == platform::ConnectStatus::NoServer) {
            started = true;
            std::string problem;
            if (!platform::start_server(executable, socket, problem)) {
                result.status = ServerConnection::Status::CannotStart;
                result.problem = problem;
                return result;
            }
        }
        if (millis_left(deadline) <= 0) {
            result.status = ServerConnection::Status::TimedOut;
            result.problem = "no server appeared at " + socket.string() + " within " +
                             std::to_string(budget_ms) + " ms; see " +
                             platform::server_log_path(socket).string();
            return result;
        }
        // A short sleep rather than a spin: what is being waited for is another
        // process reaching its bind, and hammering connect() makes that slower
        // rather than faster.
        platform::Poller pause;
        (void)pause.wait(10);
    }

    result.stream = connected.take_stream();
    proto::Hello hello;
    hello.build = std::string(proto::kBuildIdentity);
    hello.client_kind = kind;
    (void)result.stream.send(proto::encode(hello));
    // Written out before anything waits for the answer to it, and a failure
    // named for what it is: a server cannot answer a handshake it was never
    // given, so "did not answer" would be this client blaming the other end for
    // its own unsent bytes.
    if (!flush_fully(result.stream, deadline)) {
        result.status = ServerConnection::Status::TimedOut;
        result.problem = "the handshake could not be sent to the server at " + socket.string();
        result.stream.close();
        return result;
    }

    proto::FrameReader reader;
    proto::Message answer;
    if (!await_message(result.stream, reader, answer, std::max(200, millis_left(deadline)))) {
        result.status = ServerConnection::Status::TimedOut;
        result.problem = "the server at " + socket.string() + " did not answer the handshake";
        result.stream.close();
        return result;
    }
    if (const auto* refuse = std::get_if<proto::Refuse>(&answer)) {
        // Written by the server to be shown as-is: it is the one message a
        // reader meets after an upgrade, and it carries the remedy.
        result.status = ServerConnection::Status::VersionMismatch;
        result.problem = refuse->reason;
        result.stream.close();
        return result;
    }
    const auto* ack = std::get_if<proto::HelloAck>(&answer);
    if (ack == nullptr) {
        result.status = ServerConnection::Status::TimedOut;
        result.problem = "the server answered the handshake with something else entirely";
        result.stream.close();
        return result;
    }
    result.status = ServerConnection::Status::Connected;
    result.server_build = ack->build;
    return result;
}

}  // namespace ckm::client
