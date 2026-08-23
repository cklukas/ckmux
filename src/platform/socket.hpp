// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The Unix socket the server listens on and clients connect to
// (the architecture spec, WP-2). One place owns the path, the permissions,
// the race between two starters, and the peer check — because every one of
// those is a decision that has to be the same on both ends of the socket, and
// two of them are security properties.
//
// No TCP, ever (the architecture spec: a v1 non-goal; remote means ssh). Nothing here
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
//
// The directory is created WITH mode 0700 rather than created and then
// corrected — between a `mkdir` under a permissive umask and a `chmod` there is
// a window in which anybody on the machine may walk in — and it is examined
// through a descriptor, so every check is about the directory the socket will
// actually be bound in. A symbolic link at the path is followed only when the
// link itself belongs to root or to this user — macOS's `/tmp` is root's link
// to `/private/tmp`, and a reader may point their own links where they like —
// and a link planted by another user is refused instead of being followed to
// a target whose ownership says what the checker wanted to hear.
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
// itself: the returned descriptor is non-blocking, and close-on-exec, so that
// no program a terminal window starts inherits the connection its own session
// is driven through.
//
// A signal arriving during the connect does not fail it. POSIX leaves an
// interrupted connect running and completes it asynchronously, so this waits
// for the descriptor to say how it went rather than calling `connect` again,
// which would answer EALREADY and be read as a failure.
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

    // What came of asking for a connection. The distinction is the point:
    // "none pending" is the ordinary answer several times a second, and a
    // descriptor table that has run out is a state a loop must stop asking
    // from — a caller that reads both as "-1, try again" polls a listener that
    // is permanently readable and spins at full speed for as long as the
    // process lives.
    enum class AcceptStatus {
        Accepted,
        // Nothing is waiting. Not an error, and not worth a word to anybody.
        Idle,
        // A signal, or a peer that hung up between connecting and being
        // accepted. Asking again immediately is right.
        Retry,
        // The connection was this server's to refuse: another user's.
        Refused,
        // Out of descriptors, or out of memory. Asking again immediately is
        // wrong — nothing will have changed, and the listener stays readable.
        Exhausted,
        // The listener is closed, or the system said something this build does
        // not classify.
        Failed,
    };

    struct AcceptResult {
        AcceptStatus status = AcceptStatus::Idle;
        int fd = -1;
        // In a reader's words, and empty when there is nothing a reader could
        // do about it (Idle and Retry).
        std::string problem;
        // `errno` as it stood, for a caller that logs or decides by it.
        int error = 0;
    };

    Status listen(const std::filesystem::path& path);

    int fd() const noexcept { return fd_; }
    const std::string& problem() const noexcept { return problem_; }
    const std::filesystem::path& path() const noexcept { return path_; }

    // Accepts one connection. Connections from another user are refused here
    // rather than anywhere later: this is the only place that can still say no
    // without having read a byte. The accepted descriptor is non-blocking and
    // close-on-exec before it is returned.
    AcceptResult accept_one();

    // The same, in the shape the server loop was written against: a descriptor
    // or -1, and a word for the reader only when there is one. Which of the
    // several reasons for -1 this was, the overload above says.
    int accept_one(std::string& refusal);

    // Stops listening and removes the socket file. Called by the destructor;
    // safe to call twice.
    //
    // The LOCK file stays where it is, and only the lock on it is released.
    // Removing it would be the same mistake the lock exists to prevent: a
    // starter that arrives while this one is shutting down has already opened
    // that file, so unlinking it leaves the two of them holding exclusive locks
    // on two different inodes, both convinced they won. A zero-byte file
    // beside the socket is the price of the race being settled at all.
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
// send a fresh snapshot when the queue drains — the protocol spec), and only the owner
// knows that.
class Stream {
public:
    // 4 MiB, the high-water mark of the protocol spec: past this the stream is in
    // trouble and nothing more should be queued at all.
    static constexpr std::size_t kHighWaterBytes = 4u * 1024u * 1024u;
    // 256 KiB, and a different question: how much SCREEN may be queued ahead of
    // a reader before sending more of it stops being worth doing.
    //
    // A delta describes a moment. One that is still waiting behind a queue when
    // the next tick comes round describes a moment that has already passed, and
    // queueing another behind it only adds to what the reader has to sit through
    // before seeing the present. Worse, everything else on the connection —
    // every answer to every question a client asks — waits behind it too:
    // measured against a flooding child, a `Ping` took **2.8 seconds** to come
    // back with only the 4 MiB mark in the way. A quarter of a megabyte is a
    // frame or two of a large screen; past it the answer is the one the protocol
    // already gives, which is a snapshot when the queue drains.
    static constexpr std::size_t kDeltaBacklogBytes = 256u * 1024u;
    // 32 MiB, and the last of the three questions. The backlog mark asks
    // whether more SCREEN is worth queueing; the high-water mark asks whether
    // the stream is in trouble; this one asks whether the peer is reading AT
    // ALL. Eight times the high-water mark and eight times a whole snapshot, so
    // an ordinary burst — a snapshot, a session list, and a tick of deltas
    // behind them — never comes near it, and only a connection nothing is being
    // read from does.
    //
    // Past it the memory is the server's and the terminals are not the client's:
    // the answer is to let the connection go, because a reconnect is a path the
    // protocol already defines (the protocol spec's universal recovery) and an unbounded
    // queue is not.
    static constexpr std::size_t kHardLimitBytes = 32u * 1024u * 1024u;

