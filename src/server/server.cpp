// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "server/server.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>
#include <variant>

#include <csignal>

#include <fcntl.h>
#include <unistd.h>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "cvision/term/posix_clock.hpp"

namespace ckm::server {
namespace {

// How much of a client's picture debt may be queued per tick (flush_tick's
// drip). Big enough that a debt retires in a handful of ticks on a local
// socket; small enough that the deltas queued behind one tick's worth stay
// prompt. Twice this is the queue depth past which a client that owes
// pictures is judged to have genuinely stopped reading rather than merely
// draining the drip.
constexpr std::size_t kOwedImageBurstBytes = 1024u * 1024u;

// The wire id a picture op is about. Every one of the five carries one, and
// the debt has to be able to ask — a frame is superseded by id, not by
// position, and the ops of two pictures interleave in the same queue.
std::uint64_t image_op_subject(const proto::Message& op) {
    if (const auto* begin = std::get_if<proto::ImageAddBegin>(&op)) return begin->id;
    if (const auto* chunk = std::get_if<proto::ImageChunk>(&op)) return chunk->id;
    if (const auto* end = std::get_if<proto::ImageEnd>(&op)) return end->id;
    if (const auto* place = std::get_if<proto::ImagePlace>(&op)) return place->id;
    if (const auto* remove = std::get_if<proto::ImageRemove>(&op)) return remove->id;
    return 0;
}

// Signals, delivered to the loop rather than acted on inside a handler.
//
// A handler may do almost nothing safely, and a server that shut itself down
// from inside one would be closing sockets and killing children on a stack it
// does not own. So a handler writes one byte down a pipe and returns; the byte
// makes `poll()` return, and the loop — which owns everything — decides.
class SignalPipe {
public:
    static SignalPipe& instance() {
        static SignalPipe pipe;
        return pipe;
    }

