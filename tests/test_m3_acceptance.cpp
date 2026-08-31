// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-15 — M3 acceptance: every row of the operations table in
// The session model driven at the wire, and
// held to the semantics AND the edge cases that table states.
//
// The table is the spec, so each case below quotes the sentence it is
// asserting. That is deliberate: an acceptance suite whose assertions have
// drifted from the document is worse than none, because it reports a
// milestone met against criteria nobody wrote down. The testing plan's fifth global
// invariant says the same thing from the other side — every plan-doc
// behaviour row maps to a named test, and drift is a test failure or a plan
// edit, never silence.
//
// Scope of this file, stated so a reader can see its edges: the rows whose
// semantics had **no behavioural coverage** when WP-15 was picked up
// (`rename-session`, `kill-terminal`), and the stated **edge cases** across
// the other rows, which is where coverage was thinnest. Rows whose happy path
// is already driven elsewhere are not re-driven here — `new-terminal` and
// `attach` in test_attach.cpp, `kill-server` in test_core_promise.cpp and
// test_server_lifecycle.cpp, `close-terminal` and `move-terminal` in
// test_server_loop.cpp, `rename-terminal` in test_rename_wire.cpp. This file
// covers what those do not, rather than duplicating them.
//
// `kill-terminal` was one of the two uncovered rows and is no longer here. It
// was covered by a case pinning a DIVERGENCE — the table promises the
// operation and nothing implemented it: no server handler, no
// `commands::kKillTerminal`, no menu path, and a request answered only by the
// unknown-message catch-all. That gap is now a package of its own (owner,
// 2026-08-20, routed to the session that confirmed it independently from the
// tree), which will implement the operation and drive it from its own suite.
// The case was removed rather than left to fail on the day the gap closes,
// because a suite that reddens the tree the moment somebody fixes something
// taxes every other session for a fact this file no longer needs to carry.
//
// Which leaves the thing that makes "every operations-table row green" a
// checkable claim rather than an asserted one: the coverage map at the foot of
// this file. Rows are driven from wherever they are best driven; the map is
// what says none was missed.
#include <algorithm>
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
    directory /= "ckmux-m3" + std::to_string(static_cast<unsigned long>(::getpid()));
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
    spec.environment = {{"TERM", "xterm-256color"}, {"PATH", "/usr/bin:/bin"}, {"LC_ALL", "C"}};
    return spec;
}

// A client on the wire, with the real `ServerSession` behind it.
struct Client {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader reader;
    ckm::client::ServerSession session{nullptr};
    std::vector<ckm::proto::Message> unread;
    // `ListSessions` is a question with an answer rather than a value a client
    // holds, so the answer is captured where it arrives.
    std::vector<ckm::proto::SessionInfo> listed;
    // Likewise an `Error`: the session routes it to `on_error` and reports the
    // message handled, so it never lands in `unread`.
    std::vector<ckm::proto::Error> errors;

    bool connect(const std::filesystem::path& socket) {
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        if (result.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = ckm::platform::Stream(result.fd);
        session = ckm::client::ServerSession(
            [this](const ckm::proto::Message& message) { (void)stream.send(ckm::proto::encode(message)); });
        session.set_history_limit(100);
        session.on_sessions = [this](const std::vector<ckm::proto::SessionInfo>& sessions) {
            listed = sessions;
        };
        session.on_error = [this](const ckm::proto::Error& error) { errors.push_back(error); };
        return true;
    }

    void greet() {
        ckm::proto::Hello hello;
        hello.build = "m3 acceptance";
        (void)stream.send(ckm::proto::encode(hello));
    }

    std::size_t pump() {
        std::string arrived;
        (void)stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) return 0;
        std::size_t count = 0;
        for (;;) {
            ckm::proto::Message message;
            if (reader.next(message) != ckm::proto::DecodeError::None) break;
            ++count;
            if (std::holds_alternative<ckm::proto::HelloAck>(message)) continue;
            if (!session.handle(message)) unread.push_back(message);
        }
        session.heal_if_needed();
        return count;
    }

