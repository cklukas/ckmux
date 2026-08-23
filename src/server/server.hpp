// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The detached server: one socket, one thread, one `poll()` (WP-2,
// The architecture spec).
//
// What this file owns is the shape of the process — who may connect, what a
// connection has to say first, when the loop wakes, and how the server ends.
// What it deliberately does not own is what a client asks for: sessions
// (WP-8), attach and detach (WP-6), input routing (WP-5). Messages for those
// are answered with `Error{NotImplemented}` rather than ignored, because a
// client that gets no answer waits forever and a reader watching it has
// nothing to go on.
//
// No threads (the architecture spec). No UI: this links only ckVision's `core` and `term`,
// which is what lets a server run on a machine with no terminal at all.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <deque>
#include <unordered_map>
#include <vector>

#include "common/config.hpp"
#include "common/proto.hpp"
#include "platform/poller.hpp"
#include "platform/process_stats.hpp"
#include "platform/socket.hpp"
#include "server/diff_engine.hpp"
#include "server/terminals.hpp"
#include "cvision/core/clock.hpp"

namespace ckm::server {

// A connected client, never recycled within a server's lifetime for the same
// reason a terminal id is not.
using ClientId = std::uint64_t;
using SessionId = std::uint64_t;

// A session: a name, and the terminals that belong to it (the session model).
//
// The terminals themselves stay in one collection — they are the server's, not
// a session's, and moving one between sessions is meant to be a pointer move
// (the work queue WP-10) rather than a transplant. What a session holds is which ones
// are its own, in the order they were opened.
//
// The session model also gives a session a `layout` field, and it is this list read
// through `Terminal::layout()` plus the `focused` id below rather than a member
// here: a window's place is a fact about one terminal (the session model's Terminal
// `geometry` row), and a second list keyed by id would have to be pruned
// wherever a terminal leaves a session — which is where a stale row would
// survive to state a window for a terminal nobody has any more.
struct Session {
    SessionId id = 0;
    std::string name;
    std::vector<TerminalId> terminals;
    // Where a reader STARTS, not where any reader is. Written by whichever
    // client last focused something, read only when a client attaches with no
    // focus of its own — which is exactly what `Snapshot::focused_term` has
    // always meant on the wire: what a newcomer is told, once. Until a second
    // client existed the two readings were indistinguishable and the name was
    // the lie; focus itself is per client, below, because two readers being in
    // one terminal is nonsense (WP-41).
    std::uint64_t last_focused = 0;
    // The session's VIRTUAL DESKTOP: the coordinate space every window rect in
    // this session is expressed in (the session model, WP-40). Not any client's screen
    // — a client that differs from it has a viewing problem, and reflowing the
    // windows to fit one reader SIGWINCHes every child for every reader.
    //
    // Zero until the first client attaches, because a session made by `ckmux
    // new` has no screen to take a size from; what happens then is
    // `[general] desktop-size`, and under the default it is set once, by the
    // first client to arrive, and left alone afterwards.
    ckv::Size desktop{0, 0};
    // What this session says about printing, for the terminals in it that do
    // not say otherwise themselves.
    PrinterOverride printer;
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
        // decision anybody made (the architecture spec — a client's own environment is its
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
    // The printer policy in force for one terminal, and where each of its
    // three numbers came from: terminal → session → global → built-in
    // (the interface spec, the configuration spec). Public because it is what `Printer Settings` shows
    // and what a test asserts on; there is no second copy of this rule.
    EffectivePrinterPolicy effective_printer_policy(TerminalId id) const;
    bool waiting_to_heal() const noexcept;
    // The pixels held for clients that have not caught up yet. The other half
    // of the same gauge: `queued_bytes` measures what is on the wire, which is
    // bounded by the stream's own marks, while this measures what is waiting
    // to go ON it — the quantity that used to grow without bound under a child
    // that draws continuously, and the one a test can watch stay flat.
    std::size_t owed_image_bytes() const noexcept;
    // And how many ops those pixels are waiting behind. The companion gauge,
    // because the ops that carry no pixels are superseded too — a picture
    // dragged across a desktop moves without a byte of it being redrawn, and a
    // debt that kept every move would replay the drag rather than state where
    // the picture ended up.
    std::size_t owed_image_ops() const noexcept;
    // How many stats passes have run (WP-38). The gauge behind "nobody
    // watching, nobody sampling": a test asserts this stays at zero across
    // any number of ticks with no subscriber, which is the only way to prove
    // absence of work rather than merely absence of messages.
    std::size_t stats_passes() const noexcept { return stats_passes_; }

private:
    struct Client {
        ClientId id = 0;
        platform::Stream stream;
        proto::FrameReader reader;
        bool greeted = false;
        proto::ClientKind kind = proto::ClientKind::Ui;
        // A CLI client's Hello named a protocol version this server does not
        // speak, so its Refuse carries the words "end the running server with
        // `ckmux kill-server`" — but killing a mismatched server needs a
        // connection to still be open to receive that request on. `KillServer`
        // is the one message with nothing to agree on: no payload, and its
        // encoding is the eight-byte frame header alone, which has not changed
        // and has no reason to. Bounded rather than held open indefinitely — a
        // connection that says anything else, or nothing, in the meantime is
        // dropped exactly as an ordinary mismatch would be.
        bool awaiting_kill_from_mismatched_cli = false;
        std::int64_t mismatch_kill_deadline_nanos = 0;
        // Attached to the session, and therefore the one client being sent
        // deltas. At most one at a time (the session model: takeover — the latest client
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
        // Which terminal THIS reader is in. Per client and never broadcast: a
        // `LayoutDelta` carrying somebody else's focus would move this
        // reader's cursor because another one clicked, which is the marks
        // defect one field over (WP-41).
        std::uint64_t focused = 0;
        // The client's queue went over its high-water mark, so deltas stopped
        // being queued for it. What clears it is not the missing deltas — those
        // are gone — but a fresh snapshot once the queue has drained: repairing
        // a mirror from deltas it never saw is not possible, and pretending
        // otherwise is how a screen ends up showing something no program drew
        // (the protocol spec, and WP-5's gap rule on the other end of the same idea).
        bool dirty_snapshot = false;
        // The picture payloads this client is still owed, dripped out per tick
        // as its queue has room instead of queued whole behind a snapshot.
        // Inlining them was a loop under load: the pictures of one busy
        // session are tens of megabytes at HiDPI cell sizes, a heal that
        // queues them all crosses the backlog mark by itself, any news during
        // the drain marks the client dirty again, and the next heal re-sends
        // everything — the reader saw their pictures blink in for a moment
        // per lap and gray otherwise (field report, 2026-08-19, ckgrapher).
        // The mirror keeps showing its held pixels meanwhile (TerminalState::
        // images), so a drip that takes ticks is invisible where a loop was
        // unmissable. Superseded whole by the next snapshot; cleared on
        // detach.
        //
        // Each op carries the wire id it is about and whether it OPENS a
        // payload group, because those two are what let a frame the child has
        // already replaced be dropped before it is ever sent (`owe_image`).
        // Without that, a child that animates — ckvision_spin redrawing its
        // cube at 16fps — made every frame a permanent entry here the moment
        // a debt existed: the debt only ever grew, the reader watched minutes
        // of stale frames play back after the child had stopped, and the
        // server held hundreds of megabytes in order to replay them (field
        // report, 2026-08-19).
        struct OwedImage {
            proto::Message message;
            std::uint64_t image = 0;
            bool begins = false;
        };
        std::vector<OwedImage> owed_images;
        // How much of `owed_images` has been sent. An index rather than a
        // deque so supersession is one assignment.
        std::size_t owed_sent = 0;
        // A heal was sent and its bytes have not yet drained under the
        // backlog mark. While this is true, news does NOT re-mark the client
        // dirty: the queue is over the mark because of the heal itself, and
        // re-arming on that is the whole loop — a snapshot larger than the
        // mark plus a blinking cursor healed once per drain, forever. Deltas
        // keep flowing meanwhile (the sequence restarted at the snapshot, so
        // they are its valid continuation), and a reader who has genuinely
        // stopped is the hard limit's business, as it always was.
        bool draining_heal = false;
        bool closing = false;
        // This connection asked for per-terminal process stats (`WatchStats`,
        // WP-38). Per connection, dies with it, and never session state: the
        // subscription is a fact about a reader's View menu, not about what
        // is running.
        bool watch_stats = false;
    };

