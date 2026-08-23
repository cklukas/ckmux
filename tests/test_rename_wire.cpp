// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-36 over the wire: the name a reader gives a terminal, kept by the server
// as session state and stated back to every client watching it.
//
// A separate suite from `test_attach.cpp` although it drives the same shape of
// harness, because what it is about is different: that one is about a client
// falling behind and being healed, this one is about a fact the server holds
// on a terminal's behalf. Its own copy of the socket scaffolding is the price,
// and it is a small one — the parts that matter here are a real `Server` on a
// real Unix socket with the tick stepped by hand, and nothing else.
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <unistd.h>

#include "client/server_connection.hpp"
#include "client/server_session.hpp"
#include "common/proto.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"

#include "cvision/core/clock.hpp"
#include "cvision/testing/cktest.hpp"

namespace {

using ckm::server::Server;

std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-n" + std::to_string(static_cast<unsigned long>(::getpid()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (std::string(name) + ".sock");
}

void forget(const std::filesystem::path& socket) {
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    std::filesystem::remove(socket.parent_path(), ignored);
}

ckm::Settings test_settings() {
    ckm::Settings settings;
    settings.shell = "/bin/sh";
    settings.login_shell = false;
    settings.scrollback = 100;
    settings.max_fps = 30;
    return settings;
}

ckm::server::TerminalSpec spec_running(std::string command) {
    ckm::server::TerminalSpec spec;
    spec.command = std::move(command);
    spec.working_directory = "/";
    spec.columns = 80;
    spec.rows = 24;
    spec.pixel_width = 80 * 9;
    spec.pixel_height = 24 * 18;
    return spec;
}

// One connected client, with the real `ServerSession` behind it.
struct Client {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader reader;
    ckm::client::ServerSession session{nullptr};

    bool connect(const std::filesystem::path& socket) {
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        if (result.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = ckm::platform::Stream(result.fd);
        session = ckm::client::ServerSession([this](const ckm::proto::Message& message) {
            (void)stream.send(ckm::proto::encode(message));
        });
        session.set_history_limit(100);
        return true;
    }

    void greet() {
        ckm::proto::Hello hello;
        hello.build = "a test";
        (void)stream.send(ckm::proto::encode(hello));
    }

    // Reads whatever has arrived and routes it into the session, which is the
    // only consumer these tests have: what they assert on is mirror state.
    void pump() {
        std::string arrived;
        (void)stream.receive(arrived, 1024 * 1024);
        if (!arrived.empty() && !reader.append(arrived)) return;
        for (;;) {
            ckm::proto::Message message;
            if (reader.next(message) != ckm::proto::DecodeError::None) break;
            if (std::holds_alternative<ckm::proto::HelloAck>(message)) continue;
            (void)session.handle(message);
        }
        session.heal_if_needed();
    }
};

// Steps the server until a predicate holds. The clock is manual but the
// children are real, so a wait on a child's OUTPUT crossing a PTY is bounded
// by the machine's scheduler rather than by virtual ticks — hence the real
// millisecond per unsatisfied pass, and the larger budget where a child has to
// be scheduled at all.
inline constexpr int kChildOutputPasses = 2000;
template <typename Ready>
bool run_until(Server& server, ckv::ManualClock& clock, Client& client, Ready ready,
               int passes = 200) {
    for (int pass = 0; pass < passes; ++pass) {
        clock.advance(34'000'000);
        if (!server.step()) return false;
        client.pump();
        if (ready()) return true;
        ::usleep(1000);
    }
    return false;
}

}  // namespace

