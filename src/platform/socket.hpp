// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The Unix socket the server listens on and clients connect to
// (plans/02-architecture.md, WP-2). One place owns the path, the permissions,
// the race between two starters, and the peer check — because every one of
// those is a decision that has to be the same on both ends of the socket, and
// two of them are security properties.
//
// No TCP, ever (plans/02: a v1 non-goal; remote means ssh). Nothing here
// knows what the bytes mean; framing and messages are common/proto.hpp.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace ckm::platform {

// Where the server listens.
//
//   1. `$CKMUX_SOCKET`, used exactly as given — the explicit override, and
//      what a test or a parallel development instance sets.
//   2. `<runtime>/ckmux-<uid>/default.sock`, where `<runtime>` is
//      `$XDG_RUNTIME_DIR`, else `$TMPDIR`, else `/tmp`.
//
// The uid is in the directory name so that two people on one machine cannot
// be handed each other's socket by a shared `/tmp`, and the directory is the
// thing that carries the 0700 that makes that true.
std::filesystem::path socket_path();

// Whether a path fits in a `sockaddr_un`. macOS allows 104 bytes including the
// terminator and Linux 108, and the failure mode when it does not fit is a
// silently truncated path — a server listening somewhere else entirely. Every
// bind and connect checks first, and a caller that builds a path from an
// environment variable should check before it gets that far.
bool socket_path_fits(const std::filesystem::path& path);

// Creates the directory a socket lives in, owner-only. Returns false if it
// exists and is not a directory owned by this user with no access for anyone
// else: a socket in a directory somebody else can write to is a socket
// somebody else can replace.
bool prepare_socket_directory(const std::filesystem::path& socket, std::string& problem);

enum class ConnectStatus {
    Connected,
    // Nothing is listening: no such file, or a socket file left behind by a
    // server that died. Both mean "start one" to a client, which is why they
    // are one answer rather than two.
    NoServer,
    // The path exists and is listening, but this user may not use it, or the
    // directory it sits in is not safe.
    Denied,
    // The path cannot be a socket address, or something else went wrong.
    Unusable,
};

struct ConnectResult {
    ConnectStatus status = ConnectStatus::Unusable;
    int fd = -1;
    std::string problem;  // in a reader's words, for the Denied/Unusable cases
};

// Connects to a listening server. Never blocks for longer than the connect
// itself: the returned descriptor is non-blocking.
ConnectResult connect_to_server(const std::filesystem::path& path);

// The listening end, and the lock that makes starting one race-free.
//
// Binding is not itself a lock, which is the trap this class exists to avoid:
// a stale socket file has to be unlinked before a bind can succeed, so two
// starters that each unlink and bind both succeed — and the one that went
// first is left listening on a socket file nobody will ever connect to again.
// So the sequence is: take an exclusive lock on a sibling lock file; while
// holding it, try to connect (a live server means this starter simply lost and
// should connect instead); only then unlink and bind. The lock is held for as
// long as the server lives, so a starter that arrives later always loses it
// while there is a server to connect to.
class Listener {
public:
    enum class Status {
        Listening,
        // Another process holds the start lock right now. It is about to
        // finish binding, so a client should retry connecting.
        Racing,
        // A server is already listening on this path.
        AlreadyRunning,
        Failed,
    };

    Listener() = default;
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    Status listen(const std::filesystem::path& path);

    int fd() const noexcept { return fd_; }
    const std::string& problem() const noexcept { return problem_; }
    const std::filesystem::path& path() const noexcept { return path_; }

    // Accepts one connection, or -1 when there is none pending. Connections
    // from another user are refused here rather than anywhere later: this is
    // the only place that can still say no without having read a byte.
    int accept_one(std::string& refusal);

    // Stops listening and removes the socket file and the lock. Called by the
    // destructor; safe to call twice.
    void close() noexcept;

private:
    int fd_ = -1;
    int lock_fd_ = -1;
    std::filesystem::path path_;
    std::string problem_;
};

// One connection's byte streams, with the queue that keeps a server from ever
// blocking on a client.
//
// A wedged client must not be able to stall the loop that reads PTYs — if it
// could, one client hitting ^Z in the wrong place would freeze every terminal
// on the machine for everybody. So writes go into a queue, the queue has a
// ceiling, and going over the ceiling is a fact the owner is told about rather
// than an error: the answer to it is protocol-level (stop queueing deltas,
// send a fresh snapshot when the queue drains — plans/05), and only the owner
// knows that.
class Stream {
public:
    // 4 MiB, the high-water mark of plans/05.
    static constexpr std::size_t kHighWaterBytes = 4u * 1024u * 1024u;

    Stream() = default;
    explicit Stream(int fd) : fd_(fd) {}
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    Stream(Stream&& other) noexcept;
    Stream& operator=(Stream&& other) noexcept;

    int fd() const noexcept { return fd_; }
    bool open() const noexcept { return fd_ >= 0; }

    // Queues bytes and writes what it can immediately. Returns false once the
    // queue is over its high-water mark — the bytes are still queued, because
    // dropping half a frame would desynchronise the stream; what the caller
    // must stop doing is adding more.
    bool send(std::string_view bytes);

    // Writes whatever the socket will take. Returns false when the peer is
    // gone.
    bool flush();

    // Reads whatever has arrived, appending to `into`. Returns false at
    // end-of-stream (the peer closed, which is how a detach looks from here)
    // or on a fatal error.
    bool receive(std::string& into);

    std::size_t queued() const noexcept { return pending_.size() - sent_; }
    bool over_high_water() const noexcept { return queued() > kHighWaterBytes; }
    bool wants_write() const noexcept { return queued() > 0; }

    void close() noexcept;

private:
    int fd_ = -1;
    std::string pending_;
    std::size_t sent_ = 0;
};

}  // namespace ckm::platform
