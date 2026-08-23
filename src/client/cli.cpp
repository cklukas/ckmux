// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/cli.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "client/server_connection.hpp"
#include "common/config.hpp"
#include "common/proto.hpp"
#include "platform/socket.hpp"

namespace ckm::client {
namespace {

// The subcommands this build answers. One list, consulted by both the
// dispatch and the refusal, because two lists are how a word comes to be
// accepted by one and rejected by the other — the defect `keys_not_honoured_yet()`
// documents having made once already in the configuration.
constexpr std::string_view kSubcommands[] = {"ls", "new", "attach", "kill-session",
                                             "check-config"};

// Connects to a server that is already there, and completes the handshake.
//
// Deliberately does NOT start one, and shares that reasoning with
// `kill-server`: `ls` and `kill-session` are questions about what exists, and
// a question that brings its own subject into existence answers itself. A
// reader running `ckmux ls` on a machine with no server wants to be told so,
// not to be given one.
struct CliConnection {
    bool connected = false;
    platform::Stream stream;
    proto::FrameReader reader;
};

CliConnection connect_without_starting(const std::filesystem::path& socket) {
    CliConnection result;
    const platform::ConnectResult connected = platform::connect_to_server(socket);
    if (connected.status != platform::ConnectStatus::Connected) {
        if (connected.status == platform::ConnectStatus::NoServer)
            std::fprintf(stderr, "ckmux: no server is running at %s\n", socket.string().c_str());
        else
            std::fprintf(stderr, "ckmux: %s\n", connected.problem.c_str());
        return result;
    }
    result.stream = platform::Stream(connected.fd);
    proto::Hello hello;
    hello.build = std::string(proto::kBuildIdentity);
    hello.client_kind = proto::ClientKind::Cli;
    (void)result.stream.send(proto::encode(hello));
    (void)result.stream.flush();

    proto::Message answer;
    if (!await_message(result.stream, result.reader, answer)) {
        std::fprintf(stderr, "ckmux: the server did not answer the handshake\n");
        return result;
    }
    if (const auto* refuse = std::get_if<proto::Refuse>(&answer)) {
        // A version mismatch is fatal for a question — unlike `kill-server`,
        // which is worth doing to a server of any version — because the
        // answer would be decoded by a different codec than wrote it.
        std::fprintf(stderr, "ckmux: %s\n", refuse->reason.c_str());
        return result;
    }
    result.connected = true;
    return result;
}

// Asks for the session list and waits for it. Every subcommand here needs it:
// `ls` to print, `kill-session` and `attach` to turn the name a reader typed
// into the id the wire carries.
bool ask_for_sessions(CliConnection& connection, std::vector<proto::SessionInfo>& sessions) {
    (void)connection.stream.send(proto::encode(proto::ListSessions{}));
    (void)connection.stream.flush();
    proto::Message answer;
    if (!await_message(connection.stream, connection.reader, answer)) {
        std::fprintf(stderr, "ckmux: the server did not answer\n");
        return false;
    }
    const auto* list = std::get_if<proto::SessionList>(&answer);
    if (list == nullptr) {
        std::fprintf(stderr, "ckmux: the server answered something other than a session list\n");
        return false;
    }
    sessions = list->sessions;
    return true;
}

}  // namespace

bool is_cli_subcommand(const std::string& word) {
    return std::find(std::begin(kSubcommands), std::end(kSubcommands), word) !=
           std::end(kSubcommands);
}

CliRequest parse_cli(const std::vector<std::string>& arguments) {
    CliRequest request;
    if (arguments.empty()) return request;
    request.subcommand = arguments.front();

    if (request.subcommand == "ls" || request.subcommand == "check-config") {
        // Neither takes anything. A reader who typed something after them
        // meant something by it, and silently ignoring it is how `ckmux ls
        // work` comes to look like it filtered.
        if (arguments.size() > 1) {
            request.problem = "`" + request.subcommand + "` takes no arguments";
            request.usage = "ckmux " + request.subcommand;
        }
        return request;
    }

    if (request.subcommand == "new") {
        request.usage = "ckmux new [-s <name>] [command...]";
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const std::string& argument = arguments[index];
            if (argument == "-s" || argument == "--session") {
                if (index + 1 >= arguments.size()) {
                    request.problem = "`" + argument + "` needs a session name after it";
                    return request;
                }
                request.name = arguments[++index];
                continue;
            }
            // The first word that is not a flag begins the command, and
            // everything after it belongs to the command rather than to
            // ckmux — including words that look like flags, which is what
            // lets `ckmux new ls -l` mean what it says.
            for (std::size_t rest = index; rest < arguments.size(); ++rest) {
                if (rest > index) request.command += ' ';
                request.command += arguments[rest];
            }
            break;
        }
        return request;
    }