    void accept_pending();
    // Reads what has arrived and acts on every complete frame. A decode error
    // is fatal for the connection and only for the connection (the architecture spec): the
    // peer is speaking a language this server does not, and there is no way to
    // find the next frame boundary in a stream that may never have had one.
    void serve(Client& client);
    void handle(Client& client, const proto::Message& message);
    void send(Client& client, const proto::Message& message);
    // Queues one picture op behind a debt, dropping what it makes untrue.
    // Every route into `Client::owed_images` goes through here, so that the
    // debt holds the CURRENT picture rather than a history of frames.
    void owe_image(Client& client, proto::Message op);
    // Whether any client could take a fresh payload for this picture now — the
    // question the differ asks before it builds one. False means every client
    // that would be sent it already owes an unsent payload for the same wire
    // id, or there is nobody to send it to at all.
    // Not const, only because the session lookup it needs is not: it reads
    // and decides nothing.
    bool picture_wanted_now(TerminalId term, std::uint64_t wire_id);
    void drop(Client& client, std::string_view why);
    void flush_tick();
    std::int64_t nanos_until_tick() const;

    // The stats sampler (WP-38): one pass a second, and only while somebody
    // asked. Whether anybody is watching is derived, never stored — a
    // subscription ends by message, by detach, or by the socket going, and a
    // stored flag would have to be cleared at every one of those doors.
    bool anyone_watching_stats() const noexcept;
    std::int64_t nanos_until_stats() const;
    // One process-table snapshot, then per watched terminal: resolve the
    // tree, sum it, derive the CPU rate against the previous pass, and tell
    // the one client watching that session. A terminal whose child is gone is
    // announced dead once, so the watcher clears the readout rather than
    // freezing it.
    void stats_tick();

