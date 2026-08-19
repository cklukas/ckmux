// Copyright (c) 2026 C. Klukas. All rights reserved.
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
        case proto::MessageType::Attached: return "Attached";
        case proto::MessageType::Detach: return "Detach";
        case proto::MessageType::KillServer: return "KillServer";
        default: break;
    }
    return "a message";
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

std::size_t Server::queued_bytes() const noexcept {
    std::size_t queued = 0;
    for (const std::unique_ptr<Client>& client : clients_) queued += client->stream.queued();
    return queued;
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
        // 64 KiB per terminal per pass (plans/02). Bounded because a child
        // running `yes` must not be able to hold this loop.
        (void)terminals_.drain(64 * 1024);
        for (const TerminalId id : terminals_.ids())
            if (Terminal* terminal = terminals_.find(id)) terminal->flush_replies_to_child();
    }

    if (clock_.now_nanos() >= next_tick_nanos_) flush_tick();

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
            // both numbers and the remedy in words (plans/05). "Version
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

    if (const auto* ping = std::get_if<proto::Ping>(&message)) {
        proto::Pong pong;
        pong.nonce = ping->nonce;
        send(client, pong);
        return;
    }
    if (std::holds_alternative<proto::Hello>(message)) {
        drop(client, "said Hello twice");
        return;
    }
    if (std::holds_alternative<proto::ListSessions>(message)) {
        // Asked before attaching, which is the whole point: a client decides
        // what to do — attach, take over, or start a new one — from what is
        // actually running (plans/06's picker).
        //
        // Timestamps stay zero: a server's clock is monotonic by design
        // (ckVision's determinism rule), so "created at" needs a wall-clock
        // reading nothing here is entitled to take.
        send_session_list(client);
        return;
    }
    if (const auto* request = std::get_if<proto::NewSession>(&message)) {
        Session& session = create_session(request->name);
        std::fprintf(stderr, "ckmux server: session %llu \"%s\" created\n",
                     static_cast<unsigned long long>(session.id), session.name.c_str());
        // The list, not just an id: the client that asked wants to show the
        // result, and every other client's picker is now out of date.
        for (const std::unique_ptr<Client>& watcher : clients_)
            if (watcher->greeted) send_session_list(*watcher);
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
        // A name is a reader's label, not an identifier: the id is what
        // everything else refers to, so a duplicate name is their business.
        session->name = request->name.empty() ? session->name : request->name;
        for (const std::unique_ptr<Client>& watcher : clients_)
            if (watcher->greeted) send_session_list(*watcher);
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
        // made it (plans/03) — what a reattaching client then does with one is
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
            // (plans/03 says the same about closing a terminal twice).
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
        // hold the child (plans/02) — a client's own environment is its own.
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
        if (Client* const watcher = client_attached_to(session->id)) send(*watcher, opened);
        return;
    }
    if (const auto* input = std::get_if<proto::Input>(&message)) {
        // Opaque on purpose: the bytes were encoded by the client against
        // mirrored mode state (plans/04), so the server writes them to the PTY
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
            if (Session* const home = session_holding(input->term)) home->focused = input->term;
        }
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
    error.human = std::string(name_of(type)) +
                  " is not implemented by this build of the server yet (sessions arrive with WP-8, "
                  "attach with WP-6).";
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

