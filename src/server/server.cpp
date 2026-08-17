// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "server/server.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
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

    const std::vector<proto::GridDelta> deltas = diffs_.flush(terminals_);
    if (deltas.empty()) return;
    for (const std::unique_ptr<Client>& client : clients_) {
        if (!client->greeted || client->kind != proto::ClientKind::Ui) continue;
        // Above the high-water mark a client stops being sent deltas at all; it
        // gets a snapshot when its queue drains (WP-6). Sending anyway would
        // grow a queue that is already too big.
        if (client->stream.over_high_water()) {
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
