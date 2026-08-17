// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "client/run_client.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>

#include "client/server_connection.hpp"
#include "client/server_session.hpp"
#include "cvision/core/clock.hpp"
#include "cvision/term/terminal.hpp"
#include "cvision/ui/application.hpp"

namespace ckm::client {
namespace {

// The application's own sources, plus the socket. ckVision hands out its
// descriptors precisely so a host can wait on them together with its own
// (D-021); this is that pattern with one extra fd.
int wait_for_work(ckv::ui::Application& app, int socket_fd, bool want_write, int timeout_ms) {
    std::vector<pollfd> waiting;
    for (const ckv::term::WaitHandle& handle : app.wait_handles())
        if (handle.kind == ckv::term::WaitHandleKind::PosixFileDescriptor)
            waiting.push_back(pollfd{static_cast<int>(handle.value), POLLIN, 0});
    if (socket_fd >= 0)
        waiting.push_back(
            pollfd{socket_fd, static_cast<short>(POLLIN | (want_write ? POLLOUT : 0)), 0});
    if (waiting.empty()) return 0;
    return ::poll(waiting.data(), static_cast<nfds_t>(waiting.size()), timeout_ms);
}

}  // namespace

int run_attached_client(ckv::term::Terminal& terminal, ckv::Clock& clock, RunOptions options) {
    ServerConnection connection =
        connect_to_server(options.socket, options.executable, proto::ClientKind::Ui);
    if (!connection.ok()) {
        std::fprintf(stderr, "ckmux: %s\n", connection.problem.c_str());
        return 1;
    }

    ckv::ui::Application app(terminal, clock);
    ServerSession session([&connection](const proto::Message& message) {
        (void)connection.stream.send(proto::encode(message));
    });
    session.set_history_limit(static_cast<std::size_t>(options.client.settings.scrollback));

    // Windows follow terminals, and terminals belong to the server. A window is
    // opened when the server says a terminal exists, which is also what makes a
    // reattach draw what is already running rather than starting something new.
    // Windows follow the server's terminals. A window opens when a terminal is
    // announced — which covers both a reader asking for a new one and every
    // terminal that was already running when this client attached, and is why a
    // reattach draws what is there rather than starting something else.
    //
    // The mirrors are owned by the session rather than adopted into the
    // application: a mirror has no descriptors to wait on and nothing to drain,
    // so adoption would buy nothing and would split the ownership of the very
    // thing the routing looks up by id.
    ClientApp* client = nullptr;
    std::vector<RemoteTerminalSubsession*> pending;
    options.client.terminal_source =
        [&pending](TerminalRequest request) -> ckv::term::TerminalSubsession& {
        (void)request;
        RemoteTerminalSubsession* ready = pending.front();
        pending.erase(pending.begin());
        return *ready;
    };
    options.client.request_new_terminal = [&session, &options]() {
        proto::NewTerminal ask;
        ask.command = options.client.settings.shell;
        session.request(ask);
        return true;
    };

    ClientApp application(app, std::move(options.client));
    client = &application;

    session.on_terminal_opened = [&pending, &client](RemoteTerminalSubsession& remote) {
        pending.push_back(&remote);
        // The window, now that there is something behind it. `open_terminal` is
        // the same call M1 makes; what differs is only when it happens.
        if (client != nullptr) (void)client->open_terminal("Terminal");
    };

    const ckv::Size size = terminal.size();
    session.attach(0, size, terminal.capabilities().cell_pixels);

    session.on_detached = [&app](proto::DetachReason reason, const std::string& text) {
        // A takeover is not an error and not a crash: another client has the
        // session now, and this one has nothing left to draw. Saying why on the
        // way out is the whole point of carrying the reason (plans/03).
        std::fprintf(stderr, "ckmux: detached (%d): %s\n", static_cast<int>(reason), text.c_str());
        app.request_quit();
    };

    proto::FrameReader reader;
    while (!app.quit_requested()) {
        const std::int64_t now = clock.now_nanos();
        // The application's next timer, or nothing at all when it has none. A
        // cap either way: the socket is not the only reason to wake, and a loop
        // that slept until a timer would not notice a window being resized.
        const std::optional<std::int64_t> deadline = app.next_timer_deadline_nanos();
        const std::int64_t until = deadline.has_value() && *deadline > now ? *deadline - now : 0;
        const int timeout_ms =
            static_cast<int>(std::min<std::int64_t>(until > 0 ? until / 1'000'000 : 50, 50));
        (void)wait_for_work(app, connection.stream.fd(), connection.stream.wants_write(),
                            std::max(1, timeout_ms));

        if (connection.stream.wants_write() && !connection.stream.flush()) break;
        std::string arrived;
        const bool alive = connection.stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) break;
        for (;;) {
            proto::Message message;
            const proto::DecodeError error = reader.next(message);
            if (error == proto::DecodeError::Incomplete) break;
            if (error != proto::DecodeError::None) {
                std::fprintf(stderr, "ckmux: the server sent something this build cannot read\n");
                return 1;
            }
            (void)session.handle(message);
        }
        session.heal_if_needed();
        // Every window whose terminal has news repaints, which is the same
        // notification a local terminal raises — the client does not care where
        // the bytes came from.
        for (const std::uint64_t id : session.terminal_ids())
            if (RemoteTerminalSubsession* remote = session.terminal(id))
                app.root().notify_terminal_subsession_changed(*remote);
        (void)app.step(clock.now_nanos());
        if (!alive) {
            std::fprintf(stderr, "ckmux: the server closed the connection\n");
            return 0;
        }
    }
    proto::Detach detach;
    (void)connection.stream.send(proto::encode(detach));
    (void)connection.stream.flush();
    return 0;
}

}  // namespace ckm::client