    bool open() {
        if (fds_[0] >= 0) return true;
        if (::pipe(fds_) != 0) return false;
        for (const int fd : fds_) {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        return true;
    }

    int reader() const noexcept { return fds_[0]; }

    void raise(int signal) noexcept {
        last_ = signal;
        const char byte = 1;
        // The result is deliberately unused: a full pipe already means the loop
        // has a wake-up coming, which is all this needs to achieve.
        (void)!::write(fds_[1], &byte, 1);
    }

    // Drains the pipe and returns the last signal seen, or 0.
    int take() noexcept {
        char scrap[64];
        while (::read(fds_[0], scrap, sizeof scrap) > 0) {
        }
        const int signal = last_;
        last_ = 0;
        return signal;
    }

private:
    SignalPipe() = default;
    int fds_[2] = {-1, -1};
    volatile std::sig_atomic_t last_ = 0;
};

extern "C" void ckmux_signal_handler(int signal) { SignalPipe::instance().raise(signal); }

void install_signal_handlers() {
    // A client that goes away mid-write must not take the server with it, which
    // is what SIGPIPE's default action does.
    (void)std::signal(SIGPIPE, SIG_IGN);
    (void)std::signal(SIGTERM, ckmux_signal_handler);
    (void)std::signal(SIGINT, ckmux_signal_handler);
    (void)std::signal(SIGHUP, ckmux_signal_handler);
}

// One cell, from the text area a client measured and the grid it measured over.
// The protocol carries the area because that is what a client can measure;
// everything downstream wants the cell (WP-3).
template <typename WithGeometry>
ckv::Size cell_metric_of(const WithGeometry& message) {
    if (message.columns == 0 || message.rows == 0) return ckv::Size{0, 0};
    return ckv::Size{message.pixel_width / message.columns, message.pixel_height / message.rows};
}

// The reader's `[printer] mode`, in the wire's own enumeration. One place, so
// the two spellings of the same three answers cannot drift apart.
proto::PrinterMode wire_printer_mode(PrinterMode mode) {
    switch (mode) {
        case PrinterMode::Ask: return proto::PrinterMode::Ask;
        case PrinterMode::Capture: return proto::PrinterMode::Capture;
        case PrinterMode::Off: return proto::PrinterMode::Off;
    }
    return proto::PrinterMode::Ask;
}

// And back. Two functions rather than a cast for the reason the wire's Rect is
// not ckVision's: the two enums are separate on purpose, and a cast would make
// one silently follow a renumbering of the other.
PrinterMode wire_printer_mode_in(proto::PrinterMode mode) {
    switch (mode) {
        case proto::PrinterMode::Ask: return PrinterMode::Ask;
        case proto::PrinterMode::Capture: return PrinterMode::Capture;
        case proto::PrinterMode::Off: return PrinterMode::Off;
    }
    return PrinterMode::Ask;
}

// Every message type, by name, for the `context` field of an `Error` — whose
// entire job is to say WHICH request failed.
//
// Written without a `default:` arm on purpose, and that is the whole fix. This
// function used to name eleven types and answer the literal string "a message"
// for the other forty-two, so a reader refused a `KillTerminal` was told that
// "a message" is not implemented — a sentence that names nothing and cannot be
// searched for. The `default:` is what let it drift: forty-two message types
// were added over the project's life and not one of them made anything fail.
//
// With every enumerator listed and no catch-all, `-Wswitch` under `-Werror`
// breaks the build the day somebody adds a message and forgets this table. A
// guard that cannot go stale beats a guard that was correct once.
// Whether a message from a client would CHANGE the session it is attached to,
// as against asking it something or telling the server about this reader alone
// (WP-49, the session model's two tables).
//
// Exhaustive on purpose, with no `default:` — the same discipline `name_of`
// below uses and for a stronger reason. A missing name is a log line that says
// "0x0211"; a message that silently defaults to "harmless" is a watcher who can
// do the one thing the mode exists to stop. `-Wswitch` is the guard, and it
// cannot go stale.
//
// Server→client messages appear here too, because the type is one enum. They
// answer false: a client that sends one is talking nonsense, and the ordinary
// not-implemented refusal at the bottom of `handle` is a better answer than a
// read-only one, which would tell a reader to stop watching in order to send a
// message no client may send in any mode.
bool changes_the_session(proto::MessageType type) {
    switch (type) {
        // Typing, pasting, and everything that ends, makes, moves or renames
        // what the other readers are looking at.
        case proto::MessageType::Input:
        case proto::MessageType::PasteChunk:
        case proto::MessageType::NewTerminal:
        case proto::MessageType::CloseTerminal:
        case proto::MessageType::KillTerminal:
        case proto::MessageType::RespawnTerminal:
        case proto::MessageType::MoveTerminal:
        case proto::MessageType::RenameTerminal:
        case proto::MessageType::RenameSession:
        case proto::MessageType::KillSession:
        case proto::MessageType::NewSession:
        // The window arrangement is session state, so a watcher rearranging it
        // moves the other reader's windows (the session model). Their own drags are not
        // blocked at the client — nothing is reported, and the next
        // `LayoutDelta` puts the windows back, which is the same contract a
        // client that fell behind already has.
        case proto::MessageType::SetLayout:
        case proto::MessageType::ZoomTerm:
        case proto::MessageType::Raise:
        // `MoveResize` is not a window at all: that rect is the terminal's own
        // grid and it SIZES A PTY (the protocol spec, "Two geometries"). A watcher's
        // mirror lays out and reports one without any reader asking it to, so
        // leaving this out would have every watcher silently resizing the
        // children of the session they came to look at.
        case proto::MessageType::MoveResize:
        // The desktop is every reader's coordinate space and reflowing it
        // SIGWINCHes every child in the session.
        case proto::MessageType::SetDesktopSize:
        // The printer policy answers on the CHILD's behalf, and a discarded job
        // is a job the other reader cannot save any more.
        case proto::MessageType::SetPrinterPolicy:
        case proto::MessageType::PrintJobDiscard:
        // Strictly worse than anything above it: a watcher who can end the
        // server ends every session on the machine. Not in the session model's table
        // because the table asks what changes THIS session, and this changes
        // all of them.
        case proto::MessageType::KillServer:
            return true;

        // Questions, self-repair, and what belongs to this reader alone.
        // `Attach` is here because a resnapshot is an `Attach` (the protocol spec) and
        // because changing your own mode by re-attaching must stay possible —
        // it is also the one message that can take a reader OUT of watching.
        case proto::MessageType::Hello:
        case proto::MessageType::Ping:
        case proto::MessageType::ListSessions:
        case proto::MessageType::Attach:
        case proto::MessageType::Detach:
        case proto::MessageType::ClientResize:
        case proto::MessageType::FocusTerm:
        case proto::MessageType::WatchStats:
        case proto::MessageType::PrintJobFetch:
        case proto::MessageType::PasteAck:
        // Its own scope check, in its own handler: `{Me, ...}` is how a reader
        // stops watching, and `{Others, ...}` is refused there rather than
        // here so the refusal can say which half was the problem.
        case proto::MessageType::SetReaderMode:
            return false;

        // Server→client. See the note above.
        case proto::MessageType::HelloAck:
        case proto::MessageType::Refuse:
        case proto::MessageType::Pong:
        case proto::MessageType::SessionList:
        case proto::MessageType::SessionsChanged:
        case proto::MessageType::Attached:
        case proto::MessageType::Detached:
        case proto::MessageType::ReaderMode:
        case proto::MessageType::LayoutDelta:
        case proto::MessageType::TermOpened:
        case proto::MessageType::TermClosed:
        case proto::MessageType::TermMeta:
        case proto::MessageType::GridDelta:
        case proto::MessageType::ImageAddBegin:
        case proto::MessageType::ImageChunk:
        case proto::MessageType::ImageEnd:
        case proto::MessageType::ImagePlace:
        case proto::MessageType::ImageRemove:
        case proto::MessageType::ClipboardSet:
        case proto::MessageType::TermDiagnostic:
        case proto::MessageType::TermStats:
        case proto::MessageType::Error:
        case proto::MessageType::PrintState:
        case proto::MessageType::PrintJobAdded:
        case proto::MessageType::PrintJobData:
            return false;
    }
    return false;
}

const char* name_of(proto::MessageType type) {
    switch (type) {
        case proto::MessageType::Hello: return "Hello";
        case proto::MessageType::HelloAck: return "HelloAck";
        case proto::MessageType::Refuse: return "Refuse";
        case proto::MessageType::Ping: return "Ping";
        case proto::MessageType::Pong: return "Pong";
        case proto::MessageType::ListSessions: return "ListSessions";
        case proto::MessageType::NewSession: return "NewSession";
        case proto::MessageType::Attach: return "Attach";
        case proto::MessageType::Detach: return "Detach";
        case proto::MessageType::ClientResize: return "ClientResize";
        case proto::MessageType::RenameSession: return "RenameSession";
        case proto::MessageType::KillSession: return "KillSession";
        case proto::MessageType::KillServer: return "KillServer";
        case proto::MessageType::SessionList: return "SessionList";
        case proto::MessageType::SessionsChanged: return "SessionsChanged";
        case proto::MessageType::Attached: return "Attached";
        case proto::MessageType::Detached: return "Detached";
        case proto::MessageType::NewTerminal: return "NewTerminal";
        case proto::MessageType::CloseTerminal: return "CloseTerminal";
        case proto::MessageType::KillTerminal: return "KillTerminal";
        case proto::MessageType::RespawnTerminal: return "RespawnTerminal";
        case proto::MessageType::MoveTerminal: return "MoveTerminal";
        case proto::MessageType::MoveResize: return "MoveResize";
        case proto::MessageType::Raise: return "Raise";
        case proto::MessageType::FocusTerm: return "FocusTerm";
        case proto::MessageType::ZoomTerm: return "ZoomTerm";
        case proto::MessageType::RenameTerminal: return "RenameTerminal";
        case proto::MessageType::Input: return "Input";
        case proto::MessageType::PasteChunk: return "PasteChunk";
        case proto::MessageType::PasteAck: return "PasteAck";
        case proto::MessageType::SetLayout: return "SetLayout";
        case proto::MessageType::WatchStats: return "WatchStats";
        case proto::MessageType::SetDesktopSize: return "SetDesktopSize";
        case proto::MessageType::SetReaderMode: return "SetReaderMode";
        case proto::MessageType::ReaderMode: return "ReaderMode";
        case proto::MessageType::LayoutDelta: return "LayoutDelta";
        case proto::MessageType::TermOpened: return "TermOpened";
        case proto::MessageType::TermClosed: return "TermClosed";
        case proto::MessageType::TermMeta: return "TermMeta";
        case proto::MessageType::GridDelta: return "GridDelta";
        case proto::MessageType::ImageAddBegin: return "ImageAddBegin";
        case proto::MessageType::ImageChunk: return "ImageChunk";
        case proto::MessageType::ImageEnd: return "ImageEnd";
        case proto::MessageType::ImagePlace: return "ImagePlace";
        case proto::MessageType::ImageRemove: return "ImageRemove";
        case proto::MessageType::ClipboardSet: return "ClipboardSet";
        case proto::MessageType::TermDiagnostic: return "TermDiagnostic";
        case proto::MessageType::TermStats: return "TermStats";
        case proto::MessageType::Error: return "Error";
        case proto::MessageType::SetPrinterPolicy: return "SetPrinterPolicy";
        case proto::MessageType::PrintState: return "PrintState";
        case proto::MessageType::PrintJobAdded: return "PrintJobAdded";
        case proto::MessageType::PrintJobFetch: return "PrintJobFetch";
        case proto::MessageType::PrintJobData: return "PrintJobData";
        case proto::MessageType::PrintJobDiscard: return "PrintJobDiscard";
    }
    // Not a message this build has a name for, which after the switch above
    // can only be a value no enumerator has: a frame whose type field was
    // corrupt or came from a build that is not this one.
    return "an unrecognised message";
}

}  // namespace

Server::Server(Options options, ckv::Clock& clock)
    : options_(std::move(options)), clock_(clock), terminals_(options_.settings) {}

Server::~Server() = default;

Server::StartStatus Server::start() {
    if (!SignalPipe::instance().open()) return StartStatus::Failed;
    install_signal_handlers();
    switch (listener_.listen(options_.socket)) {
        case platform::Listener::Status::Listening: break;
        case platform::Listener::Status::AlreadyRunning: return StartStatus::AlreadyRunning;
        case platform::Listener::Status::Racing: return StartStatus::Racing;
        case platform::Listener::Status::Failed: return StartStatus::Failed;
    }
    next_tick_nanos_ = clock_.now_nanos();
    return StartStatus::Listening;
}

std::size_t Server::greeted_count() const noexcept {
    std::size_t count = 0;
    for (const std::unique_ptr<Client>& client : clients_)
        if (client->greeted) ++count;
    return count;
}

EffectivePrinterPolicy Server::effective_printer_policy(TerminalId id) const {
    // Terminal → session → global → built-in, in that order, and each of the
    // three numbers resolved SEPARATELY. A scope that says "always capture"
    // and nothing else must not also drag its own idea of a spool size along
    // with it: what a reader set is what a reader set, and the rest keeps
    // coming from wherever it was already coming from (the interface spec, the configuration spec).
    //
    // The scope each answer came from travels with it, because the Printer
    // Settings dialog is required to show a reader not only what is in force
    // but where it came from — a per-session override they set an hour ago and
    // forgot is exactly the thing an effective-value display must not hide.
    EffectivePrinterPolicy resolved;
    resolved.mode = options_.settings.printer_mode;
    resolved.ask_cache =
        static_cast<std::uint32_t>(std::min<std::size_t>(options_.settings.printer_ask_cache_bytes,
                                                         0xFFFFFFFFu));
    resolved.spool_limit = static_cast<std::uint32_t>(
        std::min<std::size_t>(options_.settings.printer_spool_limit_bytes, 0xFFFFFFFFu));

    const auto apply = [&resolved](const PrinterOverride& said, proto::PrinterScope from) {
        if (said.mode) {
            resolved.mode = *said.mode;
            resolved.mode_from = from;
        }
        if (said.ask_cache) {
            resolved.ask_cache = *said.ask_cache;
            resolved.ask_cache_from = from;
        }
        if (said.spool_limit) {
            resolved.spool_limit = *said.spool_limit;
            resolved.spool_limit_from = from;
        }
    };

    // Widest first, so the narrowest scope that says anything is the one whose
    // answer survives — and `..._from` ends up naming it.
    if (const Session* const home = session_holding(id))
        apply(home->printer, proto::PrinterScope::Session);
    if (const Terminal* const terminal = terminals_.find(id))
        apply(terminal->printer_override(), proto::PrinterScope::Terminal);
    return resolved;
}

std::size_t Server::queued_bytes() const noexcept {
    std::size_t queued = 0;
    for (const std::unique_ptr<Client>& client : clients_) queued += client->stream.queued();
    return queued;
}

std::size_t Server::owed_image_bytes() const noexcept {
    std::size_t owed = 0;
    for (const std::unique_ptr<Client>& client : clients_)
        for (std::size_t index = client->owed_sent; index < client->owed_images.size(); ++index)
            if (const auto* chunk = std::get_if<proto::ImageChunk>(&client->owed_images[index].message))
                owed += chunk->bytes.size();
    return owed;
}

std::size_t Server::owed_image_ops() const noexcept {
    std::size_t owed = 0;
    for (const std::unique_ptr<Client>& client : clients_)
        owed += client->owed_images.size() - client->owed_sent;
    return owed;
}

bool Server::waiting_to_heal() const noexcept {
    for (const std::unique_ptr<Client>& client : clients_)
        if (client->dirty_snapshot) return true;
    return false;
}

std::int64_t Server::nanos_until_tick() const {
    const std::int64_t now = clock_.now_nanos();
    return next_tick_nanos_ > now ? next_tick_nanos_ - now : 0;
}

bool Server::step() {
    const std::int64_t now = clock_.now_nanos();
    poller_.clear();
    // The listener, unless accepting is paused — and then it is left out of the
    // set rather than merely left unasked. A listener with a connection pending
    // is readable until that connection is accepted, so watching one this
    // server has decided not to ask from is a `poll()` that returns instantly,
    // every time, for as long as the pause lasts.
    if (now >= accept_paused_until_nanos_)
        poller_.watch(listener_.fd(), platform::Interest::Read);
    poller_.watch(SignalPipe::instance().reader(), platform::Interest::Read);
    for (const std::unique_ptr<Client>& client : clients_) {
        if (!client->stream.open()) continue;
        poller_.watch(client->stream.fd(), client->stream.wants_write()
                                              ? platform::Interest::Read | platform::Interest::Write
                                              : platform::Interest::Read);
    }
    // Every PTY the terminals are waiting on, so a child's output wakes the
    // loop instead of being noticed a tick later.
    for (const ckv::term::WaitHandle& handle : terminals_.wait_handles())
        if (handle.kind == ckv::term::WaitHandleKind::PosixFileDescriptor)
            poller_.watch(static_cast<int>(handle.value), platform::Interest::Read);

    std::int64_t until_wake = nanos_until_tick();
    // A pause is a deadline like the tick is, and the wait must not sleep past
    // it: nothing else will wake this loop while the listener is out of the
    // set, so a server that slept its full tick would still be right, and a
    // server with no terminals and no clients would sit there for a second
    // longer than it said it would.
    if (accept_paused_until_nanos_ > now)
        until_wake = std::min(until_wake, accept_paused_until_nanos_ - now);
    // The stats sampler's deadline joins the wait only while somebody watches;
    // a server nobody subscribed to has no per-second wakeup at all. The
    // off-to-on edge drops every baseline, because a rate derived against a
    // sample from before the sampler went idle would average over the whole
    // quiet stretch rather than say what is happening now.
    const bool watching_stats = anyone_watching_stats();
    if (watching_stats && !stats_were_running_) {
        stats_baselines_.clear();
        next_stats_nanos_ = now;
    }
    stats_were_running_ = watching_stats;
    if (watching_stats) until_wake = std::min(until_wake, nanos_until_stats());
    const int timeout_ms = static_cast<int>(std::min<std::int64_t>(until_wake / 1'000'000, 1'000));
    const std::vector<platform::Ready>& ready = poller_.wait(timeout_ms);
    switch (poller_.outcome()) {
        case platform::Poller::Outcome::Failed:
            // A wait that failed will fail the same way again, and the empty
            // set it hands back cannot be told from a timeout — which is a loop
            // running at full speed with nothing to show for it. Three in a row
            // is a system this server cannot work on, so it says so and goes,
            // which at least gets the children closed down properly.
            if (++poll_failures_ >= 3) {
                std::fprintf(stderr, "ckmux server: cannot wait on its descriptors; stopping\n");
                running_ = false;
                return false;
            }
            break;
        case platform::Poller::Outcome::Interrupted:  // the next pass looks again
        case platform::Poller::Outcome::TimedOut:
        case platform::Poller::Outcome::Ready:
            poll_failures_ = 0;
            break;
    }

    bool child_output = false;
    for (const platform::Ready& event : ready) {
        if (event.fd == listener_.fd()) {
            accept_pending();
            continue;
        }
        if (event.fd == SignalPipe::instance().reader()) {
            const int signal = SignalPipe::instance().take();
            if (signal != 0) {
                // A signal is a request to end, and the answer is the same
                // orderly shutdown a `KillServer` gets: children first, socket
                // second. A server killed with -9 leaves its children to the
                // close protocol of whoever reaps them, which is why -9 is the
                // one case this cannot make tidy.
                std::fprintf(stderr, "ckmux server: stopping on signal %d\n", signal);
                running_ = false;
            }
            continue;
        }
        Client* owner = nullptr;
        for (const std::unique_ptr<Client>& client : clients_)
            if (client->stream.fd() == event.fd) owner = client.get();
        if (owner != nullptr) {
            if (event.writable && !owner->stream.flush()) {
                drop(*owner, "the connection closed while writing");
                continue;
            }
            if (event.readable || event.hangup) serve(*owner);
            continue;
        }
        // Anything else in the set is a PTY: reading it is the terminals'
        // business, and they are drained together below so that one child
        // cannot be served twice in a pass while another waits.
        child_output = true;
    }

    if (child_output) {
        // 64 KiB per terminal per pass (the architecture spec). Bounded because a child
        // running `yes` must not be able to hold this loop.
        (void)terminals_.drain(64 * 1024);
        for (const TerminalId id : terminals_.ids())
            if (Terminal* terminal = terminals_.find(id)) terminal->flush_replies_to_child();
    }

    if (clock_.now_nanos() >= next_tick_nanos_) flush_tick();
    if (watching_stats && clock_.now_nanos() >= next_stats_nanos_) stats_tick();

    // Connections that asked to go, and the streams that died under them.
    const std::size_t before = clients_.size();
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [](const std::unique_ptr<Client>& client) {
                                      return client->closing || !client->stream.open();
                                  }),
                   clients_.end());
    // A closed connection is a descriptor back. Waiting out the rest of an
    // accept pause when the thing it was waiting for has already happened only
    // makes the next reader wait for nothing.
    if (clients_.size() < before) accept_paused_until_nanos_ = 0;
    return running_;
}

void Server::accept_pending() {
    for (;;) {
        const platform::Listener::AcceptResult result = listener_.accept_one();
        switch (result.status) {
            case platform::Listener::AcceptStatus::Accepted: {
                auto client = std::make_unique<Client>();
                client->id = next_client_++;
                client->stream = platform::Stream(result.fd);
                clients_.push_back(std::move(client));
                continue;
            }
            case platform::Listener::AcceptStatus::Idle:
                // Nothing waiting: the ordinary answer, several times a second,
                // and worth no word to anybody.
                return;
            case platform::Listener::AcceptStatus::Retry:
                // A signal, or a peer that hung up while it was in the queue.
                continue;
            case platform::Listener::AcceptStatus::Refused:
            case platform::Listener::AcceptStatus::Failed:
                std::fprintf(stderr, "ckmux server: %s\n", result.problem.c_str());
                return;
            case platform::Listener::AcceptStatus::Exhausted:
                // Out of descriptors. Asking again is wrong — nothing will have
                // changed — and so is going back round the loop, which would
                // find the listener readable and come straight back here. So
                // accepting stops for a moment, and the listener stops being
                // watched with it (M-S2).
                std::fprintf(stderr, "ckmux server: %s; not accepting for a second\n",
                             result.problem.c_str());
                accept_paused_until_nanos_ = clock_.now_nanos() + 1'000'000'000;
                return;
        }
    }
}

void Server::serve(Client& client) {
    std::string arrived;
    const bool alive = client.stream.receive(arrived);
    if (!arrived.empty() && !client.reader.append(arrived)) {
        drop(client, "sent more than a frame may ever be");
        return;
    }
    for (;;) {
        proto::Message message;
        const proto::DecodeError error = client.reader.next(message);
        if (error == proto::DecodeError::Incomplete) break;
        if (error != proto::DecodeError::None) {
            // Fatal for this connection and nothing else. There is no way to
            // resynchronise a stream whose framing is wrong, and guessing would
            // mean acting on bytes of unknown provenance.
            drop(client, "sent a frame this server cannot read");
            return;
        }
        handle(client, message);
        if (client.closing) return;
    }
    // End of stream is how a detach looks from here: the client is gone, the
    // terminals are not. That is the whole promise of the project, and it is
    // one line because WP-3 put the ownership in the right place.
    if (!alive) drop(client, "");
}

void Server::handle(Client& client, const proto::Message& message) {
    const proto::MessageType type = proto::type_of(message);

    // A CLI client already told a version mismatch to run this exact request,
    // and now has nothing left to say but the request itself: `KillServer`
    // needs no protocol agreement (no payload; its frame is the eight-byte
    // header alone), so it is the one message honoured without a completed
    // handshake. Anything else — a second Hello, an ordinary command, silence
    // past the deadline `flush_tick` enforces — ends the connection exactly as
    // an ordinary mismatch would.
    if (client.awaiting_kill_from_mismatched_cli) {
        if (std::holds_alternative<proto::KillServer>(message)) {
            std::fprintf(stderr, "ckmux server: stopping at a client's request (protocol mismatch)\n");
            running_ = false;
            return;
        }
        drop(client, "spoke another version of the protocol and did not ask to kill the server");
        return;
    }

    if (!client.greeted) {
        // Every connection says Hello first. A connection that says anything
        // else is either a much older client or not a client at all, and in
        // both cases the server has nothing to say back that could be
        // understood.
        const auto* hello = std::get_if<proto::Hello>(&message);
        if (hello == nullptr) {
            send(client, proto::Refuse{"the first message on a connection must be Hello; this one "
                                       "was " + std::string(name_of(type))});
            drop(client, "did not say Hello first");
            return;
        }
        if (hello->proto_version != proto::kProtocolVersion) {
            // The one message a reader meets after an upgrade, so it carries
            // both numbers and the remedy in words (the protocol spec). "Version
            // mismatch" on its own tells them nothing they can act on.
            send(client,
                 proto::Refuse{"this server speaks protocol version " +
                               std::to_string(proto::kProtocolVersion) + " and the client speaks " +
                               std::to_string(hello->proto_version) +
                               ". They are the same program, so one of them is from before an "
                               "upgrade: end the running server with `ckmux kill-server` (every "
                               "terminal in it is lost) or keep using the client that matches it."});
            // A UI client has nothing safe to do with a server it cannot
            // understand and is dropped exactly as before. A CLI client is
            // given one bounded chance to be the `kill-server` this refusal
            // just told somebody to run — which is the ordinary reason a CLI
            // client is the one connecting to a server it does not match.
            if (hello->client_kind == proto::ClientKind::Cli) {
                client.awaiting_kill_from_mismatched_cli = true;
                client.mismatch_kill_deadline_nanos = clock_.now_nanos() + 2'000'000'000;
                return;
            }
            drop(client, "speaks another version of the protocol");
            return;
        }
        client.greeted = true;
        client.kind = hello->client_kind;
        proto::HelloAck ack;
        ack.build = std::string(proto::kBuildIdentity);
        send(client, ack);
        return;
    }

    // A reader who asked only to look (WP-49). One gate here rather than a
    // guard in each of fifteen handlers, and the difference is not tidiness:
    // `changes_the_session` is an EXHAUSTIVE switch, so the day somebody adds a
    // message the build stops until they have said which side of this line it
    // falls on. Fifteen scattered guards are fifteen chances to forget, and the
    // one that is forgotten is a hole in a mode whose whole value is that it
    // has none.
    //
    // Enforced at the server although the client greys its own views, because a
    // mode a stale or modified client can decline to honour is not a mode.
    if (client.watching() && changes_the_session(type)) {
        // `Input` is the one deliberate silence in this handler, and it reads
        // like exactly the bug the rest of it exists to avoid — so: a client
        // waiting on a reply that never comes cannot tell a server from a hung
        // one, which is why everything else is answered. This client is not
        // waiting. It asked for this mode, it knows it is watching, and it
        // greys its own views on the strength of that; the drop here is the
        // backstop, not the reader-facing behaviour. An `Error` per keystroke
        // would put a frame on the hot path for every key a reader leans on.
        if (type == proto::MessageType::Input) return;
        // A paste is acked and discarded rather than refused, for the reason
        // the ack already exists (WP-18): the credit is about the CLIENT'S
        // queue, not about what happened to the bytes, and a chunk left unacked
        // holds the rest of that paste — and every later one — in a queue
        // nothing drains. A watcher who pastes gets nothing pasted and no
        // wedged client.
        if (const auto* chunk = std::get_if<proto::PasteChunk>(&message)) {
            proto::PasteAck ack;
            ack.seq = chunk->seq;
            send(client, ack);
            return;
        }
        (void)refuse_if_watching(client, name_of(type));
        return;
    }

    if (const auto* ping = std::get_if<proto::Ping>(&message)) {
        proto::Pong pong;
        pong.nonce = ping->nonce;
        send(client, pong);
        return;
    }
    if (const auto* watch = std::get_if<proto::WatchStats>(&message)) {
        // Nothing to answer: the first `TermStats` within a sample period IS
        // the answer, and a client that turned the last checkbox off simply
        // stops hearing them. `step()` notices the off-to-on edge and starts
        // the sampler from fresh baselines.
        client.watch_stats = watch->on != 0;
        return;
    }
    if (std::holds_alternative<proto::Hello>(message)) {
        drop(client, "said Hello twice");
        return;
    }
    if (std::holds_alternative<proto::ListSessions>(message)) {
        // Asked before attaching, which is the whole point: a client decides
        // what to do — attach, take over, or start a new one — from what is
        // actually running (the interface spec's picker).
        //
        // Timestamps stay zero: a server's clock is monotonic by design
        // (ckVision's determinism rule), so "created at" needs a wall-clock
        // reading nothing here is entitled to take.
        send_session_list(client);
        return;
    }
    if (const auto* request = std::get_if<proto::NewSession>(&message)) {
        // Two sessions with one name is a reader looking at a picker that
        // cannot tell them which is which, and a `ckmux attach build` that has
        // to refuse rather than answer. The id is still the identity — that is
        // what everything on the wire refers to — but the NAME is the only
        // handle a reader has, and a handle that points at two things is not
        // one. Refused here rather than silently disambiguated, because the
        // reader is right here and can choose another.
        if (const Session* const taken = session_named(request->name)) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NameTaken);
            error.context = "NewSession";
            error.human = "a session is already called \"" + taken->name + "\"";
            send(client, error);
            return;
        }
        Session& session = create_session(request->name);
        // The fields this message always carried and this handler ignored,
        // found by WP-11: `ckmux new 'make -j8'` made an empty session and
        // dropped the command on the floor — and `spawn_first` DEFAULTS to on,
        // so the silent half was the default. The terminal opens at the
        // conventional size, because a session created over a bare connection
        // has no watcher whose display could say better; the first client to
        // attach resizes it a frame later, exactly as a `NewTerminal` opened
        // before layout is.
        if (request->spawn_first != 0) {
            TerminalSpec spec;
            spec.command = request->command;
            spec.working_directory = options_.working_directory;
            spec.host_sixel = client.host_sixel;
            (void)open_terminal(session.id, spec);
        }
        std::fprintf(stderr, "ckmux server: session %llu \"%s\" created\n",
                     static_cast<unsigned long long>(session.id), session.name.c_str());
        // The list, not just an id: the client that asked wants to show the
        // result, and every other client's picker is now out of date.
        broadcast_session_list();
        return;
    }
    if (const auto* request = std::get_if<proto::KillSession>(&message)) {
        Session* const session = session_for(request->session != 0 ? request->session : client.session);
        if (session == nullptr) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchSession);
            error.context = "KillSession";
            error.human = "no such session";
            send(client, error);
            return;
        }
        begin_kill(*session, request->force != 0, static_cast<int>(request->grace_seconds));
        return;
    }
    if (const auto* request = std::get_if<proto::RenameSession>(&message)) {
        Session* const session =
            session_for(request->id != 0 ? request->id : client.session);
        if (session == nullptr) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchSession);
            error.context = "RenameSession";
            error.human = "no such session";
            send(client, error);
            return;
        }
        // The same rule as `NewSession`, and it has to be the same rule: a
        // name a reader cannot take at creation is a name they could otherwise
        // reach in two steps by renaming into it, which would leave the picker
        // in exactly the state the other handler exists to prevent.
        //
        // `except_id` is what makes renaming a session to the name it already
        // has a no-op rather than a refusal — a reader confirming a dialog
        // without changing the text has not asked for anything and should not
        // be told off for it. An EMPTY name still leaves the name alone, for
        // the older reason: a session with no name is a row in the picker with
        // nothing to point at.
        if (const Session* const taken = session_named(request->name, session->id)) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NameTaken);
            error.context = "RenameSession";
            error.human = "a session is already called \"" + taken->name + "\"";
            send(client, error);
            return;
        }
        session->name = request->name.empty() ? session->name : request->name;
        broadcast_session_list();
        return;
    }
    if (const auto* request = std::get_if<proto::RenameTerminal>(&message)) {
        Terminal* const terminal = terminals_.find(request->id);
        if (terminal == nullptr) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchTerminal);
            error.context = "RenameTerminal";
            error.human = "no such terminal";
            send(client, error);
            return;
        }
        // An EMPTY name is not a refused rename, it is the other half of the
        // feature: "use the default title again", which hands the caption back
        // to whatever the child claims. A session name cannot be cleared —
        // there the empty string means "leave it alone", because a session with
        // no name is a row in the picker with nothing to point at — and this is
        // the one place the two renames deliberately differ.
        //
        // Not refused for length either: `set_custom_title` clamps, on a
        // character boundary and through the same function the child's own
        // title goes through.
        (void)terminal->set_custom_title(request->name);
        // Nothing is sent from here. The announcement rides the tick, through
        // the same edge-triggered `TermMeta` the marks and the child's title
        // use — including to the client that asked, which needs to be told what
        // the server actually stored rather than to assume its own request was
        // honoured verbatim.
        return;
    }
    if (const auto* request = std::get_if<proto::PrintJobFetch>(&message)) {
        Terminal* const terminal = terminals_.find(request->term);
        const Terminal::HeldJob* const job =
            terminal == nullptr ? nullptr : terminal->print_job(request->job);
        if (job == nullptr) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchTerminal);
            error.context = "PrintJobFetch";
            error.human = terminal == nullptr ? "no such terminal" : "no such print job";
            send(client, error);
            return;
        }
        // Chunked, because a job may be a megabyte and one frame carrying it
        // would stall every other terminal behind it — the same reason a
        // picture is chunked (the protocol spec). The sequence lets a client assemble
        // without trusting arrival order, and `final_chunk` is what says the
        // document is whole rather than merely paused.
        //
        // An overflowed job has no text and still gets one final, empty chunk:
        // "this job printed nothing we kept" is an answer, and a client left
        // waiting for a chunk that never came would show a spinner forever.
        constexpr std::size_t kChunk = 48U * 1024U;
        const std::string& text = job->text;
        std::uint32_t seq = 0;
        for (std::size_t at = 0; at < text.size() || at == 0; at += kChunk) {
            proto::PrintJobData data;
            data.term = request->term;
            data.job = job->id;
            data.seq = seq++;
            const std::size_t take = std::min(kChunk, text.size() - std::min(at, text.size()));
            data.bytes = text.substr(std::min(at, text.size()), take);
            data.final_chunk = (at + kChunk >= text.size()) ? 1 : 0;
            send(client, data);
            if (data.final_chunk != 0) break;
        }
        return;
    }
    if (const auto* request = std::get_if<proto::PrintJobDiscard>(&message)) {
        Terminal* const terminal = terminals_.find(request->term);
        if (terminal == nullptr) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchTerminal);
            error.context = "PrintJobDiscard";
            error.human = "no such terminal";
            send(client, error);
            return;
        }
        // A discard of a job already gone is stale rather than wrong — it
        // happens whenever two clients answer the same capture — so it is not
        // refused. What it must do is leave the reader's count right, which
        // the `PrintState` on the next tick does.
        if (terminal->discard_print_jobs(request->job)) terminal->mark_printer_news();
        return;
    }
    if (const auto* request = std::get_if<proto::SetPrinterPolicy>(&message)) {
        // Which scope is being written is the request's to say, and only the
        // narrowest two name a target: `global` is this server's own setting
        // and has no id (the configuration spec).
        PrinterOverride said;
        said.mode = wire_printer_mode_in(request->mode);
        if (request->ask_cache != 0) said.ask_cache = request->ask_cache;
        if (request->spool_limit != 0) said.spool_limit = request->spool_limit;

        switch (request->scope) {
            case proto::PrinterScope::Terminal: {
                Terminal* const terminal = terminals_.find(request->target);
                if (terminal == nullptr) {
                    proto::Error error;
                    error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchTerminal);
                    error.context = "SetPrinterPolicy";
                    error.human = "no such terminal";
                    send(client, error);
                    return;
                }
                terminal->set_printer_override(said);
                break;
            }
            case proto::PrinterScope::Session: {
                Session* const session =
                    session_for(request->target != 0 ? request->target : client.session);
                if (session == nullptr) {
                    proto::Error error;
                    error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchSession);
                    error.context = "SetPrinterPolicy";
                    error.human = "no such session";
                    send(client, error);
                    return;
                }
                session->printer = said;
                break;
            }
            case proto::PrinterScope::Global:
                // The server's own setting, which is also what gets written
                // back to the file when a reader chooses "save as default"
                // (the configuration spec) — that write is the CLIENT's, because the file is
                // the reader's and a server may be running for somebody else.
                if (said.mode) options_.settings.printer_mode = *said.mode;
                if (said.ask_cache) options_.settings.printer_ask_cache_bytes = *said.ask_cache;
                if (said.spool_limit)
                    options_.settings.printer_spool_limit_bytes = *said.spool_limit;
                break;
        }
        // Every terminal the change could reach is told what it now is. A
        // policy is only real once the emulator enforcing it agrees: a
        // terminal left on its launch-time policy would go on capturing after
        // a reader turned capture off, which is the one outcome this dialog
        // exists to prevent.
        apply_printer_policies();
        return;
    }
    if (const auto* request = std::get_if<proto::Attach>(&message)) {
        attach(client, *request);
        return;
    }
    if (std::holds_alternative<proto::Detach>(message)) {
        // Asked for rather than suffered. Answered, so a client that is about to
        // exit knows the server heard it and its terminals are safe.
        detach(client, proto::DetachReason::User, "detached");
        return;
    }
    if (const auto* resize = std::get_if<proto::ClientResize>(&message)) {
        // The DESKTOP, and the cell metric that comes with it. Not the
        // terminals: each of those lives in a window with a frame around it, and
        // is sized by the client that draws it (`MoveResize`, below). Resizing
        // every terminal to the desktop is what put a shell's prompt above the
        // top of its own window.
        client.desktop = ckv::Size{resize->columns, resize->rows};
        client.cell_pixels = cell_metric_of(*resize);
        // And the SESSION's desktop — but only when this reader is the only
        // one who can be hurt by it moving (WP-40, corrected).
        //
        // WP-40's rule was that a reader resizing their own window must not
        // reflow a session's windows for everybody else, and that is right:
        // reflowing SIGWINCHes every child, for every reader watching, because
        // one person dragged a corner. But it was applied to EVERY resize, and
        // the overwhelmingly common session has exactly one reader. For them
        // the rule protected nobody and cost them the desktop: the coordinate
        // space stayed the size the terminal happened to be at first attach,
        // so making the window smaller cut windows off at the edge with no way
        // to reach them, and making it bigger left the new space unusable.
        // Field report, 2026-08-20: "half of ckmux terminal windows get
        // cut-off".
        //
        // So the condition is not "was this a resize" but "is there anybody
        // else here". A sole reader's screen IS the session's desktop and
        // follows it. The moment a second reader is attached, `[general]
        // desktop-size` decides, exactly as it does on attach, and its default
        // leaves the desktop alone.
        Session* const home = session_for(client.session);
        if (home == nullptr) return;  // attached to nothing: nothing to size
        const ckv::Size arriving = client.desktop;
        if (arriving.width <= 0 || arriving.height <= 0) return;
        std::size_t readers = 0;
        for (const std::unique_ptr<Client>& other : clients_)
            if (other->attached && other->kind == proto::ClientKind::Ui &&
                other->session == client.session)
                ++readers;
        ckv::Size wanted = home->desktop;
        if (readers <= 1) {
            wanted = arriving;
        } else {
            switch (options_.settings.desktop_size) {
                case DesktopSizePolicy::Fixed:
                    break;
                case DesktopSizePolicy::FitSmallest:
                    wanted = ckv::Size{std::min(home->desktop.width, arriving.width),
                                       std::min(home->desktop.height, arriving.height)};
                    break;
                case DesktopSizePolicy::FitLatest:
                    wanted = arriving;
                    break;
            }
        }
        if (wanted.width <= 0 || wanted.height <= 0) return;
        if (wanted.width == home->desktop.width && wanted.height == home->desktop.height) return;
        home->desktop = wanted;
        // Everybody watching is told, for the same reason `SetDesktopSize`
        // tells them: a coordinate space one client believes and another does
        // not is two different readings of every rect in the session.
        announce_layout(home->id);
        return;
    }
    if (const auto* fit = std::get_if<proto::SetDesktopSize>(&message)) {
        // A reader ASKING for the session's desktop to become this size
        // (WP-40) — as against `ClientResize` above, which reports a fact
        // about one screen and must move nothing. This is the only path by
        // which a client changes the coordinate space its neighbour's windows
        // are arranged in, and it exists because that must be an act somebody
        // took rather than a consequence of dragging a window corner.
        Session* const home = session_for(client.session);
        if (home == nullptr) return;  // attached to nothing: nothing to size
        const ckv::Size wanted{static_cast<int>(fit->columns), static_cast<int>(fit->rows)};
        if (wanted.width <= 0 || wanted.height <= 0) return;
        if (wanted.width == home->desktop.width && wanted.height == home->desktop.height) return;
        home->desktop = wanted;
        // Everybody watching is told, not just the reader who asked: a
        // coordinate space one client believes and another does not is two
        // different readings of every rect in the session. Forced, because a
        // desktop resize is not a window moving and the edge trigger would
        // otherwise swallow it whenever every window happened to stay put.
        announce_layout(home->id);
        return;
    }
    if (const auto* resize = std::get_if<proto::MoveResize>(&message)) {
        Terminal* terminal = terminals_.find(resize->term);
        if (terminal == nullptr) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchTerminal);
            error.context = "MoveResize";
            error.human = "no such terminal";
            send(client, error);
            return;
        }
        // Clamped at both ends. The floor because a terminal of no size is not
        // a terminal; the ceiling because `rect` is two `u16`s, and a client
        // asking for 65535 by 65535 is asking this server to allocate four
        // billion cells on its say-so. It is also what makes the attach
        // snapshot bounded rather than merely usually small: the budget is
        // arithmetic over grids whose size has a maximum (R1's
        // kSnapshotPayloadBudget).
        const int columns = std::clamp(static_cast<int>(resize->rect.width), 1,
                                       static_cast<int>(proto::kMaxGridColumns));
        const int rows = std::clamp(static_cast<int>(resize->rect.height), 1,
                                    static_cast<int>(proto::kMaxGridRows));
        terminal->resize(columns, rows, columns * std::max(0, client.cell_pixels.width),
                         rows * std::max(0, client.cell_pixels.height));
        return;
    }
    if (const auto* report = std::get_if<proto::SetLayout>(&message)) {
        // Where this client's windows now are. Recorded, and deliberately
        // nothing else: no PTY is sized from it (that is `MoveResize` above,
        // whose rect is the terminal's own grid rather than the window drawn
        // around it), and no window is placed by it. The server is the thing
        // that REMEMBERS an arrangement so that it survives the client that
        // made it (the session model) — what a reattaching client then does with one is
        // its own decision and a package of its own.
        Session* const session = session_of(client);
        if (session == nullptr) {
            // A layout belongs to a session, so a client that is in none has
            // said something this server cannot file anywhere. Answered rather
            // than dropped, because an unanswered request looks to a reader
            // exactly like a server that has hung.
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchSession);
            error.context = "SetLayout";
            error.human = "attach to a session before reporting its layout";
            send(client, error);
            return;
        }
        for (const proto::LayoutEntry& entry : report->entries) {
            // A terminal that has gone, or one this client does not watch, is
            // skipped in silence rather than refused: a report describes an
            // arrangement at a moment, and a window closing or moving to
            // another session races it every time — which is stale, not wrong
            // (the session model says the same about closing a terminal twice).
            Terminal* const terminal = terminals_.find(entry.term);
            if (terminal == nullptr) continue;
            if (std::find(session->terminals.begin(), session->terminals.end(), entry.term) ==
                session->terminals.end())
                continue;
            WindowLayout layout;
            layout.rect = entry.rect;
            layout.z_order = entry.z_order;
            layout.zoomed = entry.zoomed != 0;
            // Recorded exactly as reported, and — like the rect — never
            // recomputed here (WP-30). Whether an arrangement was a filled
            // tiling is ckVision's answer about the desktop the reader was
            // looking at; this server has no desktop and could only guess at it
            // from rects whose content area it does not know.
            layout.tile = entry.tile;
            (void)terminal->set_layout(layout);
        }
        // What actually changed goes out on the tick, in one message for the
        // whole session — see `announce_layout`. Not from here: a drag reports
        // many times between two ticks, and answering each one would put a
        // message on the wire per intermediate frame, which is the cost WP-7
        // measured on the delta path and the reason that path coalesces too.
        return;
    }
    if (const auto* request = std::get_if<proto::NewTerminal>(&message)) {
        // Into the session this client is attached to. A client that is not
        // attached has no session for a terminal to live in, and saying so is
        // better than inventing one behind its back.
        Session* const session = session_of(client);
        if (session == nullptr) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchSession);
            error.context = "NewTerminal";
            error.human = "attach to a session before opening a terminal in it";
            send(client, error);
            return;
        }
        // The session model's new-terminal row: "Session at terminal limit (config,
        // default 64) → error". Checked here rather than in `open_terminal`,
        // because this is the request a reader makes — a session created with
        // `spawn_first` is one terminal into a limit of at least one, and a
        // respawn replaces a terminal rather than adding one.
        if (static_cast<int>(session->terminals.size()) >= options_.settings.max_terminals) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::LimitReached);
            error.context = "NewTerminal";
            error.human = "this session already holds " +
                          std::to_string(options_.settings.max_terminals) +
                          " terminals, which is [general] max-terminals";
            send(client, error);
            return;
        }
        TerminalSpec spec;
        spec.command = request->command;
        // The opener's own display decides what the child is told about
        // graphics (WP-16): the reader in front of THIS client is the reader
        // the picture is for. Read off this request rather than
        // `client.host_sixel` (Attach's own, connection-level answer): the
        // capability probe that answer depends on is asynchronous and this
        // client may have connected before it resolved, which `Attach` alone
        // could never recover from for any pane opened afterward. Kept in
        // sync with the connection-level belief too, since a takeover or a
        // second client attaching to this session reads that one instead.
        spec.host_sixel = request->host_sixel != 0;
        client.host_sixel = spec.host_sixel;
        // Where a shell opens, and what it is told about its machine. The
        // server is the one that knows both, because it is the process that will
        // hold the child (the architecture spec) — a client's own environment is its own.
        // Stated in the options rather than read here: a terminal that opened
        // in whatever HOME the process that started this server happened to
        // carry would open somewhere different depending on who started it.
        spec.working_directory = options_.working_directory;
        // The size the client asked for, if it said — a `NewTerminal` carries a
        // rect for exactly this. A client with no opinion yet gets a
        // conventional screen and corrects it the moment its view is laid out,
        // which is one frame away. Held to the same ceiling a resize is, and
        // for the same reason: a grid this server will allocate is one the
        // protocol can carry back out again.
        spec.columns = request->rect.width > 0
                           ? std::min(static_cast<int>(request->rect.width),
                                      static_cast<int>(proto::kMaxGridColumns))
                           : 80;
        spec.rows = request->rect.height > 0 ? std::min(static_cast<int>(request->rect.height),
                                                        static_cast<int>(proto::kMaxGridRows))
                                             : 24;
        spec.pixel_width = spec.columns * std::max(0, client.cell_pixels.width);
        spec.pixel_height = spec.rows * std::max(0, client.cell_pixels.height);
        Terminal& terminal = open_terminal(session->id, spec);
        proto::TermOpened opened;
        opened.term = terminal.id();
        opened.session = session->id;
        opened.columns = static_cast<std::uint16_t>(spec.columns);
        opened.rows = static_cast<std::uint16_t>(spec.rows);
        // Only to the client watching THAT session: a terminal in somebody
        // else's session is none of this one's business.
        for_each_attached(session->id, [&](Client& watcher) { send(watcher, opened); });
        return;
    }
    if (const auto* request = std::get_if<proto::RespawnTerminal>(&message)) {
        // WP-13: the same command in the same window (the interface spec). The id
        // does not change — that is what "same window" means to a client,
        // whose window is keyed on it — so this is not an id being reused for
        // a second terminal (WP-3), it is one terminal running again.
        Terminal* const terminal = terminals_.find(request->term);
        if (terminal == nullptr) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchTerminal);
            error.context = "RespawnTerminal";
            error.human = "no such terminal";
            send(client, error);
            return;
        }
        // Refused while the child is alive. Relaunching over a running program
        // would drop the last handle on its process group — a process still
        // running that nothing can reach — which is the one outcome a
        // multiplexer must never produce. A reader who wants that asks for a
        // close first, and the close protocol exists to do it gracefully.
        if (!terminals_.respawn(request->term)) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchTerminal);
            error.context = "RespawnTerminal";
            error.human = "that terminal is still running";
            send(client, error);
            return;
        }
        // Everything believed about this terminal is now wrong in every cell,
        // so the belief is dropped rather than diffed against: the next tick
        // finds no differ and states the terminal whole. `TermOpened` re-announces
        // it under the id the client already has, which is what lets the window
        // survive its child.
        diffs_.forget(request->term);
        stats_baselines_.erase(request->term);
        if (Session* const home = session_holding(request->term)) {
            proto::TermOpened opened;
            opened.term = request->term;
            opened.session = home->id;
            opened.columns = static_cast<std::uint16_t>(terminal->columns());
            opened.rows = static_cast<std::uint16_t>(terminal->rows());
            for_each_attached(home->id, [&](Client& watcher) { send(watcher, opened); });
        }
        return;
    }
    if (const auto* input = std::get_if<proto::Input>(&message)) {
        // Opaque on purpose: the bytes were encoded by the client against
        // mirrored mode state (the terminal-emulation spec), so the server writes them to the PTY
        // without understanding them. A server that re-encoded would be a third
        // opinion about the child's dialect.
        if (Terminal* terminal = terminals_.find(input->term)) {
            terminal->send_input(input->bytes);
            // The terminal a reader's input goes to is the terminal the reader
            // is in. That is the only statement of focus this protocol carries
            // today — `Raise` and `FocusTerm` encode and nothing sends them —
            // and it is a true one: keys, mouse reports and focus reports all
            // arrive as `Input`. Kept because two things read it: the snapshot
            // a reattaching client is given (`focused_term`), and the activity
            // mark, which means "output in a terminal the reader is not in".
            client.focused = input->term;
            if (Session* const home = session_holding(input->term))
                home->last_focused = input->term;
        }
        return;
    }
    if (const auto* chunk = std::get_if<proto::PasteChunk>(&message)) {
        // A paste, one credited chunk at a time (WP-18). The bytes are as
        // opaque here as `Input`'s and for the same reason: the client encoded
        // them against mirrored mode state — bracketed-paste markers included
        // — and a server that re-encoded would be a third opinion about the
        // child's dialect.
        Terminal* const terminal = terminals_.find(chunk->term);
        if (terminal == nullptr) {
            // Acked whether or not the terminal was there. The credit is about
            // the CLIENT'S queue, not about this terminal's existence: a client
            // that pastes into a window whose program has just ended would
            // otherwise wait for ever on an ack that is never coming, holding
            // the rest of the paste — and every later paste behind it — in a
            // queue nothing drains. An ack for a terminal that has gone costs
            // one small frame and says only "this chunk is dealt with", which
            // is true.
            forget_paste_slot(chunk->term);
            proto::PasteAck ack;
            ack.seq = chunk->seq;
            send(client, ack);
            return;
        }
        // One paste at a time (WP-42). A chunk from anybody but the reader
        // currently mid-paste waits, and waits UNACKED — which is what bounds
        // it: WP-18's credit stops each client at two chunks on the wire, so
        // however long the paste in front of them is, a terminal holds at most
        // two per waiting reader.
        PasteSlot& slot = paste_slots_[chunk->term];
        if (slot.owner != 0 && slot.owner != client.id) {
            slot.waiting.push_back(PendingPaste{client.id, *chunk});
            return;
        }
        write_paste_chunk(*terminal, client, *chunk);
        return;
    }
    if (const auto* wish = std::get_if<proto::SetReaderMode>(&message)) {
        handle_set_reader_mode(client, *wish);
        return;
    }
    if (const auto* close_it = std::get_if<proto::CloseTerminal>(&message)) {
        // Asked, watched, and only then removed — the terminal keeps being
        // served while its program decides, so a goodbye it prints lands on
        // the reader's screen rather than in a closed PTY.
        begin_close(close_it->term, close_it->force != 0,
                    static_cast<int>(close_it->grace_seconds));
        return;
    }
    if (const auto* kill_it = std::get_if<proto::KillTerminal>(&message)) {
        // The session model's operations table has promised this since M1 and nothing
        // implemented it: the message type existed, sat in the `Message`
        // variant and round-tripped in `test_proto`'s catalogue, and no server
        // handler ever claimed it — so a reader asking got the not-implemented
        // refusal below, which named the wrong packages and did not even say
        // which request had failed.
        begin_kill_terminal(kill_it->term);
        return;
    }
    if (const auto* move = std::get_if<proto::MoveTerminal>(&message)) {
        move_terminal(client, *move);
        return;
    }
    if (std::holds_alternative<proto::KillServer>(message)) {
        std::fprintf(stderr, "ckmux server: stopping at a client's request\n");
        running_ = false;
        return;
    }

    // Everything else is a message this build does not implement yet. Answering
    // is not a formality: a client waiting for a reply that never comes looks
    // to a reader exactly like a server that has hung.
    proto::Error error;
    error.code = static_cast<std::uint16_t>(proto::ErrorCode::NotImplemented);
    error.context = name_of(type);
    // No package numbers. The old text promised sessions "with WP-8" and attach
    // "with WP-6" — both landed on 2026-08-17/18, so for three days the server
    // answered every unimplemented request by pointing at work that was already
    // done. A refusal a reader can act on says what was asked and that this
    // build cannot do it; which release fixes it is not a fact the server has.
    error.human = std::string(name_of(type)) + " is not something this build of the server does.";
    send(client, error);
}