    // The attach path. `attach` grants immediately and takes the session from
    // whoever held it (the session model), because a reader whose laptop slept cannot be
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
    // The session already called `name`, ignoring the one with `except_id` —
    // which is what lets a rename to a session's OWN name be a no-op rather
    // than a refusal. Names are compared exactly: a reader who wants `Build`
    // and `build` to be different sessions is entitled to them, and case
    // folding is a locale question this has no business answering.
    const Session* session_named(std::string_view name, SessionId except_id = 0) const;
    void send_session_list(Client& client);
    // Whoever is attached to this session, or nullptr. At most one (the session model:
    // the latest client wins), which is why this can answer with one pointer.
    // The first client attached to a session, or nullptr. Kept for the one
    // question that is genuinely singular — "is anybody watching?" — and NOT
    // for broadcasts: with simultaneous attach (WP-44) a session may have
    // several readers, and a broadcast that reached the first of them would
    // leave the others looking at a screen that stopped changing.
    Client* client_attached_to(SessionId id);
    // Every client attached to a session, in connection order. The shape every
    // broadcast site uses, so that "one reader" is nowhere assumed by
    // construction.
    template <typename Fn>
    void for_each_attached(SessionId id, Fn&& fn) {
        for (const std::unique_ptr<Client>& client : clients_)
            if (client->attached && !client->closing && client->session == id) fn(*client);
    }
    std::size_t attached_count(SessionId id);
    // The connection with this id, or nullptr once it has gone. Used by the
    // paste slot below, which remembers a client across the time its chunk
    // spends waiting for another reader to finish.
    Client* client_with_id(ClientId id);