    if (request.subcommand == "attach") {
        request.usage = "ckmux attach [--share] [--watch] [--adopt-size] <name|id>";
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const std::string& word = arguments[index];
            if (word == "--share") {
                request.share = true;
                continue;
            }
            if (word == "--watch") {
                // Watching is a way of joining, so this sets both rather than
                // relying on every reader of `share` to remember that `watch`
                // implies it. `--share --watch` is then simply redundant.
                request.watch = true;
                request.share = true;
                continue;
            }
            if (word == "--adopt-size") {
                request.adopt_size = true;
                continue;
            }
            // Flags come before the session and nothing comes after it. An
            // unknown one is refused rather than taken for a session name: a
            // reader who mistypes `--adopt-siz` means the flag, and resolving
            // it as a session gives them "no such session `--adopt-siz`",
            // which sends them looking in the wrong place entirely.
            if (word.rfind("--", 0) == 0) {
                request.problem = "`attach` has no `" + word + "` flag";
                return request;
            }
            if (!request.name.empty()) {
                request.problem = "`attach` takes one session, not several";
                return request;
            }
            request.name = word;
        }
        if (request.name.empty()) {
            request.problem = "`attach` needs the session to act on";
            return request;
        }
        return request;
    }

    if (request.subcommand == "kill-session") {
        request.usage = "ckmux kill-session <name|id>";
        if (arguments.size() < 2) {
            request.problem = "`kill-session` needs the session to act on";
            return request;
        }
        if (arguments.size() > 2) {
            request.problem = "`kill-session` takes one session, not several";
            return request;
        }
        request.name = arguments[1];
        return request;
    }

    request.subcommand.clear();
    return request;
}

// Turns what a reader typed into the id the wire carries. A name first, then
// a bare number as an id — that order because a session NAMED "3" is a
// session a reader made and can see in `ckmux ls`, and resolving it to
// whatever session happens to hold id 3 would act on the wrong one.
//
// Returns 0 for "no match", and sets `ambiguous` when a name names more than
// one.
//
// That branch is DEFENSIVE, not live, and saying so is the point: a server of
// this build refuses a duplicate name at creation and at rename (the session model's
// two operation rows, made to agree on 2026-08-20), so it cannot produce a
// list this branch would fire on. It stays because the cost is a counter and
// the alternative — picking one of two silently — is how a reader kills the
// session they meant to keep. A comment that still claimed "two sessions may
// share a name" would be a true sentence about the old server read as a fact
// about this one.
namespace {

std::uint64_t resolve_session(const std::vector<proto::SessionInfo>& sessions,
                              const std::string& what, bool& ambiguous) {
    ambiguous = false;
    std::uint64_t found = 0;
    int matches = 0;
    for (const proto::SessionInfo& info : sessions)
        if (info.name == what) {
            ++matches;
            found = info.id;
        }
    if (matches > 1) {
        ambiguous = true;
        return 0;
    }
    if (matches == 1) return found;

    if (!what.empty() && what.find_first_not_of("0123456789") == std::string::npos) {
        const std::uint64_t asked = std::strtoull(what.c_str(), nullptr, 10);
        for (const proto::SessionInfo& info : sessions)
            if (info.id == asked) return info.id;
    }
    return 0;
}

}  // namespace