Session* Server::session_for(SessionId id) {
    for (Session& session : sessions_)
        if (session.id == id) return &session;
    return nullptr;
}

Session* Server::session_of(const Client& client) {
    return client.attached ? session_for(client.session) : nullptr;
}

std::size_t Server::attached_count(SessionId id) {
    std::size_t watching = 0;
    for_each_attached(id, [&watching](Client&) { ++watching; });
    return watching;
}

proto::AttachMode Server::attach_mode_of(const proto::Attach& request) noexcept {
    switch (static_cast<proto::AttachMode>(request.mode)) {
        case proto::AttachMode::TakeOver:
        case proto::AttachMode::Join:
        case proto::AttachMode::Watch: return static_cast<proto::AttachMode>(request.mode);
    }
    // Not a mode this build knows. See the note at the call site: the safe
    // reading is the specified default.
    return proto::AttachMode::TakeOver;
}

bool Server::refuse_if_watching(Client& client, std::string_view context) {
    if (!client.watching()) return false;
    // Answered rather than dropped, and this is NOT the exception the input
    // path makes (see `handle`). Everything routed through here is a discrete
    // act a reader performed once — opening a terminal, renaming a session,
    // reporting an arrangement — so one refusal per act is one refusal, and a
    // client left waiting on a reply that never comes looks to a reader
    // exactly like a server that has hung.
    proto::Error error;
    error.code = static_cast<std::uint16_t>(proto::ErrorCode::ReadOnly);
    error.context = std::string(context);
    error.human = "this client is watching this session; stop watching to change it";
    send(client, error);
    return true;
}

