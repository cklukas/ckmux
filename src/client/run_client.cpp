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
    // What this display can actually show, said once and carried on every
    // Attach: the server folds it into the graphics advertisement the
    // children this client opens are given (WP-16). Read from the probed
    // capabilities, the same evidence Help ▸ Terminal Report shows.
    session.set_host_sixel(terminal.capabilities().sixel_graphics);
    // Where the bytes are reassembled into frames. Declared here rather than
    // beside the loop because a reconnection has to start it over: half a frame
    // from a server that has gone is not the beginning of one from its
    // successor.
    proto::FrameReader reader;
    // A server ends with its last session (plans/03), so losing the connection
    // is an ordinary thing that happens to a client whose reader ended their
    // work — not a failure. The next request that needs a server starts one,
    // exactly as the first one did.
    const auto ensure_connected = [&connection, &session, &reader, &options]() -> bool {
        if (connection.stream.open()) return true;
        ServerConnection fresh =
            connect_to_server(options.socket, options.executable, proto::ClientKind::Ui);
        if (!fresh.ok()) return false;
        connection = std::move(fresh);
        reader = proto::FrameReader{};
        // Nothing this client held survives: the mirrors were of terminals in
        // another process, and the attachment was to a session that server had.
        session.connection_lost();
        return true;
    };
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
    // Detaching, and quitting, are the same act for an attached client: say so
    // to the server, and stop drawing. The programs go on running — which is
    // the whole point, and why quitting must not ask a reader whether to close
    // anything.
    bool detached = false;
    options.client.detach_from_server = [&session, &app, &detached]() {
        detached = true;
        session.request(proto::Detach{});
        app.request_quit();
    };
    // Asking is not being told: the list arrives later, and only a list this
    // client asked for on a READER's behalf opens the picker. The others — the
    // one after every attach, the one after a rename — keep the names current
    // and put nothing in anybody's way.
    bool show_picker_when_the_list_arrives = false;
    options.client.list_sessions = [&session, &ensure_connected, &show_picker_when_the_list_arrives]() {
        show_picker_when_the_list_arrives = true;
        if (!ensure_connected()) return;
        session.request(proto::ListSessions{});
    };
    // Switching sessions is a detach and an attach — and everything on the
    // screen belongs to the session being left, so it goes. A client that kept
    // its windows would be showing a reader the terminals of a session it is no
    // longer watching, each one frozen at the last frame it saw.
    // One way to start watching a session, wherever the asking came from.
    //
    // The windows go first, because they belong to the session being left. The
    // server needs no `Detach`: an `Attach` to another session IS the switch,
    // and it leaves the one being left with its programs running and nobody
    // watching — which is exactly the state ckmux exists to keep (plans/03).
    const auto switch_to = [&session, &client, &terminal, &ensure_connected](std::uint64_t id) {
        if (session.attached() && session.session() == id) return;
        if (!ensure_connected()) return;
        if (client != nullptr) client->forget_terminals();
        session.attach(id, terminal.size(), terminal.capabilities().cell_pixels);
    };
    options.client.attach_to_session = switch_to;
    // Creating a session means going to it: a reader who typed a name and
    // pressed Create was not asking for a row in a list.
    bool waiting_for_a_new_session = false;
    options.client.create_session = [&session, &ensure_connected,
                                     &waiting_for_a_new_session](const std::string& name) {
        // The one place a server is started rather than found: a reader asking
        // for a session after the last one ended is asking for a server too,
        // and they should not have to know that.
        if (!ensure_connected()) return;
        proto::NewSession ask;
        ask.name = name;
        waiting_for_a_new_session = true;
        session.request(ask);
    };
    options.client.rename_session = [&session](const std::string& name) {
        proto::RenameSession ask;
        ask.id = session.session();
        ask.name = name;
        session.request(ask);
    };
    options.client.kill_session = [&session](bool force, int grace_seconds) {
        proto::KillSession ask;
        ask.session = session.session();
        ask.force = force ? 1 : 0;
        ask.grace_seconds = static_cast<std::uint16_t>(std::max(0, grace_seconds));
        session.request(ask);
    };
    // The close dialog's promise, carried to the server: asked first, killed
    // after the grace only when the reader ticked it. The window stays up —
    // it goes when the TermClosed comes back, because only the server knows
    // when (or whether) the program actually ended.
    options.client.close_terminal_in_session = [&session](ckv::term::TerminalSubsession& terminal,
                                                          bool force, int grace_seconds) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return;
        proto::CloseTerminal ask;
        ask.term = *id;
        ask.force = force ? 1 : 0;
        ask.grace_seconds = static_cast<std::uint16_t>(std::max(0, grace_seconds));
        session.request(ask);
    };
    options.client.move_terminal = [&session](ckv::term::TerminalSubsession& terminal,
                                              std::uint64_t destination, bool to_new_session) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return;
        proto::MoveTerminal ask;
        ask.term = *id;
        ask.destination_session = destination;
        ask.to_new_session = to_new_session ? 1 : 0;
        session.request(ask);
    };
    // The session usually has terminals already — that is what attaching to one
    // MEANS. One is asked for only if it turns out to be empty, once the
    // snapshot has said so.
    options.client.open_terminal_at_startup = false;

    ClientApp application(app, std::move(options.client));
    client = &application;

    // What to do with a session list depends on when it arrives. The first one
    // is the startup decision — attach, or ask — and every one after it is a
    // reader having asked what is running.
    //
    //   * Nothing running: start a session and attach to it. A reader who ran
    //     `ckmux` wants a terminal, not a dialog about how to get one.
    //   * One session, nobody watching it: attach. This is the reattach that
    //     the whole project is for, and it needs no permission.
    //   * Anything else — a session already in use, or several to choose from —
    //     is a genuine choice, and only a reader can make it. The picker offers
    //     taking it over, a new one, or carrying on with no session at all.
    bool startup_decided = false;
    session.on_sessions = [&client, &session, &switch_to, &startup_decided, &waiting_for_a_new_session,
                           &show_picker_when_the_list_arrives](
                              const std::vector<proto::SessionInfo>& sessions) {
        if (client == nullptr) return;
        std::vector<SessionRow> rows;
        rows.reserve(sessions.size());
        for (const proto::SessionInfo& info : sessions)
            rows.push_back(SessionRow{info.id, info.name, info.terminals, info.attached != 0});

        // A session this client asked for, now that it exists. The newest is
        // the one it asked for: ids only ever go up.
        if (waiting_for_a_new_session && !rows.empty()) {
            waiting_for_a_new_session = false;
            startup_decided = true;
            std::uint64_t newest = 0;
            for (const SessionRow& row : rows) newest = std::max(newest, row.id);
            // The list is what names sessions, so it is remembered before the
            // attach that will ask for the name of the one just made.
            client->remember_sessions(rows);
            switch_to(newest);
            return;
        }
        if (!startup_decided) {
            startup_decided = true;
            if (rows.empty()) {
                waiting_for_a_new_session = true;
                startup_decided = false;
                proto::NewSession ask;
                session.request(ask);
                return;
            }
            if (rows.size() == 1 && !rows.front().attached) {
                client->remember_sessions(rows);
                switch_to(rows.front().id);
                return;
            }
            show_picker_when_the_list_arrives = false;
            client->show_session_picker(std::move(rows));
            return;
        }
        if (!show_picker_when_the_list_arrives) {
            client->remember_sessions(std::move(rows));
            return;
        }
        show_picker_when_the_list_arrives = false;
        client->show_session_picker(std::move(rows));
    };

    // A terminal the server has ended takes its window with it — including
    // every terminal of a session this client has just left, which is how a
    // switch clears what it was showing.
    session.on_terminal_closed = [&session, &client](std::uint64_t id) {
        if (client == nullptr) return;
        if (RemoteTerminalSubsession* remote = session.terminal(id))
            client->close_window_for_terminal(*remote);
    };

    session.on_terminal_opened = [&pending, &client](RemoteTerminalSubsession& remote) {
        pending.push_back(&remote);
        // The window, now that there is something behind it. `open_terminal` is
        // the same call M1 makes; what differs is only when it happens.
        // Empty: the client numbers and places it, exactly as it does for a
        // local one. The child's own caption replaces the name as soon as it
        // sets one.
        if (client != nullptr) (void)client->open_terminal({});
    };

    // An empty session gets a terminal; a session with programs in it gets left
    // alone. Checked once, after the first snapshot, because that is the first
    // moment the answer exists.
    bool asked_for_the_first_terminal = false;
    const auto open_one_if_the_session_is_empty = [&session, &asked_for_the_first_terminal,
                                                   &options]() {
        if (asked_for_the_first_terminal || !session.attached()) return;
        asked_for_the_first_terminal = true;
        if (!session.terminal_ids().empty()) return;
        proto::NewTerminal ask;
        ask.command = options.client.settings.shell;
        session.request(ask);
    };

    // One frame before the first question. The session list can arrive in the
    // very first poll pass — before the application has ever laid the desktop
    // out — and a dialog presented into an unlaid desktop is sized against
    // nothing: a zero-rect window, on screen but invisible, and modal, so it
    // also swallows every key a reader presses at what looks like a working
    // ckmux. One step gives the desktop its real bounds first.
    (void)app.step(clock.now_nanos());

    // Asked before anything is attached to: what this client does next depends
    // on what is already running, and only the server knows that.
    session.request(proto::ListSessions{});

    session.on_attached = [&client, &session, &asked_for_the_first_terminal](std::uint64_t id) {
        if (client == nullptr) return;
        // The name comes from the list rather than the attach: a client that
        // attached from a picker already has it, and one that did not will get
        // it with the next list. Zero would be a lie either way — it is
        // watching this session now.
        client->set_attached_session(id, client->session_name(id));
        // The next empty-session check belongs to THIS attachment: a session
        // with nothing in it gets a terminal, and one with programs in it is
        // left exactly as its reader left it.
        asked_for_the_first_terminal = false;
        // So the name is right in the prompts that use it, and so that a reader
        // who takes a session over sees the list they are now part of.
        session.request(proto::ListSessions{});
    };
    session.on_detached = [&app, &client, &session, &detached,
                           &show_picker_when_the_list_arrives](proto::DetachReason reason,
                                                              const std::string& text) {
        // Nothing is written to stderr here, and nowhere else in this loop
        // either: an attached client's stderr IS the reader's terminal, and
        // ckVision is drawing on it. One line about a takeover landed in the
        // middle of a window frame and stayed there.
        //
        // A detach this client asked for is a client on its way out. Any other
        // — a takeover, a session ended under it — leaves it running with
        // nothing to watch, which is a state it can be in perfectly well: the
        // commands that need a session go grey and the reader can pick another.
        (void)reason;
        (void)text;
        if (detached) {
            app.request_quit();
            return;
        }
        if (client != nullptr) {
            client->forget_terminals();
            client->set_attached_session(0, {});
        }
        // What now? A reader whose session was taken from them, or ended under
        // them, is looking at an empty desktop; the list says what else is
        // there and the picker lets them go to it.
        show_picker_when_the_list_arrives = true;
        session.request(proto::ListSessions{});
    };

    // Anything worth telling the reader on the way out, printed once the
    // application has finished with the terminal.
    std::string farewell;
    std::int64_t next_reconnect_nanos = 0;
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

        if (connection.stream.wants_write() && !connection.stream.flush()) {
            // A flush that fails is the server going away mid-write, and the
            // promise is the same as reading EOF three lines down: this client
            // keeps running (WP-8). Quitting here was the pre-WP-8 answer, and
            // it is why a reader who ended their last session could sometimes
            // watch their whole ckmux vanish instead of getting the picker —
            // whether the doomed request's flush beat the server's close was a
            // coin toss. The bytes were for nobody; the reconnect throttle
            // below decides what happens next.
            connection.stream.close();
            session.connection_lost();
            if (client != nullptr) {
                client->forget_terminals();
                client->set_attached_session(0, {});
            }
        }
        std::string arrived;
        const bool alive = !connection.stream.open() || connection.stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) break;
        for (;;) {
            proto::Message message;
            const proto::DecodeError error = reader.next(message);
            if (error == proto::DecodeError::Incomplete) break;
            if (error != proto::DecodeError::None) {
                // Said after the loop, once ckVision has given the terminal
                // back: a line printed now would land on top of the frame it
                // is still drawing.
                farewell = "ckmux: the server sent something this build cannot read";
                app.request_quit();
                break;
            }
            (void)session.handle(message);
        }
        session.heal_if_needed();
        open_one_if_the_session_is_empty();
        // Every window whose terminal has news repaints, which is the same
        // notification a local terminal raises — the client does not care where
        // the bytes came from.
        for (const std::uint64_t id : session.terminal_ids())
            if (RemoteTerminalSubsession* remote = session.terminal(id))
                app.root().notify_terminal_subsession_changed(*remote);
        (void)app.step(clock.now_nanos());
        if (!alive) {
            // The server has gone — because a reader ended the last session, or
            // because it failed. Either way this client keeps running with
            // nothing attached: its windows belong to terminals that no longer
            // exist, and the commands that need a session go grey until there
            // is one again. Quitting on it would take a reader's whole ckmux
            // away for something they may well have asked for.
            connection.stream.close();
            session.connection_lost();
            if (client != nullptr) {
                client->forget_terminals();
                client->set_attached_session(0, {});
            }
        }
        // A reader who was waiting for a session list still is, connection or
        // no connection. Asking again starts a server, which is what makes
        // "end the last session, then start another" one uninterrupted thing —
        // and it RETRIES on later passes rather than deciding once, because
        // the moment after a server exits is exactly when connecting to its
        // socket can fail: the old one may not have unbound yet. Throttled, so
        // a machine where no server can start is not asked twenty times a
        // second.
        if (show_picker_when_the_list_arrives && !connection.stream.open()) {
            const std::int64_t now_nanos = clock.now_nanos();
            if (now_nanos >= next_reconnect_nanos) {
                next_reconnect_nanos = now_nanos + 1'000'000'000;
                if (ensure_connected()) session.request(proto::ListSessions{});
            }
        }
    }
    if (connection.stream.open()) {
        if (!detached) (void)connection.stream.send(proto::encode(proto::Detach{}));
        (void)connection.stream.flush();
    }
    if (!farewell.empty()) {
        std::fprintf(stderr, "%s\n", farewell.c_str());
        return 1;
    }
    return 0;
}

}  // namespace ckm::client