int run_ls(const std::filesystem::path& socket) {
    CliConnection connection = connect_without_starting(socket);
    if (!connection.connected) return 1;

    std::vector<proto::SessionInfo> sessions;
    if (!ask_for_sessions(connection, sessions)) return 1;

    if (sessions.empty()) {
        // Not an error: a server with no sessions is a server that has just
        // reaped its last one, and a script asking "what is there" got its
        // answer. Said on stdout, because it IS the answer.
        std::printf("no sessions\n");
        return 0;
    }
    // Name first, because a name is what the other subcommands take. The
    // columns are tab-free and single-spaced on purpose: this is output a
    // script will cut, and a table that aligns with padding is a table whose
    // column positions move when a session is renamed.
    for (const proto::SessionInfo& info : sessions) {
        std::printf("%s: %u terminal%s%s (id %llu)\n", info.name.c_str(),
                    static_cast<unsigned>(info.terminals), info.terminals == 1 ? "" : "s",
                    info.attached != 0 ? ", attached" : "",
                    static_cast<unsigned long long>(info.id));
    }
    return 0;
}

int run_new(const std::filesystem::path& socket, const std::filesystem::path& executable,
            const CliRequest& request) {
    // The one question here that DOES start a server: a reader asking for a
    // session on a machine with none is asking for a server too, and should
    // not have to know that (the same reasoning `run_client.cpp` states for
    // the picker's Create button).
    ServerConnection connection =
        connect_to_server(socket, executable, proto::ClientKind::Cli);
    if (!connection.ok()) {
        std::fprintf(stderr, "ckmux: %s\n", connection.problem.c_str());
        return 1;
    }
    proto::FrameReader reader;

    proto::NewSession ask;
    ask.name = request.name;
    // Both fields are honoured by the server as of WP-11's own finding: the
    // handler read neither, so `spawn_first` — which DEFAULTS to on — was the
    // silent half, and `ckmux new 'make -j8'` made an empty session and
    // dropped the command. One message still, rather than a session followed
    // by a `NewTerminal`: a CLI connection is not attached, and `NewTerminal`
    // requires a session to be attached to.
    ask.spawn_first = 1;
    ask.command = request.command;
    (void)connection.stream.send(proto::encode(ask));
    (void)connection.stream.flush();

    // The server answers a `NewSession` with the whole list, to this client
    // and every other — so the confirmation and the new session's name arrive
    // together, and the id is the highest one in it (ids only ever go up).
    proto::Message answer;
    if (!await_message(connection.stream, reader, answer)) {
        std::fprintf(stderr, "ckmux: the server did not confirm the new session\n");
        return 1;
    }
    // A refusal is an answer, and it is the one worth repeating word for word.
    // Without this the reader of `ckmux new -s build` on a name already taken
    // got "the server did not confirm the new session" — true, useless, and
    // indistinguishable from a server that had fallen over.
    if (const auto* refused = std::get_if<proto::Error>(&answer)) {
        std::fprintf(stderr, "ckmux: %s\n", refused->human.c_str());
        return 1;
    }
    const auto* list = std::get_if<proto::SessionList>(&answer);
    if (list == nullptr || list->sessions.empty()) {
        std::fprintf(stderr, "ckmux: the server did not confirm the new session\n");
        return 1;
    }
    const proto::SessionInfo* newest = &list->sessions.front();
    for (const proto::SessionInfo& info : list->sessions)
        if (info.id > newest->id) newest = &info;
    std::printf("%s\n", newest->name.c_str());
    return 0;
}