CK_TEST(a_name_a_reader_gives_a_terminal_outlives_the_client_that_gave_it) {
    // The whole reason a custom title is on this wire at all. A client that
    // pinned a caption locally would show a name that vanished the moment its
    // reader detached — which in a multiplexer is the one thing a name must
    // not do. So: the server holds it, states it to the client that asked, and
    // hands it to the next client in the snapshot it attaches with.
    const std::filesystem::path socket = private_socket("rename");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));
    const ckm::server::TerminalId id = terminal.id();

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));
    ckm::client::RemoteTerminalSubsession* mirror = watcher.session.terminal(id);
    CK_CHECK(mirror != nullptr);
    if (mirror == nullptr) {
        server.terminals().close_all();
        forget(socket);
        return;
    }
    CK_CHECK(mirror->mirror().custom_title().empty());

    // The child names itself, the ordinary way. Both facts travel, and they
    // are different fields: a wire that carried one of them would make the
    // override and the program's own title the same string, which is exactly
    // what it must not be.
    terminal.session().feed_output("\x1b]2;make -j8\a");
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return mirror->mirror().title() == "make -j8"; },
                       kChildOutputPasses));

    ckm::proto::RenameTerminal rename;
    rename.id = id;
    rename.name = "the build";
    watcher.session.request(rename);
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return mirror->mirror().custom_title() == "the build"; }));
    // Stated back rather than assumed: the client that asked is told what the
    // server actually stored. And the child's own title is untouched under it.
    CK_CHECK(mirror->mirror().title() == "make -j8");
    CK_CHECK(server.terminals().find(id)->custom_title() == "the build");

    // The child renames itself again while the reader's name is up. It reaches
    // the mirror, and it does not disturb the override.
    terminal.session().feed_output("\x1b]2;make install\a");
    CK_CHECK(run_until(server, clock, watcher,
                       [&] { return mirror->mirror().title() == "make install"; },
                       kChildOutputPasses));
    CK_CHECK(mirror->mirror().custom_title() == "the build");

    // The reader goes away and somebody attaches instead — the reattach case
    // and the second-client case in one. The name is in the snapshot, so it is
    // there before any `TermMeta` could arrive.
    watcher.session.request(ckm::proto::Detach{});
    CK_CHECK(run_until(server, clock, watcher, [&] { return !watcher.session.attached(); }));

    Client returning;
    CK_CHECK(returning.connect(socket));
    returning.greet();
    returning.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, returning, [&] { return returning.session.attached(); }));
    ckm::client::RemoteTerminalSubsession* again = returning.session.terminal(id);
    CK_CHECK(again != nullptr);
    if (again != nullptr) {
        CK_CHECK(again->mirror().custom_title() == "the build");
        CK_CHECK(again->mirror().title() == "make install");
    }

    // And handing the name back is the same message with nothing in it, which
    // the server stores as "none" rather than reading as "leave it alone".
    ckm::proto::RenameTerminal cleared;
    cleared.id = id;
    returning.session.request(cleared);
    CK_CHECK(run_until(server, clock, returning,
                       [&] { return again != nullptr && again->mirror().custom_title().empty(); }));
    CK_CHECK(server.terminals().find(id)->custom_title().empty());

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(naming_a_terminal_that_is_gone_is_answered_rather_than_ignored) {
    // Every other per-terminal request answers an unknown id with an Error
    // naming the request; a rename that silently did nothing would leave a
    // reader's dialog looking as though it had worked.
    const std::filesystem::path socket = private_socket("rename-gone");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    ckm::proto::Error refused;
    bool told = false;
    watcher.session.on_error = [&refused, &told](const ckm::proto::Error& error) {
        refused = error;
        told = true;
    };
    watcher.session.attach(0, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    // An id this server has never had. Ids are never recycled, so this can only
    // be a stale client — the case a reader has to be told about rather than
    // left with a dialog that looked as though it worked.
    ckm::proto::RenameTerminal rename;
    rename.id = 9999;
    rename.name = "nothing is called this";
    watcher.session.request(rename);
    CK_CHECK(run_until(server, clock, watcher, [&] { return told; }));
    CK_CHECK(refused.code == static_cast<std::uint16_t>(ckm::proto::ErrorCode::NoSuchTerminal));
    CK_CHECK(refused.context == "RenameTerminal");
    CK_CHECK(!refused.human.empty());

    server.terminals().close_all();
    forget(socket);
}