Server::Client* Server::client_attached_to(SessionId id) {
    for (const std::unique_ptr<Client>& client : clients_)
        if (client->attached && !client->closing && client->session == id) return client.get();
    return nullptr;
}

const Session* Server::session_named(std::string_view name, SessionId except_id) const {
    if (name.empty()) return nullptr;
    for (const Session& session : sessions_)
        if (session.id != except_id && session.name == name) return &session;
    return nullptr;
}

std::string Server::next_session_name() const {
    // One past the largest number already taken, so a reader who renames
    // "session-2" to "build" does not get a second "session-2" tomorrow.
    long highest = 0;
    for (const Session& session : sessions_) {
        if (session.name.rfind("session-", 0) != 0) continue;
        const std::string tail = session.name.substr(8);
        if (tail.empty() || tail.find_first_not_of("0123456789") != std::string::npos) continue;
        highest = std::max(highest, std::strtol(tail.c_str(), nullptr, 10));
    }
    return "session-" + std::to_string(highest + 1);
}

Terminal& Server::open_terminal(SessionId session_id, const TerminalSpec& spec) {
    Terminal& terminal = terminals_.open(spec);
    // Zero means "the default one", the same as it does in an `Attach`: the
    // session already there, or a new one when this is the first terminal on a
    // server nobody has asked anything of yet. A terminal with no session to go
    // in would be a program nobody can ever see — not in a snapshot, not in a
    // delta, not closable — so one is made rather than the terminal orphaned.
    Session* session = session_for(session_id);
    if (session == nullptr && session_id == 0 && !sessions_.empty()) session = &sessions_.front();
    Session& home = session != nullptr ? *session : create_session({});
    home.terminals.push_back(terminal.id());
    home.last_focused = terminal.id();
    return terminal;
}

Session& Server::create_session(std::string name) {
    Session session;
    a_session_has_existed_ = true;
    session.id = next_session_++;
    session.name = name.empty() ? next_session_name() : std::move(name);
    sessions_.push_back(std::move(session));
    return sessions_.back();
}

void Server::begin_kill(Session& session, bool force, int grace_seconds) {
    // Asked, not killed: a program that is given a chance to save its work
    // usually takes it, and the ones that ignore SIGTERM are exactly the ones a
    // reader wants to decide about rather than have decided for them.
    for (const TerminalId id : session.terminals)
        if (Terminal* const terminal = terminals_.find(id)) terminal->request_termination();
    PendingKill pending;
    pending.session = session.id;
    pending.force = force;
    pending.deadline_nanos =
        clock_.now_nanos() + static_cast<std::int64_t>(std::max(0, grace_seconds)) * 1'000'000'000;
    kills_.push_back(pending);
    std::fprintf(stderr, "ckmux server: ending session %llu \"%s\" (%d s grace, %s)\n",
                 static_cast<unsigned long long>(session.id), session.name.c_str(), grace_seconds,
                 force ? "then kill" : "no kill");
}