Server::Client* Server::client_attached_to(SessionId id) {
    for (const std::unique_ptr<Client>& client : clients_)
        if (client->attached && !client->closing && client->session == id) return client.get();
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
    home.focused = terminal.id();
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
            for (const std::unique_ptr<Client>& watcher : clients_)
                if (watcher->greeted) send_session_list(*watcher);
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
        // away (plans/03 on-exit). So the terminal is NOT removed — its last
        // screen is what the banner is drawn over, and it is this server that
        // still holds it.
        proto::TermClosed closed;
        closed.term = id;
        closed.exited = 1;
        closed.exit_status = status.value_or(0);
        closed.hold = 1;
        if (Session* const home = session_holding(id))
            if (Client* const watcher = client_attached_to(home->id)) send(*watcher, closed);
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
        if (home->focused == id) {
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
            if (!a_client_resized_it) {
                bool wrote = damage.scrollback_pushed > 0;
                for (const ckv::term::TerminalDamage::RowSpan& row : damage.rows) {
                    if (row.empty()) continue;
                    wrote = true;
                    break;
                }
                if (wrote) terminal->mark_activity();
            }
        }

        Client* const watcher = client_attached_to(home->id);
        if (watcher == nullptr) continue;

        // Either half is news: the marks, or the name the reader gave it. One
        // message states both, so a rename costs the same single `TermMeta` a
        // bell does rather than a message of its own.
        if (!terminal->marks_announced() || !terminal->custom_title_announced()) {
            proto::TermMeta meta;
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
        if (damage.printer) {
            const ckv::term::TerminalStatus status = terminal->status();
            proto::PrintState printing;
            printing.term = id;
            printing.mode = wire_printer_mode(options_.settings.printer_mode);
            printing.state = status.printer_controller_active ? proto::PrinterState::Capturing
                                                              : proto::PrinterState::Idle;
            printing.bytes = static_cast<std::uint32_t>(
                std::min<std::size_t>(status.printer_pending_bytes, 0xFFFFFFFFu));
            printing.jobs = static_cast<std::uint16_t>(
                std::min<std::size_t>(status.printer_jobs_ready, 0xFFFFu));
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

void Server::announce_layout() {
    for (const Session& session : sessions_) {
        // Nothing to say unless a window actually moved. The gate is per
        // terminal and the message is per session, so one report that moves
        // three windows costs one message — the same edge trigger `TermMeta`
        // uses, and for the same reason: a client that reports an arrangement
        // on every frame of a drag must not cost a message per frame.
        //
        // It is also what stops the echo from becoming a loop. The client that
        // reported this layout is the client this states it back to (there is
        // at most one watcher per session — plans/03's takeover), and a mirror
        // applies what it is told; if applying it produced another report, an
        // unconditional producer would answer that one too, forever. An
        // unchanged layout changes nothing, so the exchange stops after one.
        bool moved = false;
        for (const TerminalId id : session.terminals) {
            const Terminal* const terminal = terminals_.find(id);
            if (terminal == nullptr || terminal->layout_announced()) continue;
            moved = true;
            break;
        }
        if (!moved) continue;
        Client* const watcher = client_attached_to(session.id);
        // No watcher, no announcement — and the flag stays down, so the
        // arrangement is stated to the next client to attach by the snapshot it
        // is given rather than lost. A session nobody is watching is exactly
        // when a layout most needs remembering.
        if (watcher == nullptr) continue;

        proto::LayoutDelta layout;
        layout.session = session.id;
        layout.focused_term = session.focused;
        // The desktop this client declared, the same number the snapshot
        // states, because a fraction of a desktop means nothing without the
        // desktop it is a fraction of.
        layout.desktop_columns = static_cast<std::uint16_t>(std::max(0, watcher->desktop.width));
        layout.desktop_rows = static_cast<std::uint16_t>(std::max(0, watcher->desktop.height));
        for (const TerminalId id : session.terminals) {
            Terminal* const terminal = terminals_.find(id);
            if (terminal == nullptr) continue;
            // Every window, not only the ones that moved: a `z_order` is a
            // position among the others, so a partial list is an arrangement a
            // mirror cannot apply.
            const WindowLayout& held = terminal->layout();
            layout.entries.push_back(proto::LayoutEntry{
                id, held.rect, held.z_order, static_cast<std::uint8_t>(held.zoomed ? 1 : 0),
                held.tile});
            terminal->note_layout_announced();
        }
        send(*watcher, layout);
    }
}

void Server::remove_terminal(TerminalId id, bool exited) {
    Session* const home = session_holding(id);
    std::optional<int> status;
    if (Terminal* const terminal = terminals_.find(id)) status = terminal->exit_status();
    (void)terminals_.close(id);
    diffs_.forget(id);
    proto::TermClosed closed;
    closed.term = id;
    closed.exited = exited ? 1 : 0;
    closed.exit_status = status.value_or(0);
    if (home == nullptr) return;
    home->terminals.erase(std::remove(home->terminals.begin(), home->terminals.end(), id),
                          home->terminals.end());
    if (Client* const watcher = client_attached_to(home->id)) send(*watcher, closed);
    // Every client's picker counts terminals, and the count just changed.
    for (const std::unique_ptr<Client>& watcher : clients_)
        if (watcher->greeted) send_session_list(*watcher);
    // The reader's kill-empty-session (plans/03): a session emptied by an
    // explicit close has served its purpose, and the watcher falls back to
    // the picker rather than keeping an empty desktop nobody asked for.
    if (home->terminals.empty() && options_.settings.kill_empty_session)
        forget_session(home->id, proto::DetachReason::SessionKilled, "its last terminal closed");
}

Session* Server::session_holding(TerminalId id) {
    for (Session& session : sessions_)
        for (const TerminalId held : session.terminals)
            if (held == id) return &session;
    return nullptr;
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
    if (target == source) return;  // plans/03: target = source is a no-op
    // A move is the reader deciding to keep the program: a close that was
    // pending against this terminal is off.
    closes_.erase(std::remove_if(closes_.begin(), closes_.end(),
                                 [&request](const PendingClose& pending) {
                                     return pending.term == request.term;
                                 }),
                  closes_.end());
    // Reparenting is editing the two id lists. The PTY, the emulator and the
    // scrollback never lived anywhere else, so the child does not notice
    // (plans/03).
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
    if (Client* const watcher = client_attached_to(source->id)) send(*watcher, left);
    // The target's watcher learns from a fresh snapshot. A terminal it has
    // never seen has no delta baseline — the same problem as a client that
    // fell behind, healed the same way.
    if (Client* const watcher = client_attached_to(target->id)) watcher->dirty_snapshot = true;
    for (const std::unique_ptr<Client>& watcher : clients_)
        if (watcher->greeted) send_session_list(*watcher);
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
    for (const std::unique_ptr<Client>& client : clients_)
        if (client->greeted) send_session_list(*client);
    // The last session was the reason this process existed. A server with none
    // left is holding a socket, a lock and a process table entry on behalf of
    // nothing — so it goes, and the next reader who wants a session gets a
    // fresh one started for them (plans/03: the server is started on demand).
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
        info.attached = client_attached_to(session.id) != nullptr ? 1 : 0;
        list.sessions.push_back(std::move(info));
    }
    send(client, list);
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
    // Granted immediately, and taken from whoever held THAT session (plans/03).
    // A reader whose laptop slept cannot be kept out by the client that is
    // nominally still holding their session — which is why the previous holder
    // is told rather than asked.
    if (Client* previous = client_attached_to(session->id);
        previous != nullptr && previous != &client) {
        detach(*previous, proto::DetachReason::Takeover,
               "taken over by another client on this machine");
    }
    // Said out loud, because "which client is watching which session" is the
    // first question anybody debugging a multiplexer asks, and a server log
    // that answers it costs one line.
    std::fprintf(stderr, "ckmux server: client %llu attached to session %llu \"%s\"\n",
                 static_cast<unsigned long long>(client.id),
                 static_cast<unsigned long long>(session->id), session->name.c_str());
    client.attached = true;
    client.session = session->id;
    client.dirty_snapshot = false;
    client.host_sixel = request.host_sixel != 0;
    if (request.columns > 0 && request.rows > 0) {
        client.desktop = ckv::Size{request.columns, request.rows};
        client.cell_pixels = cell_metric_of(request);
    }
    send_snapshot(client);
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
    snapshot.desktop_columns = static_cast<std::uint16_t>(std::max(0, client.desktop.width));
    snapshot.desktop_rows = static_cast<std::uint16_t>(std::max(0, client.desktop.height));
    const Session* const session = session_for(client.session);
    snapshot.focused_term = session != nullptr ? session->focused : 0;
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
        state.printer_mode = wire_printer_mode(options_.settings.printer_mode);
        // Where the reader last had this window. The layout half of what
        // survives a detach, said with everything else a reattaching client
        // gets rather than through a message of its own — the arrangement is
        // part of the state, not news about it (plans/03: window layout is
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
        for (proto::Message& op : diffs_.attach_images(id))
            client.owed_images.push_back(std::move(op));
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
        // does (plans/05).
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

void Server::drop(Client& client, std::string_view why) {
    // A socket that closed IS a detach (plans/03): no message arrives, nothing
    // is asked, and the terminals carry on. Marked before the stream goes so
    // that whatever the loop does next does not treat this client as attached.
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
    // drew (plans/05).
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
            send(*client, client->owed_images[client->owed_sent++]);
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

    const DiffEngine::Tick tick = diffs_.flush(terminals_);

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
        if (Client* const watcher = client_attached_to(home->id)) watcher->dirty_snapshot = true;
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
                client->owed_images.push_back(op.message);
            else
                send(*client, op.message);
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
    // what a terminal tells its child it can do (plans/08).
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