    template <typename T>
    const T* first_of() const {
        for (const ckm::proto::Message& message : unread)
            if (const T* found = std::get_if<T>(&message)) return found;
        return nullptr;
    }

    template <typename T>
    std::size_t count_of() const {
        std::size_t count = 0;
        for (const ckm::proto::Message& message : unread)
            if (std::holds_alternative<T>(message)) ++count;
        return count;
    }
};

inline constexpr int kChildPasses = 2000;

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

// Both clients, since several rows below are about what the OTHER reader sees.
template <typename Ready>
bool run_until_both(Server& server, ckv::ManualClock& clock, Client& a, Client& b, Ready ready,
                    int passes = 200) {
    for (int pass = 0; pass < passes; ++pass) {
        clock.advance(34'000'000);
        if (!server.step()) return false;
        a.pump();
        b.pump();
        if (ready()) return true;
        ::usleep(1000);
    }
    return false;
}

}  // namespace

// --- rename-session: implemented since M3, never driven ---------------------

CK_TEST(renaming_a_session_reaches_every_client_that_is_watching) {
    // The session model: "`rename-session` | Update name; broadcast to every client's
    // picker."
    //
    // Implemented on both sides — `server.cpp` handles the message, the menu
    // reaches it — and covered by nothing but a codec round-trip in
    // test_proto.cpp when WP-15 was picked up. A rename that updated the
    // server and told nobody would pass that round-trip perfectly.
    const std::filesystem::path socket = private_socket("rename-session");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Session& session = server.create_session("before");
    const std::uint64_t id = session.id;

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.attach(id, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    ckm::proto::RenameSession rename;
    rename.id = id;
    rename.name = "after";
    watcher.session.request(rename);

    // The broadcast is what is being asserted, so the check is on what the
    // CLIENT was told rather than on the server's own field.
    CK_CHECK(run_until(server, clock, watcher, [&] {
        watcher.session.request(ckm::proto::ListSessions{});
        for (const ckm::proto::SessionInfo& summary : watcher.listed)
            if (summary.id == id) return summary.name == "after";
        return false;
    }));

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(an_empty_new_name_leaves_a_session_called_what_it_was) {
    // The session model, the edge case, and it differs from `rename-terminal` on
    // purpose: "an EMPTY name leaves it alone, because a session with no name
    // cannot be picked."
    //
    // The opposite of WP-36's rule for a terminal, where an empty name is a
    // request to hand the caption back. Two operations, two answers to the
    // same input, and the only way to keep them straight is to assert both.
    const std::filesystem::path socket = private_socket("rename-empty");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Session& session = server.create_session("keep-me");
    const std::uint64_t id = session.id;

    Client watcher;
    CK_CHECK(watcher.connect(socket));
    watcher.greet();
    watcher.session.attach(id, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, watcher, [&] { return watcher.session.attached(); }));

    // First a rename that must take, so that the empty one below is measured
    // against a name the server actually accepted rather than against the
    // one it started with — otherwise "unchanged" proves nothing.
    ckm::proto::RenameSession real;
    real.id = id;
    real.name = "renamed-once";
    watcher.session.request(real);
    CK_CHECK(run_until(server, clock, watcher, [&] {
        watcher.session.request(ckm::proto::ListSessions{});
        for (const ckm::proto::SessionInfo& summary : watcher.listed)
            if (summary.id == id) return summary.name == "renamed-once";
        return false;
    }));

    ckm::proto::RenameSession empty;
    empty.id = id;
    empty.name = "";
    watcher.session.request(empty);
    for (int pass = 0; pass < 60; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        watcher.pump();
    }
    watcher.session.request(ckm::proto::ListSessions{});
    for (int pass = 0; pass < 30; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        watcher.pump();
    }
    bool still_named = false;
    for (const ckm::proto::SessionInfo& summary : watcher.listed)
        if (summary.id == id) still_named = summary.name == "renamed-once";
    CK_CHECK(still_named);

    server.terminals().close_all();
    forget(socket);
}

// --- the edge cases the table states ---------------------------------------

CK_TEST(detaching_the_last_client_from_an_empty_session_leaves_the_session_alive) {
    // The session model: "Detaching the last client from an *empty* session (0
    // terminals) does not kill it — sessions die only explicitly or when their
    // last terminal closes."
    //
    // Two ways to die are named and detaching is neither, which is the whole
    // content of the rule: a reader who closes their last window and then
    // detaches still has a session to come back to.
    const std::filesystem::path socket = private_socket("detach-empty");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Session& session = server.create_session("empty-but-mine");
    const std::uint64_t id = session.id;

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    client.session.attach(id, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, client, [&] { return client.session.attached(); }));

    client.session.request(ckm::proto::Detach{});
    CK_CHECK(run_until(server, clock, client, [&] { return !client.session.attached(); }));

    // The session is still there to be listed, which is the reader-visible
    // form of "not killed" — a session that exists but no picker shows is not
    // a session anybody can come back to.
    client.session.request(ckm::proto::ListSessions{});
    CK_CHECK(run_until(server, clock, client, [&] {
        for (const ckm::proto::SessionInfo& summary : client.listed)
            if (summary.id == id) return true;
        return false;
    }));

    forget(socket);
}

