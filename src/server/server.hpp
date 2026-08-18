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
using SessionId = std::uint64_t;

// A session: a name, and the terminals that belong to it (plans/03).
//
// The terminals themselves stay in one collection — they are the server's, not
// a session's, and moving one between sessions is meant to be a pointer move
// (plans/12 WP-10) rather than a transplant. What a session holds is which ones
// are its own, in the order they were opened.
struct Session {
    SessionId id = 0;
    std::string name;
    std::vector<TerminalId> terminals;
    std::uint64_t focused = 0;
};

class Server {
public:
    struct Options {
        std::filesystem::path socket;
        Settings settings;
        // Where a terminal opens when nothing else says. Stated once, by
        // whoever starts the server, rather than read out of the ambient
        // environment every time a window is opened: a server is a process some
        // client happened to start, and the environment it inherited is not a
        // decision anybody made (plans/02 — a client's own environment is its
        // own). `run_server_process` fills it from the user's home directory.
        std::string working_directory = "/";
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
    // A session, and a terminal inside one. Public because a session is what a
    // terminal LIVES in — `terminals()` alone opens one that belongs to nobody,
    // which no client can attach to and no snapshot will ever carry — and
    // because a host that starts a server needs to be able to set up the state
    // ckmux exists for: programs already running before anyone attached.
    Session& create_session(std::string name = {});
    Terminal& open_terminal(SessionId session, const TerminalSpec& spec);
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
        SessionId session = 0;
        // The desktop this client declared, on attach and on every host resize,
        // and what one cell of it measures. The metric is what turns a
        // terminal's grid into the pixel fields its child asks about
        // (XTWINOPS 14/16), and it belongs to the client's display rather than
        // to any one terminal.
        ckv::Size desktop{0, 0};
        ckv::Size cell_pixels{0, 0};
        // Whether this client's OUTER terminal reported Sixel, from `Attach`
        // (WP-16). Consulted when this client opens a terminal, because the
        // child's advertisement should say what the reader can actually see.
        bool host_sixel = false;
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
    // Sessions, and who is watching them.
    Session* session_for(SessionId id);
    Session* session_of(const Client& client);
    // The name a session gets when nobody chose one: "session-N", where N is
    // one past the largest number already taken. A reader who names nothing
    // still gets names that sort and do not collide.
    std::string next_session_name() const;
    void send_session_list(Client& client);
    // Whoever is attached to this session, or nullptr. At most one (plans/03:
    // the latest client wins), which is why this can answer with one pointer.
    Client* client_attached_to(SessionId id);
    // Ending a session, without blocking the loop.
    //
    // Every terminal is ASKED to end — ckVision signals the process group,
    // because it is the only thing that knows one — and the server keeps
    // serving while they do. When the grace runs out, whatever is still alive
    // is either killed or left, which is the reader's choice and not this
    // code's. Checked on the tick, which is what makes it non-blocking.
    struct PendingKill {
        SessionId session = 0;
        std::int64_t deadline_nanos = 0;
        bool force = true;
        // The second deadline, and whether the signal that started it has gone
        // out. A child wedged writing to a full PTY cannot act on any signal
        // until somebody empties it, so the escalation is a signal and then a
        // wait on the tick — not a signal and a hope, and above all not
        // `close()`, which does its own waiting inside this loop.
        std::int64_t kill_deadline_nanos = 0;
        bool killed = false;
    };
    void begin_kill(Session& session, bool force, int grace_seconds);
    void advance_kills();
    // Ending one terminal, the same shape as ending a session: asked first,
    // watched on the tick, killed at the deadline only when the reader said
    // so. A second ask for the same terminal replaces the first one's terms
    // rather than stacking — the reader changed their mind, not their count.
    struct PendingClose {
        TerminalId term = 0;
        std::int64_t deadline_nanos = 0;
        bool force = true;
        // The same second deadline a pending kill keeps, for the same reason.
        std::int64_t kill_deadline_nanos = 0;
        bool killed = false;
    };
    void begin_close(TerminalId term, bool force, int grace_seconds);
    void advance_closes();
    // Tells the watchers about children that ended on their own — the ones
    // nobody asked to close. On the tick, once, per terminal: what happens next
    // is the reader's `[general] on-exit`, which either takes the window away
    // or keeps it with a banner over the last screen the program drew.
    void notice_exits();
    // Actually removes one terminal: the PTY, the diffs, the session row, and
    // the watcher's window. `exited` distinguishes a child that ended from a
    // terminal that merely left for another session — the client keeps a
    // banner only for the former. Ending a session's last terminal ends the
    // session too, when the reader's kill-empty-session says so.
    void remove_terminal(TerminalId term, bool exited);
    // The session whose terminal list holds this id, or nullptr.
    Session* session_holding(TerminalId term);
    // Reparents one terminal into another session — or into a fresh one, when
    // the request says so. The child never notices; the watchers do.
    void move_terminal(Client& client, const proto::MoveTerminal& request);
    void forget_session(SessionId id, proto::DetachReason reason, std::string text);
    // Everything a client needs to hold every terminal: the full grid, the
    // history, the cursor and the modes, per terminal. Also what heals a client
    // that fell too far behind, which is why it is not only the attach path.
    void send_snapshot(Client& client);



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
    std::vector<Session> sessions_;
    std::vector<PendingKill> kills_;
    std::vector<PendingClose> closes_;
    SessionId next_session_ = 1;
    // When accepting may be tried again. A descriptor table that has run out
    // does not refill by being asked: `accept()` fails, the listener stays
    // readable, `poll()` returns instantly, and the loop spins at 100 % CPU for
    // the life of the process. So the listener is not merely un-asked while
    // this is in the future, it is un-WATCHED — a listener that is still in the
    // poll set brings the loop straight back here, and the spin survives the
    // fix.
    std::int64_t accept_paused_until_nanos_ = 0;
    // Consecutive `poll()` failures. A wait that failed will fail the same way
    // again, and the empty set it returns is indistinguishable from a timeout —
    // which is the same spin by another route, with nothing in the log to say
    // so.
    int poll_failures_ = 0;
    // Whether this server has ever had a session. A server that has just
    // started has none and must not read that as "the work is done" — it is
    // waiting for the client that started it to say what it wants.
    bool a_session_has_existed_ = false;
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
