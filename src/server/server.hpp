// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The detached server: one socket, one thread, one `poll()` (WP-2,
// plans/02-architecture.md).
//
// What this file owns is the shape of the process — who may connect, what a
// connection has to say first, when the loop wakes, and how the server ends.
// What it deliberately does not own is what a client asks for: sessions
// (WP-8), attach and detach (WP-6), input routing (WP-5). Messages for those
// are answered with `Error{NotImplemented}` rather than ignored, because a
// client that gets no answer waits forever and a reader watching it has
// nothing to go on.
//
// No threads (plans/02). No UI: this links only ckVision's `core` and `term`,
// which is what lets a server run on a machine with no terminal at all.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/config.hpp"
#include "common/proto.hpp"
#include "platform/poller.hpp"
#include "platform/socket.hpp"
#include "server/diff_engine.hpp"
#include "server/terminals.hpp"
#include "cvision/core/clock.hpp"

namespace ckm::server {

// A connected client, never recycled within a server's lifetime for the same
// reason a terminal id is not.
using ClientId = std::uint64_t;

class Server {
public:
    struct Options {
        std::filesystem::path socket;
        Settings settings;
    };

    enum class StartStatus {
        Listening,
        // Somebody else is already listening on this socket. Not an error: it
        // is the ordinary end of a race between two clients that both found no
        // server and both tried to start one.
        AlreadyRunning,
        // Another starter holds the start lock this instant. Also not an error;
        // the caller should go back to connecting.
        Racing,
        Failed,
    };

    Server(Options options, ckv::Clock& clock);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    StartStatus start();
    const std::string& problem() const noexcept { return listener_.problem(); }

    // One pass of the loop: wait until something is ready or the flush tick is
    // due, then act on whichever it was. Returns false when the server should
    // stop — a `KillServer`, or a signal asking it to end.
    //
    // Public and separate from `run()` so a test can drive the loop one pass at
    // a time against a `ManualClock`, which is the only way to test a tick
    // without waiting for one.
    bool step();

    // Steps until stopped, then closes everything down.
    void run();

    // Ends the loop at the next opportunity. The one route in is a message or a
    // signal; nothing inside decides to stop on its own.
    void stop() noexcept { running_ = false; }

    Terminals& terminals() noexcept { return terminals_; }
    DiffEngine& diffs() noexcept { return diffs_; }
    std::size_t client_count() const noexcept { return clients_.size(); }
    // How many clients have completed the handshake. Distinct from the count
    // above, because a connection that has said nothing yet is a connection
    // that may still be refused.
    std::size_t greeted_count() const noexcept;
    // How much is queued for the attached client, and whether it has fallen far
    // enough behind to be waiting for a snapshot. The backpressure gauge: a
    // server can log it, WP-20's budget gates will assert on it, and a test can
    // watch a client cross the high-water mark for real rather than by
    // arithmetic.
    std::size_t queued_bytes() const noexcept;
    bool waiting_to_heal() const noexcept;

private:
    struct Client {
        ClientId id = 0;
        platform::Stream stream;
        proto::FrameReader reader;
        bool greeted = false;
        proto::ClientKind kind = proto::ClientKind::Ui;
        // Attached to the session, and therefore the one client being sent
        // deltas. At most one at a time (plans/03: takeover — the latest client
        // always wins), which is also why one belief per terminal is the whole
        // truth in the diff engine.
        bool attached = false;
        std::uint64_t session = 0;
        // The desktop this client declared, on attach and on every host resize.
        ckv::Size desktop{0, 0};
        // The client's queue went over its high-water mark, so deltas stopped
        // being queued for it. What clears it is not the missing deltas — those
        // are gone — but a fresh snapshot once the queue has drained: repairing
        // a mirror from deltas it never saw is not possible, and pretending
        // otherwise is how a screen ends up showing something no program drew
        // (plans/05, and WP-5's gap rule on the other end of the same idea).
        bool dirty_snapshot = false;
        bool closing = false;
    };

    void accept_pending();
    // Reads what has arrived and acts on every complete frame. A decode error
    // is fatal for the connection and only for the connection (plans/02): the
    // peer is speaking a language this server does not, and there is no way to
    // find the next frame boundary in a stream that may never have had one.
    void serve(Client& client);
    void handle(Client& client, const proto::Message& message);
    void send(Client& client, const proto::Message& message);
    void drop(Client& client, std::string_view why);
    void flush_tick();
    std::int64_t nanos_until_tick() const;

    // The attach path. `attach` grants immediately and takes the session from
    // whoever held it (plans/03), because a reader whose laptop slept cannot be
    // kept out by the client that is still nominally holding their session.
    void attach(Client& client, const proto::Attach& request);
    void detach(Client& client, proto::DetachReason reason, std::string text);
    // Everything a client needs to hold every terminal: the full grid, the
    // history, the cursor and the modes, per terminal. Also what heals a client
    // that fell too far behind, which is why it is not only the attach path.
    void send_snapshot(Client& client);

    Client* attached_client();

    Options options_;
    ckv::Clock& clock_;
    platform::Listener listener_;
    platform::Poller poller_;
    std::vector<std::unique_ptr<Client>> clients_;
    ClientId next_client_ = 1;
    Terminals terminals_;
    DiffEngine diffs_;
    std::int64_t next_tick_nanos_ = 0;
    // The one session this build has. WP-8 makes them plural, named, and
    // listable; until then every terminal belongs to this one, so that attach,
    // detach and takeover can be real before the thing they attach TO is.
    std::uint64_t session_ = 1;
    std::uint64_t focused_terminal_ = 0;
    bool running_ = true;
};

// Everything a `--server` invocation does: detach, listen, serve, and go away
// again. Separate from `main` so that the argument parsing and this are each
// readable on their own, and so a test can run the same sequence in a process
// it controls.
//
// `foreground` skips the detaching, which is what a test needs: a server that
// double-forks cannot be waited for, and a test that cannot wait for its
// subject has to sleep and hope.
int run_server_process(const std::filesystem::path& socket, bool foreground);

}  // namespace ckm::server