CK_TEST(a_second_client_takes_the_session_over_and_the_first_is_told_why) {
    // The session model: "Already attached elsewhere: **automatic takeover — latest
    // client wins**; the previous client is detached first, informed
    // best-effort."
    //
    // The half that is easy to get wrong is "informed": a takeover that simply
    // stopped sending to the old client would satisfy "latest wins" and leave
    // a reader staring at a frozen screen with no idea why.
    const std::filesystem::path socket = private_socket("takeover");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Session& session = server.create_session("contested");
    const std::uint64_t id = session.id;
    (void)server.open_terminal(id, spec_running("sleep 30"));

    Client first;
    CK_CHECK(first.connect(socket));
    first.greet();
    ckm::proto::DetachReason told = ckm::proto::DetachReason::User;
    bool was_told = false;
    first.session.on_detached = [&](ckm::proto::DetachReason reason, const std::string&) {
        told = reason;
        was_told = true;
    };
    first.session.attach(id, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, first, [&] { return first.session.attached(); }));

    Client second;
    CK_CHECK(second.connect(socket));
    second.greet();
    second.session.attach(id, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until_both(server, clock, first, second, [&] { return second.session.attached(); }));

    // Latest wins, and the loser knows.
    CK_CHECK(second.session.attached());
    CK_CHECK(run_until_both(server, clock, first, second, [&] { return was_told; }));
    CK_CHECK(was_told);
    CK_CHECK(!first.session.attached());
    CK_CHECK(told == ckm::proto::DetachReason::Takeover);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(killing_a_session_tells_every_reader_watching_it_and_says_why) {
    // The session model: "Attached **clients** get `detached(reason=session-killed)` and
    // fall back to the session picker."
    //
    // Plural, and this case is plural, because the first version of it was not
    // and that is a defect rather than a shortcut. A claim about "clients"
    // asserted with one client is a claim tested against nobody: the code path
    // that tells the SECOND reader is exactly the one a single-reader test
    // cannot reach, and it is the one that goes wrong — a loop that breaks after
    // the first, a lookup that finds only the newest, a detach that unbinds the
    // session before the rest have been told.
    //
    // The lesson is owed to a defect the owner found by using the app on
    // 2026-08-20: `a_reader_resizing_their_own_window_changes_nothing_about_the
    // _session` forbade a resize from moving the desktop, under all three
    // policies, with ONE reader attached — so it did not merely miss the bug
    // that cut a lone reader's windows off the screen, it required it. A
    // prohibition, or a promise, made in the absence of the actor it concerns
    // has been tested against nobody.
    const std::filesystem::path socket = private_socket("kill-session");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Session& session = server.create_session("doomed");
    const std::uint64_t id = session.id;
    (void)server.open_terminal(id, spec_running("sleep 30"));
    // A server ends with its LAST session (the session model), so a survivor keeps this
    // case measuring a session being killed rather than a server shutting down
    // — the reason code would be ServerShutdown instead, and the assertion
    // below would pass for the wrong reason.
    ckm::server::Session& survivor = server.create_session("survivor");
    (void)server.open_terminal(survivor.id, spec_running("sleep 30"));

    // Two readers, both sharing. Sharing is opt-in (WP-44) and the default is
    // takeover, so without `set_share` the second attach would DETACH the first
    // — and this case would then assert the plural claim against one reader
    // again, while appearing to use two.
    Client first;
    Client second;
    ckm::proto::DetachReason first_told = ckm::proto::DetachReason::User;
    ckm::proto::DetachReason second_told = ckm::proto::DetachReason::User;
    bool first_heard = false;
    bool second_heard = false;
    for (auto* pair : {&first, &second}) {
        CK_CHECK(pair->connect(socket));
        pair->greet();
        pair->session.set_share(true);
    }
    first.session.on_detached = [&](ckm::proto::DetachReason reason, const std::string&) {
        first_told = reason;
        first_heard = true;
    };
    second.session.on_detached = [&](ckm::proto::DetachReason reason, const std::string&) {
        second_told = reason;
        second_heard = true;
    };
    first.session.attach(id, ckv::Size{80, 24}, ckv::Size{9, 18});
    second.session.attach(id, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until_both(server, clock, first, second,
                            [&] { return first.session.attached() && second.session.attached(); }));
    // The positive partner: both really are attached at once, so what follows
    // is about a kill reaching two readers rather than about a takeover having
    // quietly left one.
    CK_CHECK(first.session.attached());
    CK_CHECK(second.session.attached());
    CK_CHECK(!first_heard);
    CK_CHECK(!second_heard);

    ckm::proto::KillSession kill;
    kill.session = id;
    first.session.request(kill);
    CK_CHECK(run_until_both(server, clock, first, second,
                            [&] { return first_heard && second_heard; }, kChildPasses));

    std::printf("  [m3] kill-session: first told=%s second told=%s\n",
                first_heard ? "yes" : "no", second_heard ? "yes" : "no");
    // Both told, both told WHY, and both unbound. The reason is the point: a
    // client detached without one cannot tell a kill from a takeover, and the
    // two want opposite responses from the reader.
    CK_CHECK(first_heard);
    CK_CHECK(second_heard);
    CK_CHECK(first_told == ckm::proto::DetachReason::SessionKilled);
    CK_CHECK(second_told == ckm::proto::DetachReason::SessionKilled);
    CK_CHECK(!first.session.attached());
    CK_CHECK(!second.session.attached());

    server.terminals().close_all();
    forget(socket);
}


