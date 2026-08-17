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
        client.desktop = ckv::Size{resize->columns, resize->rows};
        // Every terminal follows the client's window, because in this build the
        // session has one desktop and the terminals fill it. WP-8's layout makes
        // this a per-window question; the pixel fields carry the whole text area
        // and the terminal divides them down to one cell (WP-3).
        for (const TerminalId id : terminals_.ids())
            if (Terminal* terminal = terminals_.find(id))
                terminal->resize(resize->columns, resize->rows, resize->pixel_width,
                                 resize->pixel_height);
        return;
    }
    if (const auto* request = std::get_if<proto::NewTerminal>(&message)) {
        TerminalSpec spec;
        spec.command = request->command;
        // Where a shell opens, and what it is told about its machine. The
        // server is the one that knows both, because it is the process that will
        // hold the child (plans/02) — a client's own environment is its own.
        if (const char* const home = std::getenv("HOME"); home != nullptr && *home != '\0')
            spec.working_directory = home;
        else
            spec.working_directory = "/";
        spec.columns = client.desktop.width > 0 ? client.desktop.width : 80;
        spec.rows = client.desktop.height > 0 ? client.desktop.height : 24;
        Terminal& terminal = terminals_.open(spec);
        focused_terminal_ = terminal.id();
        proto::TermOpened opened;
        opened.term = terminal.id();
        opened.session = session_;
        opened.columns = static_cast<std::uint16_t>(spec.columns);
        opened.rows = static_cast<std::uint16_t>(spec.rows);
        for (const std::unique_ptr<Client>& watcher : clients_)
            if (watcher->attached) send(*watcher, opened);
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
        (void)terminals_.close(close_it->term);
        diffs_.forget(close_it->term);
        proto::TermClosed closed;
        closed.term = close_it->term;
        closed.exited = 1;
        for (const std::unique_ptr<Client>& watcher : clients_)
            if (watcher->attached) send(*watcher, closed);
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

Server::Client* Server::attached_client() {
    for (const std::unique_ptr<Client>& client : clients_)
        if (client->attached && !client->closing) return client.get();
    return nullptr;
}

void Server::attach(Client& client, const proto::Attach& request) {
    // Granted immediately, and taken from whoever held it (plans/03). A reader
    // whose laptop slept cannot be kept out by the client that is nominally
    // still holding their session — which is why the previous holder is told
    // rather than asked.
    if (Client* previous = attached_client(); previous != nullptr && previous != &client) {
        detach(*previous, proto::DetachReason::Takeover,
               "taken over by another client on this machine");
    }
    client.attached = true;
    client.session = session_ != 0 ? session_ : request.session;
    client.dirty_snapshot = false;
    if (request.columns > 0 && request.rows > 0) {
        client.desktop = ckv::Size{request.columns, request.rows};
        for (const TerminalId id : terminals_.ids())
            if (Terminal* terminal = terminals_.find(id))
                terminal->resize(request.columns, request.rows, request.pixel_width,
                                 request.pixel_height);
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
    snapshot.focused_term = focused_terminal_;
    std::uint16_t index = 0;
    for (const TerminalId id : terminals_.ids()) {
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
    attached.session = client.session != 0 ? client.session : session_;
    attached.snapshot = std::move(snapshot);
    send(client, attached);
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

    const std::vector<proto::GridDelta> deltas = diffs_.flush(terminals_);

    // A terminal that scrolled more in one tick than a delta may carry says so
    // rather than sending it. What the client needs then is the same thing a
    // client that fell behind needs, and it arrives the same way.
    for (const TerminalId id : terminals_.ids()) {
        const TerminalDiffer* differ = diffs_.differ_for(id);
        if (differ == nullptr || !differ->needs_snapshot()) continue;
        for (const std::unique_ptr<Client>& client : clients_)
            if (client->attached) client->dirty_snapshot = true;
    }

    if (deltas.empty()) return;
    for (const std::unique_ptr<Client>& client : clients_) {
        // Only the attached client, and only a UI: a CLI utility has no screen,
        // and a connection that has not attached has not asked for one.
        if (!client->attached || client->kind != proto::ClientKind::Ui) continue;
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
        for (const proto::GridDelta& delta : deltas) send(*client, delta);
    }
}

void Server::run() {
    while (step()) {
    }
    // Children first: a server that unlinked its socket and then took time
    // killing terminals would leave a window in which a new server can start
    // while the old one still owns running programs.
    terminals_.close_all();
    for (const std::unique_ptr<Client>& client : clients_) client->stream.close();
    clients_.clear();
    listener_.close();
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