void Server::advance_kills() {
    const std::int64_t now = clock_.now_nanos();
    for (std::size_t index = 0; index < kills_.size();) {
        PendingKill& pending = kills_[index];
        Session* const session = session_for(pending.session);
        if (session == nullptr) {
            kills_.erase(kills_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        // Whatever has already gone is gone: a terminal whose child exited on
        // the asking needs no further attention.
        std::vector<TerminalId> still_running;
        for (const TerminalId id : session->terminals) {
            Terminal* const terminal = terminals_.find(id);
            if (terminal != nullptr && terminal->live()) still_running.push_back(id);
        }
        const bool out_of_time = now >= pending.deadline_nanos;
        if (!still_running.empty() && !out_of_time) {
            ++index;
            continue;
        }
        if (!still_running.empty() && pending.force) {
            // The grace is up and the reader ticked the kill. The signal goes
            // now, and the reaping is watched on the tick like everything else:
            // `terminals_.close()` escalates too, but it escalates by WAITING —
            // up to three seconds per terminal, inside a loop that is meanwhile
            // serving nobody (M-S3). Ten terminals that ignore SIGTERM used to
            // freeze this server for half a minute.
            if (!pending.killed) {
                for (const TerminalId id : still_running)
                    if (Terminal* const terminal = terminals_.find(id)) terminal->request_kill();
                pending.killed = true;
                pending.kill_deadline_nanos = now + 2'000'000'000;
                ++index;
                continue;
            }
            if (now < pending.kill_deadline_nanos) {
                // Killed, not yet reaped. A child wedged in a write to its own
                // PTY cannot finish dying until the loop's next drain empties
                // it, which is precisely what carrying on gets it.
                ++index;
                continue;
            }
            // Two seconds after a SIGKILL and still not reaped is a child in a
            // state no signal can complete. `close()` is the last resort, and
            // it is bounded.
        }
        if (!still_running.empty() && !pending.force) {
            // The reader said not to kill. What is left keeps running, and so
            // does the session holding it — saying so is better than pretending
            // the session ended.
            std::fprintf(stderr,
                         "ckmux server: session %llu kept: %zu program(s) declined to quit and "
                         "kill was not enabled\n",
                         static_cast<unsigned long long>(session->id), still_running.size());
            kills_.erase(kills_.begin() + static_cast<std::ptrdiff_t>(index));
            broadcast_session_list();
            continue;
        }
        // Everything has gone, or everything that was left has been killed and
        // given its two seconds to be reaped. Closing is what actually removes a
        // terminal either way, and by now it costs nothing: ckVision reaped the
        // children on the loop's own drains, so there is nothing for `close()`
        // to wait for.
        for (const TerminalId id : session->terminals) {
            (void)terminals_.close(id);
            diffs_.forget(id);
        }
        const SessionId ended = session->id;
        kills_.erase(kills_.begin() + static_cast<std::ptrdiff_t>(index));
        forget_session(ended, proto::DetachReason::SessionKilled, "the session was ended");
    }
}

void Server::begin_close(TerminalId id, bool force, int grace_seconds) {
    Terminal* const terminal = terminals_.find(id);
    // Already gone is not an error: a client closing the window of a terminal
    // that just ended races this message, and neither side is wrong.
    if (terminal == nullptr) return;
    // Asked, not killed — the same courtesy begin_kill extends, for the same
    // reason: the programs that ignore SIGTERM are exactly the ones a reader
    // wants to decide about rather than have decided for them.
    terminal->request_termination();
    const std::int64_t deadline =
        clock_.now_nanos() + static_cast<std::int64_t>(std::max(0, grace_seconds)) * 1'000'000'000;
    for (PendingClose& pending : closes_) {
        if (pending.term != id) continue;
        pending.force = force;
        pending.deadline_nanos = deadline;
        // Replaced, escalation included: a reader who asks again has given this
        // program a fresh grace, and carrying "already killed" across it would
        // spend the new grace waiting to reap a signal the reader has just
        // superseded.
        pending.killed = false;
        pending.kill_deadline_nanos = 0;
        return;
    }
    closes_.push_back(PendingClose{id, deadline, force});
    std::fprintf(stderr, "ckmux server: closing terminal %llu (%d s grace, %s)\n",
                 static_cast<unsigned long long>(id), grace_seconds, force ? "then kill" : "no kill");
}

void Server::begin_kill_terminal(TerminalId id) {
    Terminal* const terminal = terminals_.find(id);
    // Already gone is not an error, exactly as in `begin_close`: a reader
    // killing a terminal whose program has just ended races this message, and
    // neither side is wrong. It is doubly true here — what they asked for is
    // that the terminal be dead, and it is.
    if (terminal == nullptr) return;
    // The signal now. `request_kill()` returns at once; `close()` is the one
    // that escalates by WAITING, and a server that waits has stopped being a
    // server (M-S3).
    terminal->request_kill();
    const std::int64_t now = clock_.now_nanos();
    // Entered into the close machinery at exactly the state it would have
    // reached had a forced close escalated — grace expired, kill sent — rather
    // than given a removal path of its own. Two paths that both remove a
    // terminal are two places for the paste slot, the diff state, the session's
    // terminal list, every watcher's `TermClosed` and the kill-empty-session
    // rule to be forgotten, and only one of them would get fixed next time.
    //
    // So the terminal goes on the tick that observes it died, which is the next
    // one: SIGKILL is not refusable. "Immediately" in the session model is a promise to
    // the READER — no grace, no negotiation, the window goes — and a tick is
    // 33 ms. What it cannot mean is reaping inside this handler.
    for (PendingClose& pending : closes_) {
        if (pending.term != id) continue;
        // A pending polite close is superseded rather than raced. Leaving it
        // would let its own deadline fire later against a terminal id this one
        // has already removed — harmless today because `advance_closes` checks,
        // and exactly the kind of thing that stops being harmless.
        pending.force = true;
        pending.deadline_nanos = now;
        pending.killed = true;
        pending.kill_deadline_nanos = now + 2'000'000'000;
        return;
    }
    closes_.push_back(PendingClose{id, now, /*force=*/true,
                                   /*kill_deadline_nanos=*/now + 2'000'000'000,
                                   /*killed=*/true});
    std::fprintf(stderr, "ckmux server: killing terminal %llu at a reader's request\n",
                 static_cast<unsigned long long>(id));
}

void Server::advance_closes() {
    const std::int64_t now = clock_.now_nanos();
    for (std::size_t index = 0; index < closes_.size();) {
        const PendingClose pending = closes_[index];
        Terminal* const terminal = terminals_.find(pending.term);
        if (terminal == nullptr) {
            // A session kill got there first; nothing left to watch.
            closes_.erase(closes_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if (terminal->live() && now < pending.deadline_nanos) {
            ++index;
            continue;
        }
        if (terminal->live() && pending.force) {
            // The grace is up with the kill ticked. The signal now, the reaping
            // on the tick — the same two stages a session kill goes through, and
            // for the same reason: `close()` escalates by waiting, and a loop
            // that waits has stopped being a server (M-S3).
            if (!pending.killed) {
                terminal->request_kill();
                closes_[index].killed = true;
                closes_[index].kill_deadline_nanos = now + 2'000'000'000;
                ++index;
                continue;
            }
            if (now < pending.kill_deadline_nanos) {
                ++index;
                continue;
            }
            // Two seconds after a SIGKILL and still not reaped is a child no
            // signal can finish. `close()` is the last resort, and it is
            // bounded.
        }
        if (terminal->live() && !pending.force) {
            // The reader said not to kill. The program keeps running, and its
            // terminal with it — saying so beats pretending it closed.
            std::fprintf(stderr,
                         "ckmux server: terminal %llu kept: its program declined to quit and "
                         "kill was not enabled\n",
                         static_cast<unsigned long long>(pending.term));
            closes_.erase(closes_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        // The child exited — on the asking, or on the kill that followed it.
        // Closing is what removes a terminal either way, and by now it costs
        // nothing: the reaping already happened on one of the loop's drains.
        closes_.erase(closes_.begin() + static_cast<std::ptrdiff_t>(index));
        remove_terminal(pending.term, /*exited=*/true);
    }
}

void Server::notice_exits() {
    for (const TerminalId id : terminals_.ids()) {
        Terminal* const terminal = terminals_.find(id);
        if (terminal == nullptr || terminal->exit_announced()) continue;
        // The flag ckVision grew for exactly this: `mark_exited` and
        // `mark_failed` used to change a terminal's life without setting any
        // damage at all, so a damage-gated transport — which this is — missed
        // every child that ended on its own while a reader was watching it. The
        // window stayed alive-looking with nothing behind it but a shell that
        // had gone (m-exit-msg). Read before `diffs_.flush`, which is what
        // clears it.
        if (!terminal->session().damage().lifecycle) continue;
        terminal->observe_exit();
        if (terminal->live()) continue;
        terminal->mark_exit_announced();
        const std::optional<int> status = terminal->exit_status();
        const bool failed = !status.has_value() || *status != 0;
        const bool hold = options_.settings.on_exit == ExitPolicy::Hold ||
                          (options_.settings.on_exit == ExitPolicy::HoldOnError && failed);
        if (!hold) {
            remove_terminal(id, /*exited=*/true);
            continue;
        }
        // Held: the window stays, with a banner, because a program that failed
        // has said something on that screen and closing it takes the evidence
        // away (the session model on-exit). So the terminal is NOT removed — its last
        // screen is what the banner is drawn over, and it is this server that
        // still holds it.
        proto::TermClosed closed;
        closed.term = id;
        closed.exited = 1;
        closed.exit_status = status.value_or(0);
        closed.hold = 1;
        if (Session* const home = session_holding(id))
            for_each_attached(home->id, [&](Client& watcher) { send(watcher, closed); });
    }
}

void Server::announce_terminal_news() {
    for (const TerminalId id : terminals_.ids()) {
        Terminal* const terminal = terminals_.find(id);
        if (terminal == nullptr) continue;
        Session* const home = session_holding(id);
        if (home == nullptr) continue;
        const ckv::term::TerminalDamage& damage = terminal->session().damage();

        // A screen that changed because a reader moved a window is not the
        // child saying anything. Read and cleared here whichever branch is
        // taken below, so it describes exactly one tick.
        const bool a_client_resized_it = terminal->take_client_resize();

        // The marks are kept whether or not anybody is watching — that is the
        // whole difference between a mark and an event, and a session with no
        // client is precisely when a reader most wants to be told afterwards
        // that something happened.
        // WHAT IS LEFT OF WP-41 HERE, measured rather than guessed.
        //
        // The reader-facing half is done: a client decides what it has missed
        // from the SERIALS, against its own focus, and answers them by looking
        // (`af6539a`). Nothing below is consulted for that. What remains is
        // that this branch still decides WHEN a `TermMeta` is sent, and it
        // decides it from `Session::last_focused` — one answer to a question
        // that has one per reader.
        //
        // It cannot simply be removed, and the obvious replacement was tried
        // and rejected by measurement rather than by argument. Lowering the
        // level on ANNOUNCE instead of on focus — so it means "since this was
        // last stated" — is focus-free and re-arms correctly, and it costs a
        // `TermMeta` per tick for every terminal that is writing at all:
        // `a_terminals_output_reaches_a_client_when_the_tick_is_due_and_not_before`
        // and `a_cli_client_is_not_sent_a_screenful_of_deltas` both fail, the
        // high-water case exceeds its mark, and the mirror's level flickers on
        // and off every tick. The stickiness of this level is load-bearing for
        // message economy; the focus clear is what re-arms it cheaply.
        //
        // So the residue is narrow and worth stating exactly: **activity on
        // whichever terminal the session last focused is not announced**, to
        // any reader, because this branch clears its marks. Bells are
        // unaffected — `count_bell()` announces regardless of focus. The real
        // fix is per-CLIENT announce state ("has THIS client been told the
        // current serials"), which is the same fan-out WP-44 is building for
        // `client_attached_to`, and belongs there rather than here.
        // The FACT, counted before anybody's focus is consulted (WP-41's
        // serial, the edge this NOTE was waiting for). A bell that rang rang,
        // whoever was looking; what each reader has answered is their own
        // business and is not knowable here. `wrote` is computed once and used
        // by both the count and the level below, so the two cannot come to
        // disagree about what output means.
        bool wrote_this_tick = false;
        if (!a_client_resized_it) {
            wrote_this_tick = damage.scrollback_pushed > 0;
            for (const ckv::term::TerminalDamage::RowSpan& row : damage.rows) {
                if (row.empty()) continue;
                wrote_this_tick = true;
                break;
            }
        }
        if (damage.bell) terminal->count_bell();
        if (wrote_this_tick) terminal->count_activity();

        if (home->last_focused == id) {
            // The reader is in this terminal, so there is nothing they have
            // missed. Cleared before the marks below are set, so output in the
            // focused terminal never marks itself.
            terminal->clear_marks();
        } else {
            if (damage.bell) terminal->mark_bell();
            // Activity is output on a terminal the reader is not in — the tmux
            // meaning, and the only one that makes a window marker worth
            // looking at. What counts as output is what a child put on the
            // screen: cells, or lines that scrolled into the history. A cursor
            // that moved and a mode that changed are the child doing something
            // ABOUT the screen rather than writing to it, and a program that
            // parks a cursor would otherwise mark itself forever.
            if (wrote_this_tick) terminal->mark_activity();
        }

        Client* const watcher = client_attached_to(home->id);
        if (watcher == nullptr) continue;

        // Either half is news: the marks, or the name the reader gave it. One
        // message states both, so a rename costs the same single `TermMeta` a
        // bell does rather than a message of its own.
        if (!terminal->marks_announced() || !terminal->custom_title_announced()) {
            proto::TermMeta meta;
            meta.bell_serial = terminal->bell_serial();
            meta.activity_serial = terminal->activity_serial();
            meta.term = id;
            const auto position = std::find(home->terminals.begin(), home->terminals.end(), id);
            meta.index = static_cast<std::uint16_t>(position - home->terminals.begin());
            // The title as well as the marks, because this message STATES a
            // title and a mirror applies what it is told. Through the same
            // clamp the delta's `Title` op uses, so the two producers cannot
            // disagree by a byte and leave a caption flipping once a tick.
            const ckv::term::TerminalStatus status = terminal->status();
            meta.title = std::string(clamp_utf8(status.title, proto::kMaxTitleBytes));
            // Already clamped where it was stored, and stated in full: this
            // message says what the caption IS, and a mirror applies what it
            // is told. An empty string is the reader having handed the name
            // back, which is a value and not an omission.
            meta.custom_title = terminal->custom_title();
            meta.flags = static_cast<std::uint8_t>(
                (terminal->bell_marked() ? static_cast<std::uint8_t>(proto::TermMetaFlag::Bell) : 0) |
                (terminal->activity_marked() ? static_cast<std::uint8_t>(proto::TermMetaFlag::Activity)
                                             : 0));
            send(*watcher, meta);
            terminal->note_marks_announced();
            terminal->note_custom_title_announced();
        }

        // The child asked to put text on the reader's clipboard. Gated on the
        // reader's own `[terminal] osc52`: the emulator refuses the write at
        // the child's door when that setting is off, and this is the same
        // answer said again on the way out — because what leaves this server is
        // the server's decision, and a producer that forwarded whatever the
        // emulator happened to hold would make the setting depend on a refusal
        // elsewhere staying total.
        //
        // The text lives in the snapshot rather than in `status()` (ckVision
        // keeps a serial there instead, so that reading a terminal every tick
        // costs nothing), so it is fetched only when the damage flag says there
        // is something new to fetch — which is what the flag is for.
        if (damage.clipboard && options_.settings.osc52) {
            ckv::core::TerminalSnapshotOptions wanted;
            wanted.include_scrollback = false;
            wanted.include_rasters = false;
            proto::ClipboardSet clipboard;
            clipboard.term = id;
            clipboard.text = terminal->snapshot(wanted).clipboard_text;
            if (!clipboard.text.empty()) send(*watcher, clipboard);
        }

        // The printer, in the message that already exists for it. While the
        // controller is on, the child's output is going to the printer and NOT
        // to the screen — the one printer fact a reader must be shown, or they
        // are watching a terminal that has apparently stopped responding.
        // Either the child's doing or ours: the emulator raises its own
        // printer damage, and a job discarded or a policy changed by a reader
        // moves the same numbers without the emulator knowing.
        if (damage.printer || terminal->take_printer_news()) {
            // Taken BEFORE the state is read, because taking is what moves the
            // numbers: a job that finished this tick is still counted in
            // `printer_jobs_ready` until it is drained, and a `PrintState`
            // sent first would tell a client about a job it is about to be
            // told about again by name.
            //
            // Drained even with nobody watching. The emulator hands each job
            // over exactly once and forgets it, so a server that only drained
            // while a client was attached would lose every job printed into a
            // detached session — which is precisely the session state a
            // multiplexer exists to keep (the session model).
            collect_and_announce_print_jobs(id, *terminal, watcher);

            const ckv::term::TerminalStatus status = terminal->status();
            const EffectivePrinterPolicy policy = effective_printer_policy(id);
            proto::PrintState printing;
            printing.term = id;
            printing.mode = wire_printer_mode(policy.mode);
            // Four states rather than two, because the frame button has four
            // (the interface spec) and only the server knows which one is true. `Sunk`
            // is the emulator having freed an over-limit job and gone on
            // scanning for `CSI 4 i` without storing anything — a reader whose
            // button said "capturing" through that would be told a document
            // was being kept that was not.
            printing.state = status.printer_sunk
                                 ? proto::PrinterState::Sunk
                                 : (status.printer_controller_active
                                        ? proto::PrinterState::Capturing
                                        : proto::PrinterState::Idle);
            printing.bytes = static_cast<std::uint32_t>(
                std::min<std::size_t>(status.printer_pending_bytes, 0xFFFFFFFFu));
            printing.jobs =
                static_cast<std::uint16_t>(std::min<std::size_t>(terminal->print_jobs().size(),
                                                                 0xFFFFu));
            send(*watcher, printing);
        }

        // And the newest complaint. One entry, not the ring: what a view paints
        // is the last one, and re-sending the whole ring on every change would
        // send a client entries it already holds.
        if (damage.diagnostics) {
            const std::span<const ckv::core::TerminalDiagnostic> complaints =
                terminal->session().diagnostics();
            if (!complaints.empty()) {
                proto::TermDiagnostic said;
                said.term = id;
                said.kind = wire_kind(complaints.back().kind);
                said.text =
                    std::string(clamp_utf8(complaints.back().message, proto::kMaxTitleBytes));
                send(*watcher, said);
            }
        }
    }
}

void Server::announce_layout(std::uint64_t force_session) {
    for (const Session& session : sessions_) {
        // Nothing to say unless a window actually moved. The gate is per
        // terminal and the message is per session, so one report that moves
        // three windows costs one message — the same edge trigger `TermMeta`
        // uses, and for the same reason: a client that reports an arrangement
        // on every frame of a drag must not cost a message per frame.
        //
        // It is also what stops the echo from becoming a loop, and the
        // argument changed with WP-44 — it used to rest on there being at
        // most one watcher per session (the session model's takeover), which is no
        // longer true when readers share one.
        //
        // What holds instead, and it is stronger: the gate above is an EDGE.
        // A layout is announced only when a terminal's own `layout_announced`
        // flag says something moved since the last statement, so a client
        // applying what it was told produces no new edge here even if it
        // reports afterwards — an unchanged arrangement is not a move. The
        // client half is belt to that brace: `ClientApp` reports on a
        // gesture's END, not on every settle, so being told where a window is
        // does not look like a reader putting it there.
        //
        // With two readers this stops being only an echo question and becomes
        // "whose arrangement wins", which is decided here rather than left to
        // discovery: **the reporting client's arrangement is authoritative and
        // the other is told.** Last report wins, which is the same rule one
        // reader has always had with themselves, and the alternative — merging
        // two readers' drags — is a conflict resolution nobody asked for on a
        // desktop they are both looking at.
        // …with one exception, and it is not a window moving at all. When the
        // DESKTOP resizes, every rect in the session means something different
        // than it did, and no terminal has necessarily moved — so the edge
        // above is down and, before `force_session`, the announcement was
        // skipped and nobody was told the coordinate space had changed. In a
        // session with no terminals yet it could never fire at all. Both
        // `SetDesktopSize` and a sole reader's `ClientResize` reach this.
        bool moved = session.id == force_session && force_session != 0;
        for (const TerminalId id : session.terminals) {
            if (moved) break;
            const Terminal* const terminal = terminals_.find(id);
            if (terminal == nullptr || terminal->layout_announced()) continue;
            moved = true;
            break;
        }
        if (!moved) continue;
        // No watcher, no announcement — and the flag stays down, so the
        // arrangement is stated to the next client to attach by the snapshot it
        // is given rather than lost. A session nobody is watching is exactly
        // when a layout most needs remembering.
        Client* const first = client_attached_to(session.id);
        if (first == nullptr) continue;

        // The arrangement is the SESSION's, so it is built once and every
        // reader is told the same one. Only `focused_term` differs per reader
        // — see below.
        //
        // The desktop is the session's, not any one reader's — see
        // `send_snapshot`. Two clients reading one arrangement have to be told
        // the same coordinate space or the rects mean two different things,
        // which is also why the degenerate fallback reads the FIRST attached
        // reader's screen rather than each reader's own: a session that has
        // ever been attached has a desktop (`handle_attach` sets it), so this
        // is only reachable for a client that declared no size at all, and
        // handing two readers two coordinate spaces would be worse than
        // handing both of them one arbitrary one.
        const ckv::Size desktop = session.desktop.width > 0 && session.desktop.height > 0
                                      ? session.desktop
                                      : first->desktop;
        std::vector<proto::LayoutEntry> entries;
        entries.reserve(session.terminals.size());
        for (const TerminalId id : session.terminals) {
            const Terminal* const terminal = terminals_.find(id);
            if (terminal == nullptr) continue;
            // Every window, not only the ones that moved: a `z_order` is a
            // position among the others, so a partial list is an arrangement a
            // mirror cannot apply.
            const WindowLayout& held = terminal->layout();
            entries.push_back(proto::LayoutEntry{
                id, held.rect, held.z_order, static_cast<std::uint8_t>(held.zoomed ? 1 : 0),
                held.tile});
        }

        for_each_attached(session.id, [&](Client& watcher) {
            proto::LayoutDelta layout;
            layout.session = session.id;
            // THIS watcher's focus, never the session's. A delta carrying
            // another reader's focus would move this one's cursor because
            // somebody else clicked — the marks defect, one field over. Unlike
            // the desktop, which every client must be told identically or a
            // rect means two different things, focus must NOT be shared: two
            // readers in one terminal is nonsense (WP-41).
            layout.focused_term = watcher.focused;
            layout.desktop_columns = static_cast<std::uint16_t>(std::max(0, desktop.width));
            layout.desktop_rows = static_cast<std::uint16_t>(std::max(0, desktop.height));
            layout.entries = entries;
            send(watcher, layout);
        });

        // And ONLY now, once every reader has been told (WP-48). The edge and
        // the send used to be the same loop, so a session with two readers
        // consumed the edge with a message one of them received: a window moved
        // by the second reader reached the first, and a window moved by the
        // first reached nobody at all. Clearing a flag on behalf of a client
        // that was never sent the arrangement is the general form of that bug,
        // and it is worse than not sending — the fact that anything moved is
        // gone, so no later pass can repair it.
        for (const TerminalId id : session.terminals)
            if (Terminal* const terminal = terminals_.find(id)) terminal->note_layout_announced();
    }
}

void Server::remove_terminal(TerminalId id, bool exited) {
    // Whatever was waiting to be pasted into it is acked and forgotten
    // (WP-42): the text has nowhere to go, and a client left waiting on an ack
    // holds the rest of its paste for ever.
    forget_paste_slot(id);
    Session* const home = session_holding(id);
    std::optional<int> status;
    if (Terminal* const terminal = terminals_.find(id)) status = terminal->exit_status();
    (void)terminals_.close(id);
    diffs_.forget(id);
    stats_baselines_.erase(id);
    proto::TermClosed closed;
    closed.term = id;
    closed.exited = exited ? 1 : 0;
    closed.exit_status = status.value_or(0);
    if (home == nullptr) return;
    home->terminals.erase(std::remove(home->terminals.begin(), home->terminals.end(), id),
                          home->terminals.end());
    for_each_attached(home->id, [&](Client& watcher) { send(watcher, closed); });
    // Every client's picker counts terminals, and the count just changed.
    broadcast_session_list();
    // The reader's kill-empty-session (the session model): a session emptied by an
    // explicit close has served its purpose, and the watcher falls back to
    // the picker rather than keeping an empty desktop nobody asked for.
    if (home->terminals.empty() && options_.settings.kill_empty_session)
        forget_session(home->id, proto::DetachReason::SessionKilled, "its last terminal closed");
}

void Server::apply_printer_policies() {
    for (const TerminalId id : terminals_.ids()) {
        Terminal* const terminal = terminals_.find(id);
        if (terminal == nullptr) continue;
        const EffectivePrinterPolicy policy = effective_printer_policy(id);
        // `ask` and `capture` are the same thing to an EMULATOR: capture the
        // bytes, tell the child the printer is ready. The whole difference is
        // consent — whether the reader has agreed to keep the result — which
        // is the host's question, not the terminal's (the terminal-emulation spec). So the
        // emulator is told Capture for both, and what separates them is which
        // bound applies and what this server does with the jobs afterwards.
        terminal->set_printer_policy(policy.mode == PrinterMode::Off
                                         ? ckv::term::TerminalPrinterPolicy::Deny
                                         : ckv::term::TerminalPrinterPolicy::Capture);
        // And the bound that goes with the mode. `ask` collects into the ask
        // cache, which is smaller on purpose: nothing has been consented to
        // yet, so the worst case a reader can be left holding for a terminal
        // they have not answered is that cache and nothing more (the terminal-emulation spec).
        // Once they say "capture", the larger spool applies because they asked
        // for the document.
        terminal->set_printer_spool_limit(policy.mode == PrinterMode::Ask ? policy.ask_cache
                                                                         : policy.spool_limit);
        terminal->mark_printer_news();
    }
}

void Server::collect_and_announce_print_jobs(TerminalId id, Terminal& terminal,
                                            Client* watcher) {
    // The clock, not wall time: every timestamp this server records comes from
    // the injected one, so a test can say when a job arrived (the conventions).
    for (const std::uint64_t job_id : terminal.collect_print_jobs(clock_.now_nanos())) {
        // No watcher, no message — and nothing lost. The job is in the spool,
        // and the snapshot the next client attaches with carries every job
        // this terminal holds. A session nobody is watching is exactly when a
        // capture most needs keeping.
        if (watcher == nullptr) continue;
        const Terminal::HeldJob* const job = terminal.print_job(job_id);
        if (job == nullptr) continue;
        proto::PrintJobAdded added;
        added.term = id;
        // Metadata only. A megabyte of captured text never rides the tick;
        // the payload moves when a reader actually opens the preview and the
        // client asks for it by name (`PrintJobFetch`).
        added.job = proto::PrintJobInfo{job->id, job->kind,
                                        static_cast<std::uint32_t>(
                                            std::min<std::size_t>(job->text.size(), 0xFFFFFFFFu)),
                                        job->lines, job->at};
        send(*watcher, added);
    }
}

Session* Server::session_holding(TerminalId id) {
    for (Session& session : sessions_)
        for (const TerminalId held : session.terminals)
            if (held == id) return &session;
    return nullptr;
}

const Session* Server::session_holding(TerminalId id) const {
    return const_cast<Server*>(this)->session_holding(id);
}

void Server::move_terminal(Client& client, const proto::MoveTerminal& request) {
    Session* source = session_holding(request.term);
    if (terminals_.find(request.term) == nullptr || source == nullptr) {
        proto::Error error;
        error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchTerminal);
        error.context = "MoveTerminal";
        error.human = "no such terminal";
        send(client, error);
        return;
    }
    const SessionId source_id = source->id;
    Session* target = nullptr;
    if (request.to_new_session != 0) {
        // `create_session` can grow `sessions_`, which moves every Session in
        // it — `source` is re-found rather than trusted across the call.
        target = &create_session({});
        source = session_for(source_id);
    } else {
        target = session_for(request.destination_session);
        if (target == nullptr) {
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchSession);
            error.context = "MoveTerminal";
            error.human = "no such session";
            send(client, error);
            return;
        }
    }
    if (target == source) return;  // The session model: target = source is a no-op
    // A move is the reader deciding to keep the program: a close that was
    // pending against this terminal is off.
    closes_.erase(std::remove_if(closes_.begin(), closes_.end(),
                                 [&request](const PendingClose& pending) {
                                     return pending.term == request.term;
                                 }),
                  closes_.end());
    // Reparenting is editing the two id lists. The PTY, the emulator and the
    // scrollback never lived anywhere else, so the child does not notice
    // (the session model).
    source->terminals.erase(
        std::remove(source->terminals.begin(), source->terminals.end(), request.term),
        source->terminals.end());
    target->terminals.push_back(request.term);
    // Its window's place does not come with it. A `z_order` is a position among
    // the windows of ONE session, and carried into a stack this terminal has
    // never been beside it would claim a place nobody put it in — so the
    // terminal arrives in its new session as an unplaced one, which is exactly
    // what a terminal opened there would be.
    if (Terminal* const moved = terminals_.find(request.term)) (void)moved->clear_layout();
    // The source's watcher sees the window leave without a death notice:
    // exited = 0 says nothing exited, so no banner and no invented status.
    proto::TermClosed left;
    left.term = request.term;
    left.exited = 0;
    for_each_attached(source->id, [&](Client& watcher) { send(watcher, left); });
    // The target's watcher learns from a fresh snapshot. A terminal it has
    // never seen has no delta baseline — the same problem as a client that
    // fell behind, healed the same way.
    for_each_attached(target->id, [](Client& watcher) { watcher.dirty_snapshot = true; });
    broadcast_session_list();
    std::fprintf(stderr, "ckmux server: terminal %llu moved from session %llu to session %llu\n",
                 static_cast<unsigned long long>(request.term),
                 static_cast<unsigned long long>(source_id),
                 static_cast<unsigned long long>(target->id));
    // An emptied source is subject to the same rule as any emptied session.
    if (source->terminals.empty() && options_.settings.kill_empty_session)
        forget_session(source->id, proto::DetachReason::SessionKilled,
                       "its last terminal moved away");
}

void Server::forget_session(SessionId id, proto::DetachReason reason, std::string text) {
    for (const std::unique_ptr<Client>& client : clients_)
        if (client->attached && client->session == id) detach(*client, reason, text);
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [id](const Session& session) { return session.id == id; }),
                    sessions_.end());
    broadcast_session_list();
    // The last session was the reason this process existed. A server with none
    // left is holding a socket, a lock and a process table entry on behalf of
    // nothing — so it goes, and the next reader who wants a session gets a
    // fresh one started for them (the session model: the server is started on demand).
    //
    // Only once a session HAS existed: a server that has just been started has
    // no sessions yet, and exiting there would race the client that started it.
    if (sessions_.empty() && a_session_has_existed_) {
        std::fprintf(stderr, "ckmux server: the last session ended; stopping\n");
        stop();
    }
}

void Server::send_session_list(Client& client) {
    proto::SessionList list;
    list.sessions.reserve(sessions_.size());
    for (const Session& session : sessions_) {
        proto::SessionInfo info;
        info.id = session.id;
        info.name = session.name;
        info.terminals = static_cast<std::uint16_t>(session.terminals.size());
        // Attached means somebody ELSE is watching it, from this client's point
        // of view — but the flag is about the session, so it says whether any
        // client holds it. The client that holds it knows which one it is.
        // A COUNT now, not a flag: the session model has always said 0..n, and with
        // simultaneous attach (WP-44) the picker can say how many readers a
        // session has rather than merely that it is busy.
        info.attached = static_cast<std::uint8_t>(std::min<std::size_t>(attached_count(session.id), 255)); 
        list.sessions.push_back(std::move(info));
    }
    send(client, list);
}

void Server::broadcast_session_list() {
    // Everybody, CLI clients included — this is the ANSWER to a request. A
    // `ckmux new` is a CLI client whose whole output is the list this produces,
    // and the one time it was narrowed to UI clients that command hung and
    // exited non-zero.
    //
    // Whatever the tick was going to say has just been said, and better: this
    // list is current. Clearing here keeps a pending flag from putting a
    // second, identical list behind an immediate one.
    session_list_dirty_ = false;
    for (const std::unique_ptr<Client>& watcher : clients_)
        if (watcher->greeted) send_session_list(*watcher);
}

void Server::flush_reader_counts() {
    // The unsolicited half (WP-48): an attach or a detach moved a count and
    // nobody asked to be told. Two narrowings that the answering path above
    // must NOT have, because they are about a push rather than a reply.
    //
    // UI clients only. A CLI utility asked one question and is waiting for its
    // answer; a push tells it nothing it can use, and `ckmux ls` gets its list
    // by asking, on the direct path.
    //
    // And not to a reader whose queue is backed up. A `SessionList` is pure
    // current state, so a later one is strictly better than this one and
    // deferring costs nothing — while sending it costs a great deal. Queueing
    // any ordinary frame over the high-water mark sets `dirty_snapshot`, so a
    // list landing on a client still draining its ATTACH SNAPSHOT asks for a
    // second copy of the four megabytes it is in the middle of receiving. That
    // is the M-S5 re-snapshot loop reached from a new direction: WP-48 made an
    // attach produce a frame one tick later, which is exactly when the client
    // that just attached is least able to take one.
    //
    // Deferred rather than dropped — the flag stays up for anyone skipped — so
    // a reader who was busy still gets the count, a tick or two behind.
    bool everybody_was_told = true;
    for (const std::unique_ptr<Client>& watcher : clients_) {
        if (!watcher->greeted || watcher->kind != proto::ClientKind::Ui) continue;
        if (watcher->stream.over_high_water() || watcher->dirty_snapshot) {
            everybody_was_told = false;
            continue;
        }
        send_session_list(*watcher);
    }
    session_list_dirty_ = !everybody_was_told;
}

void Server::attach(Client& client, const proto::Attach& request) {
    // Which session, and it must exist: a client attaches to something it has
    // seen in a list, and inventing one here would hide a client that asked for
    // a session somebody killed a moment ago.
    Session* session = session_for(request.session);
    if (session == nullptr && request.session == 0) {
        // Zero is "the default session": the one already there, or a new one on
        // a server that has none. A client that asks for a session BY id and
        // does not get it is told so — that one may have been killed a moment
        // ago, and inventing a replacement would hide it — but a client that
        // asked for whatever is there is asking to work, and a server with
        // nothing to attach to yet is exactly the server that should make one.
        session = sessions_.empty() ? &create_session({}) : &sessions_.front();
    }
    if (session == nullptr) {
        proto::Error error;
        error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchSession);
        error.context = "Attach";
        error.human = "no such session";
        send(client, error);
        return;
    }
    // The session's desktop, and what this client's own size may do to it
    // (WP-40). A first attach sets it, because a session created by `ckmux
    // new` has no screen of its own to take one from; after that, `[general]
    // desktop-size` decides, and its default is to leave it alone. Reflowing a
    // session's windows to fit whoever just arrived SIGWINCHes every child in
    // it — for every reader watching, not only the one who attached.
    const ckv::Size arriving{static_cast<int>(request.columns), static_cast<int>(request.rows)};
    if (arriving.width > 0 && arriving.height > 0) {
        if (session->desktop.width <= 0 || session->desktop.height <= 0) {
            session->desktop = arriving;
        } else {
            switch (options_.settings.desktop_size) {
                case DesktopSizePolicy::Fixed:
                    break;
                case DesktopSizePolicy::FitSmallest:
                    // Sticky: it shrinks to a newcomer and does not grow back
                    // when they leave. A session whose geometry oscillates
                    // with people's attach cycles is worse than one that is
                    // merely too small.
                    session->desktop = ckv::Size{std::min(session->desktop.width, arriving.width),
                                                 std::min(session->desktop.height, arriving.height)};
                    break;
                case DesktopSizePolicy::FitLatest:
                    session->desktop = arriving;
                    break;
            }
        }
    }
    // Granted immediately, and taken from whoever held THAT session (the session model).
    // A reader whose laptop slept cannot be kept out by the client that is
    // nominally still holding their session — which is why the previous holder
    // is told rather than asked.
    //
    // …unless this client asked to SHARE it (WP-44, WP-49). The mode is the
    // whole of the opt-in: at `TakeOver` the contract is exactly what it was,
    // and a reader whose laptop slept still cannot be kept out by the client
    // nominally holding their session. At `Join` or `Watch` the readers already
    // there stay, and this one joins them.
    //
    // A mode this build does not know is read as `TakeOver` — the default and
    // the specified contract — rather than refused: a newer client asking for
    // something this server cannot do must not be silently granted a share it
    // did not get, and the safe reading of "I do not understand you" is the one
    // that leaves no reader believing they are watching when they are typing.
    const proto::AttachMode mode = attach_mode_of(request);
    if (mode == proto::AttachMode::TakeOver) {
        if (Client* previous = client_attached_to(session->id);
            previous != nullptr && previous != &client) {
            detach(*previous, proto::DetachReason::Takeover,
                   "taken over by another client on this machine");
        }
    }
    // Said out loud, because "which client is watching which session" is the
    // first question anybody debugging a multiplexer asks, and a server log
    // that answers it costs one line.
    std::fprintf(stderr, "ckmux server: client %llu attached to session %llu \"%s\"\n",
                 static_cast<unsigned long long>(client.id),
                 static_cast<unsigned long long>(session->id), session->name.c_str());
    client.attached = true;
    client.session = session->id;
    // And what this reader may do while they are here (WP-49). Set on every
    // attach, including a resnapshot, so a heal cannot quietly promote a
    // watcher: the mode travels on the message that establishes the
    // attachment, and there is no other way to be attached.
    client.mode = mode;
    client.dirty_snapshot = false;
    client.host_sixel = request.host_sixel != 0;
    if (request.columns > 0 && request.rows > 0) {
        client.desktop = ckv::Size{request.columns, request.rows};
        client.cell_pixels = cell_metric_of(request);
    }
    send_snapshot(client);
    // Every picker on this machine now shows a stale reader count, including
    // this client's own (WP-48).
    //
    // One flush covers BOTH sessions when a reader switched: `attach` does not
    // detach a client from the session it was in — it reassigns
    // `client.session` — so the old session quietly loses a reader here, and
    // `send_session_list` recounts every session from scratch.
    session_list_dirty_ = true;
}

void Server::handle_set_reader_mode(Client& client, const proto::SetReaderMode& wish) {
    Session* const session = session_of(client);
    if (session == nullptr) {
        proto::Error error;
        error.code = static_cast<std::uint16_t>(proto::ErrorCode::NoSuchSession);
        error.context = "SetReaderMode";
        error.human = "attach to a session before changing what its readers may do";
        send(client, error);
        return;
    }

    const auto scope = static_cast<proto::ReaderScope>(wish.scope);
    if (scope != proto::ReaderScope::Me && scope != proto::ReaderScope::Others) {
        proto::Error error;
        error.code = static_cast<std::uint16_t>(proto::ErrorCode::InvalidRequest);
        error.context = "SetReaderMode";
        error.human = "a reader mode is aimed either at you or at the others";
        send(client, error);
        return;
    }
    // The mode is validated rather than defaulted, unlike `Attach.mode`. The
    // two cases differ: an attach with a mode this build cannot read still has
    // to attach somehow, and the safe reading is the specified default. Nothing
    // has to happen here, so an unreadable mode is simply refused and no reader
    // is left believing something that is not true.
    const auto mode = static_cast<proto::AttachMode>(wish.mode);
    if (mode != proto::AttachMode::TakeOver && mode != proto::AttachMode::Join &&
        mode != proto::AttachMode::Watch) {
        proto::Error error;
        error.code = static_cast<std::uint16_t>(proto::ErrorCode::InvalidRequest);
        error.context = "SetReaderMode";
        error.human = "no such reader mode";
        send(client, error);
        return;
    }

    if (scope == proto::ReaderScope::Me) {
        if (mode == proto::AttachMode::TakeOver) {
            // Attaching is how a session is taken, and it is where the eviction
            // lives. A scope of *me* cannot evict me, and reading this as "take
            // it from the others" would make the most destructive act on this
            // wire reachable by a plausible typo in the scope byte.
            proto::Error error;
            error.code = static_cast<std::uint16_t>(proto::ErrorCode::InvalidRequest);
            error.context = "SetReaderMode";
            error.human = "attaching is how a session is taken; aim this at the other readers";
            send(client, error);
            return;
        }
        // No `ReaderMode` back: this reader just asked for it and telling them
        // is a round trip that teaches nothing (the protocol spec).
        client.mode = mode;
        return;
    }

    // …and at everybody else. Any attached reader may do this to any other:
    // there is no owner, because the socket is uid-private and an owner among
    // readers would be a permission system with one user in it — see the session model,
    // where the symmetry is a decision rather than an omission.
    if (mode == proto::AttachMode::TakeOver) {
        // "I want this session to myself." The eviction is the one the attach
        // path already performs, which is why this needs no verb of its own.
        std::vector<ClientId> going;
        for_each_attached(session->id, [&](Client& other) {
            if (other.id != client.id) going.push_back(other.id);
        });
        // Collected first, then acted on: `detach` broadcasts a session list,
        // which walks `clients_` while `for_each_attached` is walking it too.
        for (const ClientId id : going)
            if (Client* const other = client_with_id(id))
                detach(*other, proto::DetachReason::Takeover,
                       "another reader took this session over");
        return;
    }

    for_each_attached(session->id, [&](Client& other) {
        if (other.id == client.id) return;
        // Unchanged readers are not told: a `ReaderMode` that states what a
        // client already believes is a toast about nothing, and this fires
        // whenever a reader unticks a box they had never ticked.
        if (other.mode == mode) return;
        other.mode = mode;
        // Somebody ELSE changed what this reader may do, so this one IS told.
        proto::ReaderMode told;
        told.mode = static_cast<std::uint8_t>(mode);
        send(other, told);
    });
}

void Server::detach(Client& client, proto::DetachReason reason, std::string text) {
    if (!client.attached) return;
    client.attached = false;
    client.dirty_snapshot = false;
    // A picture debt is owed to an attachment, not to a connection.
    client.owed_images.clear();
    client.owed_sent = 0;
    client.draining_heal = false;
    proto::Detached detached;
    detached.reason = reason;
    detached.text = std::move(text);
    send(client, detached);
    // And the count this session reports just fell (WP-48) — noted for the tick
    // rather than sent from here, and the difference is not efficiency.
    //
    // `forget_session` detaches every reader of a session and only THEN erases
    // it, so a list sent from inside this function describes a world halfway
    // through a change: the session is gone as far as its readers are
    // concerned and still present in `sessions_`. Sending it published exactly
    // that — and when the session being ended was the last one, the server
    // stopped before the corrected list could follow, leaving every picker
    // showing a session that no longer existed.
    //
    // Deferring to the tick makes that unrepresentable rather than fixed:
    // there is no moment mid-mutation at which anything is published.
    session_list_dirty_ = true;
    // The terminals are untouched, which is the whole promise: a detach is a
    // client going away, and the programs it was watching do not care.
}

void Server::send_snapshot(Client& client) {
    // Cleared HERE, before anything is sent, and not at the end: a `send` below
    // that goes over the high-water mark sets this flag, and clearing it
    // afterwards threw away the one thing saying that the client which had just
    // been handed a snapshot did not get all of it. That is half of the
    // re-snapshot loop (M-S5); the shared `ready_for_screen` gate is the other.
    client.dirty_snapshot = false;
    proto::Snapshot snapshot;
    // The SESSION's desktop (WP-40), not this client's screen echoed back at
    // it — a client already knows how big its own terminal is, and what it
    // needs from the server is the coordinate space the window rects below are
    // expressed in. Falls back to the client's own size only for a session
    // nobody has attached to yet, which cannot happen on this path but keeps
    // the field from ever being zero.
    const Session* const home = session_for(client.session);
    const ckv::Size desktop = home != nullptr && home->desktop.width > 0 && home->desktop.height > 0
                                  ? home->desktop
                                  : client.desktop;
    snapshot.desktop_columns = static_cast<std::uint16_t>(std::max(0, desktop.width));
    snapshot.desktop_rows = static_cast<std::uint16_t>(std::max(0, desktop.height));
    const Session* const session = session_for(client.session);
    // Where this reader is — or, for one that has just arrived with no answer
    // of its own, where the last reader was. That second reading is what
    // `focused_term` has always meant on the wire: what a newcomer is told,
    // once. A returning reader landing where they left off is behaviour a
    // single client already relies on, and it survives here.
    snapshot.focused_term =
        client.focused != 0 ? client.focused : (session != nullptr ? session->last_focused : 0);
    const std::vector<TerminalId> held =
        session != nullptr ? session->terminals : std::vector<TerminalId>{};

    // Pass one: every terminal WITHOUT its history. A grid is what a terminal
    // IS and has to go whole; a history is what it remembers, and what is left
    // of the budget once the grids are measured is what the histories share.
    std::uint16_t index = 0;
    for (const TerminalId id : held) {
        Terminal* terminal = terminals_.find(id);
        if (terminal == nullptr) continue;
        // Taking a snapshot restarts that terminal's sequence, so the first
        // delta a client sees afterwards is 1 — which is what lets it check
        // continuity from its first delta rather than take it on trust (WP-4b).
        proto::TerminalState state = diffs_.snapshot(id, terminal->session());
        state.index = index++;
        // The reader's printer policy, which is the server's to state: the
        // differ reads a terminal and this is configuration. Said here so that
        // a snapshot and the `PrintState` that follows it agree about the mode
        // rather than one of them defaulting.
        const EffectivePrinterPolicy printer = effective_printer_policy(id);
        state.printer_mode = wire_printer_mode(printer.mode);
        // The scope the MODE came from, so a reattaching client can say "from:
        // session" in Printer Settings without asking again. The interface spec requires
        // the dialog to show where the effective value came from; a client
        // that only knew the value would have to guess, and a per-session
        // override a reader forgot is exactly what must not be invisible.
        state.printer_scope = printer.mode_from;
        // And the jobs this terminal is holding, as metadata. A reattaching
        // reader finds their captures waiting — which is the whole reason the
        // spool is session state rather than the client's (the session model).
        state.print_jobs.clear();
        for (const Terminal::HeldJob& job : terminal->print_jobs())
            state.print_jobs.push_back(proto::PrintJobInfo{
                job.id, job.kind,
                static_cast<std::uint32_t>(std::min<std::size_t>(job.text.size(), 0xFFFFFFFFu)),
                job.lines, job.at});
        {
            const ckv::term::TerminalStatus printer_status = terminal->status();
            state.printer_state = printer_status.printer_sunk
                                      ? proto::PrinterState::Sunk
                                      : (printer_status.printer_controller_active
                                             ? proto::PrinterState::Capturing
                                             : proto::PrinterState::Idle);
            state.printer_bytes = static_cast<std::uint32_t>(
                std::min<std::size_t>(printer_status.printer_pending_bytes, 0xFFFFFFFFu));
        }
        // Where the reader last had this window. The layout half of what
        // survives a detach, said with everything else a reattaching client
        // gets rather than through a message of its own — the arrangement is
        // part of the state, not news about it (the session model: window layout is
        // session state, owned by the server). A terminal whose place has never
        // been reported states a zero-size rect, which is the wire's way of
        // saying "nobody has placed this yet".
        const WindowLayout& placed = terminal->layout();
        state.rect = placed.rect;
        state.z_order = placed.z_order;
        state.zoomed = static_cast<std::uint8_t>(placed.zoomed ? 1 : 0);
        // And the share of a filled tiling it held, which is the one number a
        // client reattaching at a DIFFERENT size can still use (WP-30): a 50/50
        // split is 50/50 on any desktop, where the rect beside it is only true
        // of the one it was measured on.
        state.tile = placed.tile;
        // A snapshot IS the announcement, so the tick that follows it does not
        // restate the same arrangement in a `LayoutDelta`.
        terminal->note_layout_announced();
        state.flags = static_cast<std::uint8_t>(
            (terminal->bell_marked() ? static_cast<std::uint8_t>(proto::TermMetaFlag::Bell) : 0) |
            (terminal->activity_marked() ? static_cast<std::uint8_t>(proto::TermMetaFlag::Activity)
                                         : 0));
        // A snapshot IS an announcement of the marks, so the tick that follows
        // it does not restate them: a `TermMeta` is sent when the pair changes,
        // and this has just said what the pair is.
        terminal->note_marks_announced();
        // And the name the reader gave this terminal, which is the other half
        // of what survives a detach. Here rather than in a message of its own
        // for the reason the layout is: a name is part of the state a client
        // arrives into, not news about it — and a client that had to wait for
        // a `TermMeta` to learn it would show the child's title first and
        // correct itself a tick later, in front of the reader.
        state.custom_title = terminal->custom_title();
        terminal->note_custom_title_announced();
        // The child's own life, which no delta can carry because it is not a
        // fact about a screen. A terminal whose child has ended and which is
        // STILL IN A SESSION is a held one by construction — `notice_exits`
        // removes the others — so `hold` is read off that rather than derived
        // from the policy a second time. Without these three, a reattach found
        // no field for "the child exited" and the mirror had to preserve what a
        // `TermClosed` had told it, which meant a server that genuinely
        // restarted a terminal had no way to say so (M-R4).
        if (!terminal->live()) {
            state.exited = 1;
            state.exit_status = terminal->exit_status().value_or(0);
            state.hold = 1;
        }
        // The counts, stated with every terminal a snapshot carries (WP-41).
        // This is the PRODUCER for the two fields: without it they encode,
        // decode and round-trip perfectly while always being zero — which is
        // the shape `ef8de87` found in `Attach.share`, and the reason a
        // round-trip test is not evidence that a field is reachable.
        state.bell_serial = terminal->bell_serial();
        state.activity_serial = terminal->activity_serial();
        snapshot.terminals.push_back(std::move(state));
    }
    // The eight bytes are `Attached::session`, which the snapshot's own measure
    // does not include: what is being held to the budget is the whole payload,
    // because that is what the decoder at the far end measures.
    const std::size_t spent = 8 + proto::encoded_size(snapshot);
    if (spent > proto::kSnapshotPayloadBudget) {
        // Unreachable with the geometry ceiling in place — a thousand by a
        // thousand is a megabyte of cells, and a session would need several to
        // get here — and said out loud rather than quietly truncated: a reader
        // whose session cannot be sent needs to be told that, not handed a
        // desktop with terminals missing from it.
        std::fprintf(stderr,
                     "ckmux server: session %llu is too large to send (%zu bytes of screens "
                     "alone)\n",
                     static_cast<unsigned long long>(client.session), spent);
        detach(client, proto::DetachReason::SessionKilled,
               "this session is too large for one snapshot; the screens alone are past the "
               "protocol's limit");
        return;
    }

    // Pass two: the histories. Each terminal takes what it can from an equal
    // share of what is left, and what it does not use passes to the next — so
    // one terminal with a long history cannot starve the others, and terminals
    // with none do not waste their share.
    std::size_t remaining = proto::kSnapshotPayloadBudget - spent;
    std::size_t left = snapshot.terminals.size();
    for (proto::TerminalState& state : snapshot.terminals) {
        Terminal* terminal = terminals_.find(state.term);
        const std::size_t share = remaining / std::max<std::size_t>(1, left);
        if (terminal != nullptr)
            remaining -= diffs_.fill_history(state.term, state, terminal->session(), share);
        --left;
    }

    proto::Attached attached;
    attached.session = client.session;
    attached.snapshot = std::move(snapshot);
    send(client, attached);
    // The pictures, after the grid (WP-16): the believed set, exactly what the
    // clients already watching hold, so every mirror agrees until the next
    // tick moves them all together. OWED rather than queued here: one busy
    // session's pictures are tens of megabytes at HiDPI cell sizes, and a
    // heal that queued them whole crossed the backlog mark by itself — any
    // news during the drain re-marked the client dirty and the next heal
    // re-sent everything, forever. flush_tick drips these as the queue has
    // room; the mirror keeps showing the pixels it already holds under the
    // same wire ids (TerminalState::images), so the drip is invisible where
    // the loop was unmissable. A newer snapshot supersedes the whole debt.
    client.owed_images.clear();
    client.owed_sent = 0;
    for (const TerminalId id : held)
        for (proto::Message& op : diffs_.attach_images(id)) owe_image(client, std::move(op));
    client.draining_heal = true;
}

void Server::send(Client& client, const proto::Message& message) {
    if (!client.stream.open()) return;
    bool oversize = false;
    const std::string frame = proto::encode(message, &oversize);
    if (oversize) {
        // A frame past the cap is not a large frame: it is one the peer's
        // decoder refuses, so putting it on the wire loses both the message and
        // the connection. Learning it here means the connection survives.
        std::fprintf(stderr,
                     "ckmux server: refusing to send %s to client %llu: it is past the protocol's "
                     "cap for its kind\n",
                     name_of(proto::type_of(message)),
                     static_cast<unsigned long long>(client.id));
        if (proto::type_of(message) == proto::MessageType::Attached) {
            // A snapshot that will not fit cannot be answered with a snapshot,
            // so this is the one message whose refusal ends the connection
            // rather than queueing another attempt behind it.
            detach(client, proto::DetachReason::SessionKilled, "this session is too large to send");
        } else {
            // Everything else is a client that has missed something, which is
            // the state a fresh snapshot exists to repair.
            client.dirty_snapshot = true;
        }
        return;
    }
    if (client.stream.queued() + frame.size() > platform::Stream::kHardLimitBytes) {
        // Not slow: not reading. Thirty-two megabytes is eight snapshots and
        // eight high-water marks past anything a working connection queues, so
        // what is on the other end of this one is a process that has stopped
        // taking bytes — and the memory it is costing is this server's, while
        // the terminals it was watching are not its to hold up. A reconnect is
        // a path the protocol already defines; an unbounded queue is not.
        //
        // Nothing is sent to say so, because a connection with thirty-two
        // megabytes waiting is a connection nothing further will be read from.
        // The client meets EOF, which it already treats as a lost connection,
        // and the fact lives in the server's log where it can be acted on.
        drop(client, "fell too far behind to be worth queueing for");
        return;
    }
    if (!client.stream.send(frame)) {
        // Over the high-water mark. The bytes are queued — dropping half a frame
        // would desynchronise the stream — but nothing more should be queued
        // until it drains, and the client will need a fresh snapshot when it
        // does (the protocol spec).
        //
        // Except when the frame that went over the mark IS the fresh snapshot.
        // A repair does not mark what it repaired as broken: the snapshot is
        // queued in full and will arrive, so there is nothing to re-send, and
        // asking for another one would queue a second copy of the same four
        // megabytes behind the first. The budget a snapshot is held to is the
        // high-water mark itself (proto::kSnapshotPayloadBudget), so a session
        // near it trips this every single time — which is how the M-S5 loop
        // came back at four-megabyte granularity instead of per tick. What a
        // client that really has missed something needs is said by the delta
        // gate, which marks it when it actually skips a delta.
        if (proto::type_of(message) != proto::MessageType::Attached)
            client.dirty_snapshot = true;
    }
}

bool Server::picture_wanted_now(TerminalId term, std::uint64_t wire_id) {
    // Asked per redrawn picture per tick, so the walk is over clients — of
    // which there are a handful — rather than over anything a child controls.
    const Session* const home = session_holding(term);
    if (home == nullptr) return false;
    for (const std::unique_ptr<Client>& client : clients_) {
        // The same filter the tick's own broadcast applies, in the same order,
        // because a picture is worth building exactly when the broadcast would
        // hand it to somebody: a CLI connection has no screen, an unattached
        // one has not asked for one, and a client watching another session is
        // not shown this terminal's pixels.
        if (!client->attached || client->closing) continue;
        if (client->kind != proto::ClientKind::Ui) continue;
        if (client->session != home->id) continue;
        // Waiting to be healed: the broadcast skips it, and what it will be
        // given is the BELIEVED set at its snapshot — which is precisely the
        // set this frame would not have joined.
        if (client->dirty_snapshot) continue;
        // Already owes the frame before this one. Its debt supersedes on
        // arrival (`owe_image`), so a payload built now is built to be thrown
        // away; the frame this client actually wants is the one current when
        // its debt has drained, which is a later tick's business.
        bool owes_this_picture = false;
        for (std::size_t index = client->owed_sent; index < client->owed_images.size(); ++index) {
            const Client::OwedImage& owed = client->owed_images[index];
            if (owed.image == wire_id && owed.begins) {
                owes_this_picture = true;
                break;
            }
        }
        if (owes_this_picture) continue;
        return true;
    }
    return false;
}

void Server::owe_image(Client& client, proto::Message op) {
    const std::uint64_t subject = image_op_subject(op);
    // A picture whose pixels are about to travel supersedes the frame of the
    // same picture that has not started travelling yet, and so does a Remove
    // that says the picture is gone: what is in the debt is a statement about
    // a picture NOW, and two statements about the same picture are not two
    // things a mirror has to be told. Told both, it paints the stale one and
    // then, a drip later, the current one — which is precisely what a reader
    // watching ckvision_spin through a slow host terminal saw, as a cube that
    // went on turning at the position its window had left seconds ago.
    //
    // Only what has not STARTED going out. A payload half-way across the
    // socket has to finish: the client is assembling it by sequence number,
    // and chunks that stop arriving leave it holding a picture whose end
    // never comes. So the search is for this id's next group OPENING among
    // the unsent ops — a group already under way has its ImageAddBegin
    // behind `owed_sent`, is therefore not found, and is left to complete.
    const bool supersedes = std::holds_alternative<proto::ImageAddBegin>(op) ||
                            std::holds_alternative<proto::ImageRemove>(op);
    if (supersedes && subject != 0) {
        const auto unsent =
            client.owed_images.begin() + static_cast<std::ptrdiff_t>(client.owed_sent);
        const auto stale = std::find_if(unsent, client.owed_images.end(),
                                        [subject](const Client::OwedImage& owed) {
                                            return owed.image == subject && owed.begins;
                                        });
        if (stale != client.owed_images.end())
            client.owed_images.erase(
                std::remove_if(stale, client.owed_images.end(),
                               [subject](const Client::OwedImage& owed) {
                                   return owed.image == subject;
                               }),
                client.owed_images.end());
    }
    // A placement supersedes an unsent placement of the same picture for the
    // same reason, and it is the half a reader notices first: dragging a
    // window moves its picture without redrawing a pixel of it, so a debt that
    // kept every move replayed the drag — the picture sliding through every
    // position the window had passed through, arriving where the reader put it
    // seconds after they let go. A Place is a whole message rather than part
    // of an assembly, so any unsent earlier one may go; the newest states
    // where the picture is, and the pixels it swaps in are whatever has
    // finished arriving under that id.
    if (std::holds_alternative<proto::ImagePlace>(op) && subject != 0) {
        const auto unsent =
            client.owed_images.begin() + static_cast<std::ptrdiff_t>(client.owed_sent);
        client.owed_images.erase(
            std::remove_if(unsent, client.owed_images.end(),
                           [subject](const Client::OwedImage& owed) {
                               return owed.image == subject &&
                                      std::holds_alternative<proto::ImagePlace>(owed.message);
                           }),
            client.owed_images.end());
    }
    Client::OwedImage owed;
    owed.image = subject;
    owed.begins = std::holds_alternative<proto::ImageAddBegin>(op);
    owed.message = std::move(op);
    client.owed_images.push_back(std::move(owed));
}

Server::Client* Server::client_with_id(ClientId id) {
    for (const std::unique_ptr<Client>& client : clients_)
        if (client->id == id && client->stream.open()) return client.get();
    return nullptr;
}

void Server::write_paste_chunk(Terminal& terminal, Client& client, const proto::PasteChunk& chunk) {
    terminal.send_input(chunk.bytes);
    // Pasting into a terminal is being in it, the same statement `Input`
    // makes: a reader who pastes has chosen where the text goes, and the
    // activity mark must not then call it output in a terminal they are not
    // in.
    client.focused = chunk.term;
    if (Session* const home = session_holding(chunk.term)) home->last_focused = chunk.term;
    const bool finished = chunk.final_chunk != 0;
    paste_slots_[chunk.term].owner = finished ? 0 : client.id;
    proto::PasteAck ack;
    ack.seq = chunk.seq;
    send(client, ack);
    if (finished) drain_paste_slot(chunk.term);
}

void Server::drain_paste_slot(TerminalId term) {
    // Re-found on every pass rather than held across the loop: `send` can drop
    // a client, and dropping one releases its slots — which erases from the
    // very map an iterator would be pointing into.
    for (;;) {
        auto slot = paste_slots_.find(term);
        if (slot == paste_slots_.end() || slot->second.owner != 0) return;
        if (slot->second.waiting.empty()) {
            paste_slots_.erase(slot);
            return;
        }
        const PendingPaste next = std::move(slot->second.waiting.front());
        slot->second.waiting.pop_front();
        Terminal* const terminal = terminals_.find(term);
        if (terminal == nullptr) {
            forget_paste_slot(term);
            return;
        }
        Client* const waiting = client_with_id(next.client);
        // Its reader left while it waited. Nothing to write and nobody to ack;
        // the next in line gets the slot instead.
        if (waiting == nullptr) continue;
        write_paste_chunk(*terminal, *waiting, next.chunk);
        return;  // whoever just wrote either holds the slot or drained it
    }
}

void Server::release_paste_slots(ClientId client) {
    std::vector<TerminalId> freed;
    for (auto& [term, slot] : paste_slots_) {
        // Nothing this client queued is wanted any more: the reader who asked
        // for it is gone, and half their clipboard is not what the next reader
        // of that terminal should find in it.
        std::erase_if(slot.waiting, [client](const PendingPaste& pending) {
            return pending.client == client;
        });
        if (slot.owner == client) {
            slot.owner = 0;
            freed.push_back(term);
        }
    }
    // Drained after the sweep rather than inside it: draining writes, writing
    // can drop a client, and dropping one comes back here.
    for (const TerminalId term : freed) drain_paste_slot(term);
}

void Server::forget_paste_slot(TerminalId term) {
    const auto slot = paste_slots_.find(term);
    if (slot == paste_slots_.end()) return;
    std::deque<PendingPaste> waiting = std::move(slot->second.waiting);
    paste_slots_.erase(slot);
    // Acked, not dropped. The terminal is gone and the text has nowhere to go,
    // but a client that never hears back holds the rest of that paste — and
    // every later one — in a queue nothing drains (WP-18).
    for (const PendingPaste& pending : waiting) {
        Client* const owner = client_with_id(pending.client);
        if (owner == nullptr) continue;
        proto::PasteAck ack;
        ack.seq = pending.chunk.seq;
        send(*owner, ack);
    }
}

void Server::drop(Client& client, std::string_view why) {
    // A paste this connection was holding cannot be finished by anybody, and
    // what it queued is not wanted (WP-42). Released before the socket goes,
    // so the terminal is free for whoever is still here.
    release_paste_slots(client.id);
    // A socket that closed IS a detach (the session model): no message arrives, nothing
    // is asked, and the terminals carry on. Marked before the stream goes so
    // that whatever the loop does next does not treat this client as attached.
    const bool was_attached = client.attached;
    client.attached = false;
    if (!why.empty())
        std::fprintf(stderr, "ckmux server: dropping client %llu: %.*s\n",
                     static_cast<unsigned long long>(client.id), static_cast<int>(why.size()),
                     why.data());
    // One last attempt to get whatever is queued out — a Refuse is only useful
    // if it arrives — and then the connection goes.
    (void)client.stream.flush();
    client.stream.close();
    client.closing = true;
    // And the others learn this session's new reader count on the tick (WP-48).
    // A socket that closed is the commonest detach there is — it is how
    // quitting a client, closing a window and dropping an SSH connection all
    // arrive — and it does not pass through `detach` at all, because `detach`
    // exists to send a `Detached` message and this socket has gone.
    if (was_attached) session_list_dirty_ = true;
}

void Server::flush_tick() {
    const std::int64_t period =
        1'000'000'000 / std::max(1, options_.settings.max_fps);
    const std::int64_t now = clock_.now_nanos();
    // Catch up rather than drift: a tick that was late does not make every
    // later tick late, and a server that was suspended does not owe a thousand
    // ticks when it wakes.
    next_tick_nanos_ = now + period;

    // A mismatched CLI client that never sent the `KillServer` its own
    // refusal invited it to send does not hold a connection open forever.
    for (const std::unique_ptr<Client>& client : clients_) {
        if (!client->awaiting_kill_from_mismatched_cli || client->closing) continue;
        if (now >= client->mismatch_kill_deadline_nanos)
            drop(*client, "did not ask to kill the server within the mismatch window");
    }

    for (const TerminalId id : terminals_.ids())
        if (Terminal* terminal = terminals_.find(id)) terminal->observe_exit();

    // A session being ended is watched here rather than waited for: the loop
    // keeps serving every other terminal while its programs take their time.
    if (!kills_.empty()) advance_kills();
    // And a terminal being closed, for the same reason.
    if (!closes_.empty()) advance_closes();
    // An attach or a detach moved a reader count (WP-48). Flushed HERE, after
    // the passes above have finished moving sessions around and before
    // anything else is composed: the point of the flag is that nothing is ever
    // published from the middle of an operation, and a flush placed earlier
    // would give that away again.
    if (session_list_dirty_) flush_reader_counts();

    // Then the children nobody asked to end. After the two above, so that a
    // terminal this server is already closing is removed by `advance_closes`
    // and never announced twice; before `diffs_.flush`, which clears the
    // lifecycle damage this reads.
    notice_exits();

    // A client that fell behind is healed before anything else is sent to it: a
    // fresh snapshot, once there is room for a screen again. The deltas it
    // missed are gone and cannot be reconstructed — the diff engine's belief
    // moved on without it — so repairing is not an option and pretending
    // otherwise would put a screen in front of a reader that no program ever
    // drew (the protocol spec).
    for (const std::unique_ptr<Client>& client : clients_) {
        if (!client->attached || !client->dirty_snapshot) continue;
        (void)client->stream.flush();
        // The same question the delta gate asks, in the same words. Waiting for
        // a queue that is completely empty while deltas stop at a quarter of a
        // megabyte is how a client whose socket drains slower than a snapshot
        // came to be re-snapshotted on every tick, forever (M-S5).
        if (!client->stream.ready_for_screen()) continue;
        send_snapshot(*client);
    }

    // What each client is still owed of its pictures, as its queue has room.
    // Bounded per tick so the deltas behind it stay prompt; what does not fit
    // now goes next tick, and the mirror shows its held pixels meanwhile.
    for (const std::unique_ptr<Client>& client : clients_) {
        if (!client->attached || client->closing) continue;
        while (client->owed_sent < client->owed_images.size() &&
               client->stream.queued() < kOwedImageBurstBytes)
            send(*client, client->owed_images[client->owed_sent++].message);
        if (!client->owed_images.empty() && client->owed_sent >= client->owed_images.size()) {
            client->owed_images.clear();
            client->owed_sent = 0;
        }
    }

    // Everything a terminal has to say that is not its grid — the marks, a
    // clipboard write, the printer going on or off, the newest complaint.
    //
    // Before `diffs_.flush`, which is what clears the damage each of them is
    // gated on, and AFTER the heal above, which is the ordering that matters
    // for exactly one of them: a snapshot carries the clipboard WATERMARK and
    // deliberately not the text, so a write announced before it would be
    // adopted away by the snapshot that followed it in the same tick. The
    // others are restatements of what a snapshot just said, which costs a few
    // bytes and says the same thing twice rather than losing something.
    announce_terminal_news();
    // And where the windows are, which is a fact about a session rather than
    // about any one terminal. After the heal above for the same reason the news
    // is: a snapshot states the layout, so a delta sent before it would be
    // superseded by the snapshot that followed it in the same tick.
    announce_layout();

    const DiffEngine::Tick tick = diffs_.flush(
        terminals_, [this](TerminalId term, std::uint64_t wire_id) {
            return picture_wanted_now(term, wire_id);
        });

    // A terminal that scrolled more in one tick than a delta may carry says so
    // rather than sending it. What the client watching THAT session needs is
    // the same thing a client that fell behind needs, and it arrives the same
    // way — but only that client. A flood in one session is not a reason to
    // re-snapshot every other reader on the machine, and the latch is sticky:
    // it clears only when a snapshot is taken, so a broadcast turned one noisy
    // terminal into a full snapshot per tick for everybody, forever, including
    // clients watching sessions that terminal has nothing to do with
    // (13-architecture-review C2). With the filter, a terminal in a session
    // nobody is attached to dirties nobody, and its latch clears naturally on
    // the snapshot the first watcher to attach is given.
    for (const TerminalId id : terminals_.ids()) {
        const TerminalDiffer* differ = diffs_.differ_for(id);
        if (differ == nullptr || !differ->needs_snapshot()) continue;
        const Session* const home = session_holding(id);
        if (home == nullptr) continue;
        for_each_attached(home->id, [](Client& watcher) { watcher.dirty_snapshot = true; });
    }

    if (tick.deltas.empty() && tick.images.empty()) return;
    for (const std::unique_ptr<Client>& client : clients_) {
        // Only the attached client, and only a UI: a CLI utility has no screen,
        // and a connection that has not attached has not asked for one.
        if (!client->attached || client->kind != proto::ClientKind::Ui) continue;
        const Session* const watched = session_for(client->session);
        if (watched == nullptr) continue;
        if (client->dirty_snapshot) continue;  // waiting to be healed
        // With no room for a screen, a client stops being sent deltas at all.
        // Sending anyway would grow a queue that is already too big — and every
        // byte queued behind the mark is a byte the reader will never see,
        // because what finally arrives is a snapshot that supersedes it. It is
        // also what keeps every OTHER answer on the connection prompt: a `Pong`
        // queued behind four megabytes of stale screen arrives seconds late,
        // which is what the flood gate measured before this mark existed. The
        // same predicate the heal gate above uses, so that no queue can satisfy
        // one and not the other.
        //
        // With one exception: a queue over the mark because a heal's own
        // bytes — its snapshot, or the picture drip paying its debt — have
        // not drained yet is not a client that fell behind again, and
        // marking it dirty on that was the whole loop: a snapshot larger
        // than the mark plus any steady news (a blinking cursor is enough)
        // healed once per drain, forever. While the drain window is open the
        // deltas keep flowing — the sequence restarted at the snapshot, so
        // they are its valid continuation, and the mirror needs their
        // continuity — and a reader who has genuinely stopped is what the
        // stream's hard limit is for, as it always was.
        if (client->draining_heal && client->stream.ready_for_screen() &&
            client->owed_sent >= client->owed_images.size())
            client->draining_heal = false;
        if (!client->stream.ready_for_screen() && !client->draining_heal) {
            client->dirty_snapshot = true;
            continue;
        }
        const bool owes_pictures = client->owed_sent < client->owed_images.size();
        for (const proto::GridDelta& delta : tick.deltas) {
            // A delta for a terminal in another session is not this client's to
            // see: sessions are what a reader has instead of one big desktop.
            if (std::find(watched->terminals.begin(), watched->terminals.end(), delta.term) ==
                watched->terminals.end())
                continue;
            send(*client, delta);
        }
        // The pictures ride the same gate as the deltas (WP-16): the session
        // filter, because pixels for another session's terminal are not this
        // client's to hold, and the backlog mark, because a client that will
        // be healed by a snapshot gets the believed pictures with it. While a
        // debt is still being paid, fresh picture ops JOIN it rather than
        // jumping it: one wire id's payloads must arrive in the order they
        // were produced, and the debt already holds that id's older ones.
        for (const DiffEngine::ImageOp& op : tick.images) {
            if (std::find(watched->terminals.begin(), watched->terminals.end(), op.term) ==
                watched->terminals.end())
                continue;
            if (owes_pictures)
                owe_image(*client, op.message);
            else
                send(*client, op.message);
        }
    }
}

namespace {
// One second between stats passes. Fixed in v1 (the configuration spec lists it under
// "explicitly not configurable"): a reader glancing at a frame readout is
// well served at 1 Hz, and the interval is not worth a knob until somebody
// can say what they would set it to.
constexpr std::int64_t kStatsIntervalNanos = 1'000'000'000;
}  // namespace

bool Server::anyone_watching_stats() const noexcept {
    // The capability half of nobody-watching-nobody-sampling: a platform
    // that cannot measure has nothing to schedule, however many clients ask.
    // Without this, a subscribed client on such a platform buys a wakeup per
    // second spent snapshotting an empty table.
    if (!platform::process_stats_supported()) return false;
    for (const std::unique_ptr<Client>& client : clients_)
        if (client->greeted && client->attached && client->watch_stats && !client->closing)
            return true;
    return false;
}

std::int64_t Server::nanos_until_stats() const {
    const std::int64_t now = clock_.now_nanos();
    return next_stats_nanos_ > now ? next_stats_nanos_ - now : 0;
}

void Server::stats_tick() {
    // Catch up rather than drift, exactly as the flush tick does.
    const std::int64_t now = clock_.now_nanos();
    next_stats_nanos_ = now + kStatsIntervalNanos;
    ++stats_passes_;

    // One table for the whole pass, however many terminals are watched —
    // WP-37's design point, and the reason N terminals cost one read.
    const platform::ProcessTable table = platform::ProcessTable::snapshot();

    for (const std::unique_ptr<Client>& client : clients_) {
        if (!client->greeted || !client->attached || !client->watch_stats || client->closing)
            continue;
        Session* const watched = session_of(*client);
        if (watched == nullptr) continue;
        for (const TerminalId id : watched->terminals) {
            Terminal* const terminal = terminals_.find(id);
            if (terminal == nullptr) continue;
            StatsBaseline& baseline = stats_baselines_[id];

            const int root = terminal->process_id();
            const platform::TreeSample sample =
                root >= 0 ? platform::sample_tree(table, root) : platform::TreeSample{};
            if (root < 0 || sample.process_count == 0) {
                // The child is gone. Said once, so the watcher clears the
                // readout instead of freezing its last number over a dead
                // shell — and not repeated every second for a terminal that
                // stays on screen under an exit banner.
                if (!baseline.dead_announced) {
                    baseline.dead_announced = true;
                    proto::TermStats stats;
                    stats.term = id;
                    send(*client, stats);
                }
                continue;
            }

            proto::TermStats stats;
            stats.term = id;
            stats.rss_bytes = sample.rss_bytes;
            stats.real_bytes = sample.real_bytes;
            stats.flags = static_cast<std::uint8_t>(proto::TermStatsFlag::Alive);
            if (sample.has_real)
                stats.flags |= static_cast<std::uint8_t>(proto::TermStatsFlag::HasReal);
            // The rate needs a previous pass to differ against; the first
            // sample after a subscription reports its memory and no CPU,
            // which is the honest reading of "no interval yet".
            if (baseline.primed && now > baseline.at_nanos &&
                sample.cpu_time_nanos >= baseline.cpu_nanos) {
                const std::uint64_t cpu_delta = sample.cpu_time_nanos - baseline.cpu_nanos;
                const std::uint64_t wall_delta =
                    static_cast<std::uint64_t>(now - baseline.at_nanos);
                stats.cpu_permille = static_cast<std::uint32_t>(cpu_delta * 1000u / wall_delta);
            }
            baseline.cpu_nanos = sample.cpu_time_nanos;
            baseline.at_nanos = now;
            baseline.primed = true;
            baseline.dead_announced = false;
            send(*client, stats);
        }
    }
}

void Server::run() {
    while (step()) {
    }
    // The listener first: a stopping server must stop LOOKING like a server
    // before it does anything slow. Killing terminals below can take a grace
    // period, and a client that connected to the still-bound socket in that
    // window would sit in a backlog nobody will ever accept — which is exactly
    // what a client that just ended the last session does next: reconnect, to
    // start a fresh server for a fresh session. The socket is gone before the
    // children are, and the children are gone before this returns, so at no
    // moment can a NEW server start while this one still owns running
    // programs.
    listener_.close();
    terminals_.close_all();
    // What was already queued goes out before the sockets do. A client that is
    // told "the session ended" and then reads EOF knows what happened; one that
    // only reads EOF has to guess, and would guess "the server crashed".
    //
    // Drained with retries, briefly and bounded — not one attempt: a flush
    // moves what the kernel will take at that instant, and the moment this
    // matters most is exactly when the queue is fullest, because the final
    // Detached and the emptied session list were just queued to everyone.
    // One attempt lost the tail often enough for a test to see the picker
    // starve of its list.
    for (int attempt = 0; attempt < 250; ++attempt) {
        bool still_writing = false;
        for (const std::unique_ptr<Client>& client : clients_) {
            if (!client->stream.wants_write()) continue;
            (void)client->stream.flush();
            still_writing = still_writing || client->stream.wants_write();
        }
        if (!still_writing) break;
        ::usleep(2000);
    }
    for (const std::unique_ptr<Client>& client : clients_) client->stream.close();
    clients_.clear();
}

int run_server_process(const std::filesystem::path& socket, bool foreground) {
    if (!foreground && !ckm::platform::daemonize(ckm::platform::server_log_path(socket))) {
        std::fprintf(stderr, "ckmux: cannot detach the server\n");
        return 1;
    }

    Server::Options options;
    options.socket = socket;
    // The server owns the emulator, so the server owns the settings that decide
    // what a terminal tells its child it can do (the configuration spec).
    options.settings = ckm::load_settings(ckm::platform::config_file_path()).settings;
    // Read once, here, where this process is being composed — not per terminal
    // from inside the server, which is meant to be a thing a test can drive
    // with every environmental fact already decided for it.
    options.working_directory = ckm::platform::home_directory().string();

    ckv::term::PosixClock clock;
    Server server(std::move(options), clock);
    switch (server.start()) {
        case Server::StartStatus::Listening: break;
        case Server::StartStatus::AlreadyRunning:
            // Not an error, and not worth a word on a reader's terminal: two
            // clients found no server and both started one. Exactly one of them
            // is right, and both end up connected.
            return 0;
        case Server::StartStatus::Racing: return 0;
        case Server::StartStatus::Failed:
            std::fprintf(stderr, "ckmux: %s\n", server.problem().c_str());
            return 1;
    }
    std::fprintf(stderr, "ckmux server: listening on %s\n", socket.string().c_str());
    server.run();
    std::fprintf(stderr, "ckmux server: stopped\n");
    return 0;
}

}  // namespace ckm::server