    // --- One paste at a time, per terminal (WP-42) --------------------
    //
    // `PasteChunk` is routed by terminal id and asks nothing about who is
    // attached — exactly as `Input` does — so two connected clients can paste
    // into one terminal, and without this they interleave chunk by chunk and
    // the child is given text neither reader typed. It does not take
    // multi-attach to reach: a client that was taken over still holds its
    // socket, and a CLI client never attaches at all.
    //
    // So a terminal has a slot. The first chunk of a paste takes it, the
    // final chunk releases it, and a chunk from anyone else waits — UNACKED,
    // which is what makes the waiting bounded: WP-18's credit stops each
    // client at two chunks on the wire, so a terminal can hold at most two
    // per waiting reader however long the paste in front of them is.
    struct PendingPaste {
        ClientId client = 0;
        proto::PasteChunk chunk;
    };
    struct PasteSlot {
        ClientId owner = 0;  // 0 while nobody is mid-paste
        std::deque<PendingPaste> waiting;
    };
    // Writes one chunk, acks it, and takes or releases the slot accordingly.
    void write_paste_chunk(Terminal& terminal, Client& client, const proto::PasteChunk& chunk);
    // Hands a free slot to whoever has been waiting longest, until somebody
    // takes it or nobody is left.
    void drain_paste_slot(TerminalId term);
    // A connection has gone: it cannot finish a paste it was holding, and
    // nothing it queued will ever be wanted. Without this a reader whose
    // laptop slept mid-paste would wedge that terminal for everybody else.
    void release_paste_slots(ClientId client);
    // A terminal has gone: everything waiting for it is acked rather than
    // dropped, because the credit is about a CLIENT'S queue and a client that
    // never hears back holds the rest of that paste for ever (WP-18).
    void forget_paste_slot(TerminalId term);
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
    // The session model's `kill-terminal`: SIGKILL the process group and drop the
    // terminal, with none of `begin_close`'s courtesy — no SIGHUP, no SIGTERM,
    // no grace. A reader reaches this when asking nicely has already failed,
    // so asking nicely again is the one thing it must not do.
    void begin_kill_terminal(TerminalId term);
    void advance_closes();
    // Tells the watchers about children that ended on their own — the ones
    // nobody asked to close. On the tick, once, per terminal: what happens next
    // is the reader's `[general] on-exit`, which either takes the window away
    // or keeps it with a banner over the last screen the program drew.
    void notice_exits();
    // Everything about a terminal that is NOT its grid, said once per tick to
    // the one client watching its session: the bell and activity marks, a
    // child's OSC 52 write, the printer going on or off, the newest complaint
    // the emulator had to make.
    //
    // Read before `diffs_.flush`, which clears the damage all four are gated
    // on, and after any healing snapshot, because a snapshot carries the
    // clipboard watermark and deliberately not the text — a write announced
    // before it would be adopted away by it. They are messages rather than ops
    // for the reason the protocol spec splits the two: an op describes the GRID, and
    // none of these is the grid — a clipboard write is not something a reader
    // can see on a screen at all.
    void announce_terminal_news();
    // The session's window arrangement, once per tick, to the one client
    // watching it — the same routing the news above uses, and the same edge
    // trigger: a layout that did not move costs no message.
    //
    // A session at a time rather than a terminal at a time, because that is what
    // the message is: a `z_order` is a position among windows, so stating one
    // window's without the others' says nothing a mirror can apply. One report
    // that moves three windows is therefore one `LayoutDelta` and not three.
    // Normally edge-triggered: a session is announced only when one of its
    // terminals reports having moved. `force_session` names a session to
    // announce regardless, for the changes that are not a window moving —
    // the DESKTOP itself resizing. Without it, a desktop change in a session
    // whose windows all stayed put told nobody, and a session with no
    // terminals at all could never announce anything: the coordinate space
    // moved under every rect and no client heard.
    void announce_layout(std::uint64_t force_session = 0);
    // Actually removes one terminal: the PTY, the diffs, the session row, and
    // the watcher's window. `exited` distinguishes a child that ended from a
    // terminal that merely left for another session — the client keeps a
    // banner only for the former. Ending a session's last terminal ends the
    // session too, when the reader's kill-empty-session says so.
    void remove_terminal(TerminalId term, bool exited);
    // The session whose terminal list holds this id, or nullptr.
    // Takes whatever the emulator has finished, adds it to the terminal's
    // spool, and names each new job to the watching client. Drains even with
    // nobody watching — see the call site.
    void collect_and_announce_print_jobs(TerminalId id, Terminal& terminal, Client* watcher);
    // Pushes the resolved policy down into every terminal's emulator, and
    // sizes its spool. Called whenever a scope changes, because a policy the
    // emulator has not been told is a policy that is not in force.
    void apply_printer_policies();
    Session* session_holding(TerminalId term);
    // The const half, for the readers that only ask. Same lookup; a second
    // implementation would be a second answer waiting to disagree.
    const Session* session_holding(TerminalId term) const;
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
    std::unordered_map<TerminalId, PasteSlot> paste_slots_;
    Terminals terminals_;
    DiffEngine diffs_;
    std::int64_t next_tick_nanos_ = 0;
    // The stats sampler's own cadence, beside the flush tick rather than on
    // it: the flush tick runs at max-fps and sampling thirty times a second
    // would be work nobody asked for. The baseline is what a rate is derived
    // against — cumulative CPU at the previous pass, and when that pass was.
    struct StatsBaseline {
        std::uint64_t cpu_nanos = 0;
        std::int64_t at_nanos = 0;
        // A flag rather than "at_nanos > 0": a ManualClock starts at zero, so
        // a baseline taken at t=0 is real and a sentinel would read it as
        // absent — which showed up as a spinner reporting 0‰ forever in the
        // one test whose clock had never been advanced before subscribing.
        bool primed = false;
        bool dead_announced = false;
    };
    std::unordered_map<TerminalId, StatsBaseline> stats_baselines_;
    std::int64_t next_stats_nanos_ = 0;
    // Whether the sampler was running on the previous pass. The transition
    // from nobody-watching to somebody-watching is where stale baselines are
    // dropped — a rate derived against a sample from before the sampler went
    // idle would average over the whole quiet stretch.
    bool stats_were_running_ = false;
    std::size_t stats_passes_ = 0;
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