    Stream() = default;
    // Takes ownership of `fd`, and makes the no-SIGPIPE guarantee flush()
    // states true for it: on macOS that is a socket option, which every
    // descriptor this class is handed needs and only the ones this file made
    // would otherwise have.
    explicit Stream(int fd);
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
    //
    // False is also the answer when the write itself failed, which is to say
    // the peer is gone. Both cases mean the same thing to a caller — stop
    // adding — and the caller that needs to tell them apart has `flush()`,
    // which says so directly. What this must never do is discard the failure:
    // a send that answered "fine" for a connection that has died is a server
    // queueing a session's whole output into a socket nobody will ever read.
    bool send(std::string_view bytes);

    // Writes whatever the socket will take. Returns false when the peer is
    // gone.
    bool flush();

    // Reads what has arrived, appending to `into`, up to `byte_budget`. Returns
    // false at end-of-stream (the peer closed, which is how a detach looks from
    // here) or on a fatal error.
    //
    // The budget is not an optimisation. A peer that writes as fast as this
    // reads — a server carrying a child running `yes`, or a client pasting a
    // file — never lets the socket run dry, so a loop that read "until there is
    // nothing left" would never return, and the caller would never get back to
    // its own screen, its own keyboard, or its own timers. It is the same rule
    // the server applies to a PTY (WP-3) and for the same reason: bounded reads
    // are what fairness is made of.
    bool receive(std::string& into, std::size_t byte_budget = 1024 * 1024);

    std::size_t queued() const noexcept { return pending_.size() - sent_; }
    bool over_high_water() const noexcept { return queued() > kHighWaterBytes; }
    // Whether there is already more screen queued than is worth adding to.
    bool over_delta_backlog() const noexcept { return queued() > kDeltaBacklogBytes; }
    // Whether this peer has stopped reading rather than merely fallen behind.
    bool over_hard_limit() const noexcept { return queued() > kHardLimitBytes; }
    // Whether there is room to put a screen on this connection at all.
    //
    // One question, one answer, asked by the gate that decides a healing
    // snapshot may go and by the gate that stops sending deltas alike — because
    // a client that satisfies one and not the other oscillates between them,
    // and a queue that drains slower than a snapshot is then re-snapshotted
    // forever (M-S5).
    bool ready_for_screen() const noexcept { return !over_delta_backlog(); }
    bool wants_write() const noexcept { return queued() > 0; }

    void close() noexcept;

private:
    int fd_ = -1;
    std::string pending_;
    std::size_t sent_ = 0;
};

}  // namespace ckm::platform