int run_kill_session(const std::filesystem::path& socket, const CliRequest& request) {
    CliConnection connection = connect_without_starting(socket);
    if (!connection.connected) return 1;

    std::vector<proto::SessionInfo> sessions;
    if (!ask_for_sessions(connection, sessions)) return 1;

    bool ambiguous = false;
    const std::uint64_t target = resolve_session(sessions, request.name, ambiguous);
    if (ambiguous) {
        std::fprintf(stderr,
                     "ckmux: more than one session is called \"%s\"; name it by id instead "
                     "(`ckmux ls` prints them)\n",
                     request.name.c_str());
        return 1;
    }
    if (target == 0) {
        std::fprintf(stderr, "ckmux: no session called \"%s\"\n", request.name.c_str());
        return 1;
    }

    proto::KillSession ask;
    ask.session = target;
    (void)connection.stream.send(proto::encode(ask));
    (void)connection.stream.flush();

    // A kill is asked, not done: the programs in it are given the grace the
    // configuration allows (the session model), so what is waited for here is the
    // server saying the session is gone — which it does by sending every
    // greeted client the list again.
    for (;;) {
        proto::Message answer;
        if (!await_message(connection.stream, connection.reader, answer, 10000)) {
            std::fprintf(stderr, "ckmux: the server did not confirm the kill\n");
            return 1;
        }
        if (const auto* error = std::get_if<proto::Error>(&answer)) {
            std::fprintf(stderr, "ckmux: %s\n", error->human.c_str());
            return 1;
        }
        const auto* list = std::get_if<proto::SessionList>(&answer);
        if (list == nullptr) continue;
        // The session is gone when it stops appearing in the list. A kill is
        // asked rather than done, so the first list may still hold it —
        // waiting for the next one is what makes this report "ended" rather
        // than "asked".
        const bool gone = std::none_of(
            list->sessions.begin(), list->sessions.end(),
            [target](const proto::SessionInfo& info) { return info.id == target; });
        if (gone) return 0;
    }
}

std::uint64_t resolve_attach_target(const std::filesystem::path& socket,
                                    const CliRequest& request) {
    CliConnection connection = connect_without_starting(socket);
    if (!connection.connected) return 0;

    std::vector<proto::SessionInfo> sessions;
    if (!ask_for_sessions(connection, sessions)) return 0;

    bool ambiguous = false;
    const std::uint64_t target = resolve_session(sessions, request.name, ambiguous);
    if (ambiguous) {
        std::fprintf(stderr,
                     "ckmux: more than one session is called \"%s\"; name it by id instead "
                     "(`ckmux ls` prints them)\n",
                     request.name.c_str());
        return 0;
    }
    if (target == 0)
        std::fprintf(stderr, "ckmux: no session called \"%s\"\n", request.name.c_str());
    return target;
}

int run_check_config(const std::filesystem::path& config) {
    // No socket, by design: a reader checking a file before starting anything
    // is exactly who this is for (the configuration spec).
    LoadedSettings loaded = load_settings(config);

    std::printf("%s\n", config.string().c_str());
    if (loaded.warnings.empty()) {
        std::printf("  no problems\n");
    } else {
        // Each warning already names its file and line in full — the loader
        // builds them that way so a reader can paste one into an editor —
        // so they are printed as they are rather than re-wrapped here.
        for (const std::string& warning : loaded.warnings) std::printf("  %s\n", warning.c_str());
    }

    // The second half, and the reason this subcommand is worth more than a
    // parse: a key that is spelled correctly, accepted, and reaches nothing
    // yet. Without this, "I set it and nothing happened" has no answer that is
    // neither a lie nor a warning about a documented key (the configuration spec).
    const std::vector<std::string> waiting = keys_not_honoured_yet();
    if (!waiting.empty()) {
        std::printf("\nRead, but not yet honoured by any landed package:\n");
        for (const std::string& key : waiting) std::printf("  %s\n", key.c_str());
    }
    return loaded.warnings.empty() ? 0 : 1;
}

}  // namespace ckm::client