// A helper for the four name-collision edges below, all of which need a
// session to exist before the interesting request is made.
namespace {
std::uint64_t make_session(Server& server, ckv::ManualClock& clock, Client& client,
                           const std::string& name) {
    ckm::proto::NewSession request;
    request.name = name;
    client.session.request(request);
    std::uint64_t made = 0;
    (void)run_until(server, clock, client, [&] {
        client.session.request(ckm::proto::ListSessions{});
        for (const ckm::proto::SessionInfo& summary : client.listed)
            if (summary.name == name) { made = summary.id; return true; }
        return false;
    }, kChildPasses);
    return made;
}
}  // namespace

CK_TEST(a_name_another_session_holds_is_refused_at_creation_and_at_rename) {
    // The session model, both rows, after the owner's ruling of 2026-08-20 resolved what
    // had been a contradiction between them:
    //
    //   `new-session`     "Name collision → reject with `NameTaken`."
    //   `rename-session`  "Name collision → reject with `NameTaken`, the same
    //                      rule as `new-session`… a name a reader cannot take
    //                      at creation is one they could otherwise reach in two
    //                      steps by renaming into it."
    //
    // Both halves are asserted here precisely because the second exists to
    // close the first: a build that refused at creation and allowed it at
    // rename would satisfy the `new-session` row completely and defeat it.
    const std::filesystem::path socket = private_socket("name-taken");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    CK_CHECK(make_session(server, clock, client, "taken") != 0U);
    const std::uint64_t other = make_session(server, clock, client, "mine");
    CK_CHECK(other != 0U);
    const std::size_t before_create = client.errors.size();

    // Refused at creation, and named: a refusal a reader cannot act on is not
    // much better than silence, and the UI needs the name to suggest `taken-2`.
    ckm::proto::NewSession clash;
    clash.name = "taken";
    client.session.request(clash);
    CK_CHECK(run_until(server, clock, client, [&] { return client.errors.size() > before_create; }));
    CK_CHECK(client.errors.back().context == "NewSession");
    CK_CHECK(client.errors.back().code ==
             static_cast<std::uint16_t>(ckm::proto::ErrorCode::NameTaken));
    CK_CHECK(client.errors.back().human.find("taken") != std::string::npos);

    // And refused at rename, with its own context so a reader can tell which
    // of the two requests failed.
    const std::size_t before_rename = client.errors.size();
    ckm::proto::RenameSession into;
    into.id = other;
    into.name = "taken";
    client.session.request(into);
    CK_CHECK(run_until(server, clock, client, [&] { return client.errors.size() > before_rename; }));
    CK_CHECK(client.errors.back().context == "RenameSession");
    CK_CHECK(client.errors.back().code ==
             static_cast<std::uint16_t>(ckm::proto::ErrorCode::NameTaken));

    // Both refusals took effect rather than merely being reported — the
    // positive partner, since a server that errored AND applied the change
    // would satisfy every assertion above.
    client.session.request(ckm::proto::ListSessions{});
    CK_CHECK(run_until(server, clock, client, [&] { return client.listed.size() >= 2U; }));
    std::size_t named = 0;
    std::size_t still_mine = 0;
    for (const ckm::proto::SessionInfo& summary : client.listed) {
        if (summary.name == "taken") ++named;
        if (summary.name == "mine") ++still_mine;
    }
    std::printf("  [m3] name collisions refused: sessions called \"taken\"=%zu, \"mine\" intact=%zu\n",
                named, still_mine);
    CK_CHECK(named == 1U);
    CK_CHECK(still_mine == 1U);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(renaming_a_session_to_the_name_it_already_has_is_not_a_refusal) {
    // The session model: "Renaming a session to the name it ALREADY has is a no-op, not
    // a refusal — confirming a dialog unchanged is not a request."
    //
    // The edge the collision rule creates and must then exempt: the session
    // holding the name IS a session holding the name, so a naive check refuses
    // the reader who opened the rename dialog and pressed OK without typing.
    // That is the commonest way anyone would ever meet this error.
    const std::filesystem::path socket = private_socket("rename-self");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    const std::uint64_t id = make_session(server, clock, client, "unchanged");
    CK_CHECK(id != 0U);
    const std::size_t before = client.errors.size();

    ckm::proto::RenameSession same;
    same.id = id;
    same.name = "unchanged";
    client.session.request(same);
    for (int pass = 0; pass < 120; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        client.pump();
    }
    std::printf("  [m3] rename-to-own-name: errors raised=%zu\n", client.errors.size() - before);
    CK_CHECK(client.errors.size() == before);

    // And it still has the name — a "no-op" that cleared it would also raise
    // no error.
    client.session.request(ckm::proto::ListSessions{});
    CK_CHECK(run_until(server, clock, client, [&] {
        for (const ckm::proto::SessionInfo& summary : client.listed)
            if (summary.id == id) return summary.name == "unchanged";
        return false;
    }));

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(session_names_compare_exactly_so_case_makes_two_names) {
    // The session model: "Names compare exactly: `Build` and `build` are two names,
    // because case folding is a locale question this has no business
    // answering."
    //
    // Worth pinning as a decision rather than an accident: the obvious
    // "improvement" is a case-insensitive compare, and it would be wrong in
    // Turkish before it was wrong anywhere else. A test is what stops somebody
    // adding it as a courtesy.
    const std::filesystem::path socket = private_socket("name-case");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    CK_CHECK(make_session(server, clock, client, "build") != 0U);
    const std::size_t before = client.errors.size();

    CK_CHECK(make_session(server, clock, client, "Build") != 0U);
    std::printf("  [m3] \"build\" and \"Build\": errors raised=%zu\n",
                client.errors.size() - before);
    CK_CHECK(client.errors.size() == before);

    client.session.request(ckm::proto::ListSessions{});
    CK_CHECK(run_until(server, clock, client, [&] { return client.listed.size() >= 2U; }));
    std::size_t lower = 0;
    std::size_t upper = 0;
    for (const ckm::proto::SessionInfo& summary : client.listed) {
        if (summary.name == "build") ++lower;
        if (summary.name == "Build") ++upper;
    }
    CK_CHECK(lower == 1U);
    CK_CHECK(upper == 1U);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(moving_a_terminal_into_the_session_it_is_already_in_changes_nothing) {
    // The session model, the `move-terminal` edge: "Target = source → no-op."
    //
    // Worth pinning because the implementation is a reparent — remove from one
    // list, add to another — and the degenerate case of that is a removal
    // followed by an add of the same id, which is a window closing and
    // reopening on every watcher's screen for no reason.
    const std::filesystem::path socket = private_socket("move-self");
    forget(socket);
    ckv::ManualClock clock;
    Server server(Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == Server::StartStatus::Listening);
    ckm::server::Session& home = server.create_session("home");
    ckm::server::Terminal& terminal = server.open_terminal(home.id, spec_running("sleep 30"));
    const std::uint64_t term = terminal.id();

    Client client;
    CK_CHECK(client.connect(socket));
    client.greet();
    client.session.attach(home.id, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(run_until(server, clock, client, [&] { return client.session.attached(); }));
    CK_CHECK(run_until(server, clock, client,
                       [&] { return client.session.terminal(term) != nullptr; }, kChildPasses));

    const std::size_t closed_before = client.count_of<ckm::proto::TermClosed>();
    const std::size_t opened_before = client.count_of<ckm::proto::TermOpened>();

    ckm::proto::MoveTerminal move;
    move.term = term;
    // `destination_session`, not `destination`: the session model's operations table
    // documents the wire as `MoveTerminal{term, destination, to_new_session}`
    // and the field has been `destination_session` since it was written. A
    // small drift, and the kind this suite exists to surface — the plan is the
    // spec, and a spec that names a field which does not exist cannot be
    // implemented from.
    move.destination_session = home.id;   // the session it is already in
    move.to_new_session = 0;
    client.session.request(move);
    for (int pass = 0; pass < 120; ++pass) {
        clock.advance(34'000'000);
        CK_CHECK(server.step());
        client.pump();
    }

    // Nothing happened, stated in the three ways a reader would notice: the
    // terminal is still here, no window closed, and none reopened.
    CK_CHECK(server.terminals().find(term) != nullptr);
    CK_CHECK(client.session.terminal(term) != nullptr);
    std::printf("  [m3] move-to-self: TermClosed +%zu, TermOpened +%zu\n",
                client.count_of<ckm::proto::TermClosed>() - closed_before,
                client.count_of<ckm::proto::TermOpened>() - opened_before);
    CK_CHECK(client.count_of<ckm::proto::TermClosed>() == closed_before);
    CK_CHECK(client.count_of<ckm::proto::TermOpened>() == opened_before);

    server.terminals().close_all();
    forget(socket);
}
