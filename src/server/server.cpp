// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "server/server.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    poller_.clear();
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

    const std::int64_t until_tick = nanos_until_tick();
    const int timeout_ms = static_cast<int>(std::min<std::int64_t>(until_tick / 1'000'000, 1'000));
    const std::vector<platform::Ready>& ready = poller_.wait(timeout_ms);

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
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [](const std::unique_ptr<Client>& client) {
                                      return client->closing || !client->stream.open();
                                  }),
                   clients_.end());
    return running_;
}

void Server::accept_pending() {
    for (;;) {
        std::string refusal;
        const int fd = listener_.accept_one(refusal);
        if (fd < 0) {
            if (!refusal.empty()) std::fprintf(stderr, "ckmux server: %s\n", refusal.c_str());
            return;
        }
        auto client = std::make_unique<Client>();
        client->id = next_client_++;
        client->stream = platform::Stream(fd);
        clients_.push_back(std::move(client));
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
        const int columns = std::max(1, static_cast<int>(resize->rect.width));
        const int rows = std::max(1, static_cast<int>(resize->rect.height));
        terminal->resize(columns, rows, columns * std::max(0, client.cell_pixels.width),
                         rows * std::max(0, client.cell_pixels.height));
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
        // the picture is for.
        spec.host_sixel = client.host_sixel;
        // Where a shell opens, and what it is told about its machine. The
        // server is the one that knows both, because it is the process that will
        // hold the child (plans/02) — a client's own environment is its own.
        if (const char* const home = std::getenv("HOME"); home != nullptr && *home != '\0')
            spec.working_directory = home;
        else
            spec.working_directory = "/";
        // The size the client asked for, if it said — a `NewTerminal` carries a
        // rect for exactly this. A client with no opinion yet gets a
        // conventional screen and corrects it the moment its view is laid out,
        // which is one frame away.
        spec.columns = request->rect.width > 0 ? static_cast<int>(request->rect.width) : 80;
        spec.rows = request->rect.height > 0 ? static_cast<int>(request->rect.height) : 24;
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
        if (Terminal* terminal = terminals_.find(input->term))
            terminal->send_input(input->bytes);
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
        // Out of time with the escalation enabled, or everything is already
        // gone: closing is what actually removes a terminal either way, and
        // ckVision's close escalates to SIGKILL for whatever is left.
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
        // The child exited on the asking, or the time is up with the
        // escalation enabled. Closing is what removes a terminal either way —
        // ckVision's close escalates to SIGKILL for whatever is left.
        closes_.erase(closes_.begin() + static_cast<std::ptrdiff_t>(index));
        remove_terminal(pending.term, /*exited=*/true);
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
    proto::Detached detached;
    detached.reason = reason;
    detached.text = std::move(text);
    send(client, detached);
    // The terminals are untouched, which is the whole promise: a detach is a
    // client going away, and the programs it was watching do not care.
}

void Server::send_snapshot(Client& client) {
    proto::Snapshot snapshot;
    snapshot.desktop_columns = static_cast<std::uint16_t>(std::max(0, client.desktop.width));
    snapshot.desktop_rows = static_cast<std::uint16_t>(std::max(0, client.desktop.height));
    const Session* const session = session_for(client.session);
    snapshot.focused_term = session != nullptr ? session->focused : 0;
    std::uint16_t index = 0;
    for (const TerminalId id : session != nullptr ? session->terminals : std::vector<TerminalId>{}) {
        Terminal* terminal = terminals_.find(id);
        if (terminal == nullptr) continue;
        // Taking a snapshot restarts that terminal's sequence, so the first
        // delta a client sees afterwards is 1 — which is what lets it check
        // continuity from its first delta rather than take it on trust (WP-4b).
        proto::TerminalState state = diffs_.snapshot(id, terminal->session());
        state.index = index++;
        snapshot.terminals.push_back(std::move(state));
    }
    proto::Attached attached;
    attached.session = client.session;
    attached.snapshot = std::move(snapshot);
    send(client, attached);
    // The pictures, after the grid (WP-16): the believed set, exactly what the
    // clients already watching hold, so every mirror agrees until the next
    // tick moves them all together.
    for (const TerminalId id : session != nullptr ? session->terminals : std::vector<TerminalId>{})
        for (const proto::Message& op : diffs_.attach_images(id)) send(client, op);
    client.dirty_snapshot = false;
}

void Server::send(Client& client, const proto::Message& message) {
    if (!client.stream.open()) return;
    if (!client.stream.send(proto::encode(message))) {
        // Over the high-water mark. The bytes are queued — dropping half a frame
        // would desynchronise the stream — but nothing more should be queued
        // until it drains, and the client will need a fresh snapshot when it
        // does (plans/05).
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

    for (const TerminalId id : terminals_.ids())
        if (Terminal* terminal = terminals_.find(id)) terminal->observe_exit();

    // A session being ended is watched here rather than waited for: the loop
    // keeps serving every other terminal while its programs take their time.
    if (!kills_.empty()) advance_kills();
    // And a terminal being closed, for the same reason.
    if (!closes_.empty()) advance_closes();

    // A client that fell behind is healed before anything else is sent to it: a
    // fresh snapshot, once its queue has drained. The deltas it missed are gone
    // and cannot be reconstructed — the diff engine's belief moved on without
    // it — so repairing is not an option and pretending otherwise would put a
    // screen in front of a reader that no program ever drew (plans/05).
    for (const std::unique_ptr<Client>& client : clients_) {
        if (!client->attached || !client->dirty_snapshot) continue;
        (void)client->stream.flush();
        if (client->stream.queued() > 0) continue;  // still draining
        send_snapshot(*client);
    }

    const DiffEngine::Tick tick = diffs_.flush(terminals_);

    // A terminal that scrolled more in one tick than a delta may carry says so
    // rather than sending it. What the client needs then is the same thing a
    // client that fell behind needs, and it arrives the same way.
    for (const TerminalId id : terminals_.ids()) {
        const TerminalDiffer* differ = diffs_.differ_for(id);
        if (differ == nullptr || !differ->needs_snapshot()) continue;
        for (const std::unique_ptr<Client>& client : clients_)
            if (client->attached) client->dirty_snapshot = true;
    }

    if (tick.deltas.empty() && tick.images.empty()) return;
    for (const std::unique_ptr<Client>& client : clients_) {
        // Only the attached client, and only a UI: a CLI utility has no screen,
        // and a connection that has not attached has not asked for one.
        if (!client->attached || client->kind != proto::ClientKind::Ui) continue;
        const Session* const watched = session_for(client->session);
        if (watched == nullptr) continue;
        if (client->dirty_snapshot) continue;  // waiting to be healed
        // Past the backlog mark a client stops being sent deltas at all.
        // Sending anyway would grow a queue that is already too big — and every
        // byte queued behind the mark is a byte the reader will never see,
        // because what finally arrives is a snapshot that supersedes it. It is
        // also what keeps every OTHER answer on the connection prompt: a `Pong`
        // queued behind four megabytes of stale screen arrives seconds late,
        // which is what the flood gate measured before this mark existed.
        if (client->stream.over_delta_backlog()) {
            client->dirty_snapshot = true;
            continue;
        }
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
        // be healed by a snapshot gets the believed pictures with it.
        for (const DiffEngine::ImageOp& op : tick.images) {
            if (std::find(watched->terminals.begin(), watched->terminals.end(), op.term) ==
                watched->terminals.end())
                continue;
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
