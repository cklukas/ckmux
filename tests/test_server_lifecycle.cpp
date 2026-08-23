// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-2, on the wire and against a real process. Every case here starts an
// actual `ckmux --server` on a private `CKMUX_SOCKET` and talks the actual
// protocol to it: a server that only ever runs in-process is not a server, and
// the lifecycle is exactly the part that only a process can be wrong about —
// detaching, binding, racing, dying, and being asked to stop.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "client/server_connection.hpp"
#include "common/proto.hpp"
#include "platform/paths.hpp"
#include "platform/poller.hpp"
#include "platform/process.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

using clock_type = std::chrono::steady_clock;

std::filesystem::path binary_path() {
#if defined(CKMUX_BINARY_PATH)
    return std::filesystem::path(CKMUX_BINARY_PATH);
#else
    return {};
#endif
}

// A socket path of this test's own, in a directory nothing else uses. Short, on
// purpose: `sun_path` is 104 bytes on macOS, and a build directory nested a
// little deeper than usual is exactly how a test comes to fail for a reason
// that has nothing to do with what it is testing.
std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-t" + std::to_string(static_cast<unsigned long>(::getpid()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (std::string(name) + ".sock");
}

void forget(const std::filesystem::path& socket) {
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    // And the directory, which only succeeds once it is empty — so the last
    // case to finish tidies up and the others leave it alone. A test suite that
    // litters a machine's temporary directory with one entry per run is a test
    // suite somebody eventually has to clean up by hand.
    std::filesystem::remove(ckm::platform::server_log_path(socket), ignored);
    std::filesystem::remove(socket.parent_path(), ignored);
}

// Starts a server as a plain child process — `--foreground`, so the test owns it
// and can wait for it. The detaching path is covered on its own below; every
// other case wants a process it can end deterministically.
::pid_t start_foreground_server(const std::filesystem::path& socket) {
    const ::pid_t child = ::fork();
    if (child != 0) return child;
    const std::string program = binary_path().string();
    const std::string path = socket.string();
    char* const argv[] = {const_cast<char*>(program.c_str()), const_cast<char*>("--server"),
                          const_cast<char*>(path.c_str()), const_cast<char*>("--foreground"),
                          nullptr};
    // The server's own diagnostics would otherwise interleave with the test
    // report; a foreground server writes them to stderr.
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
        (void)::dup2(null_fd, STDERR_FILENO);
        (void)::close(null_fd);
    }
    ::execv(program.c_str(), argv);
    ::_exit(127);
}

// The exact mirror of `wait_for_socket`, and the reason it did not exist is
// the reason fifteen orphaned servers were running on this machine.
//
// Every case that ends a server sent `KillServer`, discarded the answer —
// literally `(void)closer.hear(ignored, 2000)` — and then called `forget()`,
// which unlinks the socket file. When the kill was not acted on, the test
// passed anyway and left a server that is not merely leaked but **permanently
// unreachable**: it is still listening on a path that no longer exists, so no
// client can ever connect to ask it to stop again. That is why they accumulate
// across runs instead of being cleaned up by the next one, and why `pkill` was
// the only thing that ever removed them.
//
// So the death is now asserted, before the socket is taken away. A leak
// becomes a test failure, which is the only thing that stops it recurring — and
// deliberately NOT a reaper in the harness, because a reaper hides the defect
// and makes the next `pgrep`-based diagnosis lie about what is running.
bool wait_for_server_gone(const std::filesystem::path& socket, int budget_ms = 4000) {
    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(budget_ms);
    for (;;) {
        ckm::platform::ConnectResult probe = ckm::platform::connect_to_server(socket);
        if (probe.status == ckm::platform::ConnectStatus::Connected) {
            (void)::close(probe.fd);
        } else {
            return true;  // nothing is listening there any more
        }
        if (clock_type::now() >= deadline) return false;
        ::usleep(5000);
    }
}

bool wait_for_socket(const std::filesystem::path& socket, int budget_ms = 4000) {
    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(budget_ms);
    for (;;) {
        ckm::platform::ConnectResult probe = ckm::platform::connect_to_server(socket);
        if (probe.status == ckm::platform::ConnectStatus::Connected) {
            (void)::close(probe.fd);
            return true;
        }
        if (clock_type::now() >= deadline) return false;
        ::usleep(5000);
    }
}

bool reap_within(::pid_t child, int budget_ms) {
    const clock_type::time_point deadline = clock_type::now() + std::chrono::milliseconds(budget_ms);
    for (;;) {
        int status = 0;
        const ::pid_t seen = ::waitpid(child, &status, WNOHANG);
        if (seen == child) return true;
        if (seen < 0) return true;  // somebody else reaped it, or it was never ours
        if (clock_type::now() >= deadline) return false;
        ::usleep(5000);
    }
}

void end_server(::pid_t child) {
    if (child <= 0) return;
    (void)::kill(child, SIGTERM);
    if (!reap_within(child, 3000)) {
        (void)::kill(child, SIGKILL);
        (void)reap_within(child, 2000);
    }
}

// A client on the wire, with none of the client's own machinery: connect, say
// something, read what comes back.
struct WireClient {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader reader;

    bool connect(const std::filesystem::path& socket) {
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        if (result.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = ckm::platform::Stream(result.fd);
        return true;
    }

    void say(const ckm::proto::Message& message) { (void)stream.send(ckm::proto::encode(message)); }

    bool hear(ckm::proto::Message& message, int budget_ms = 3000) {
        return ckm::client::await_message(stream, reader, message, budget_ms);
    }

    // Says Hello at `version` and returns what came back.
    bool greet(ckm::proto::Message& answer, std::uint32_t version = ckm::proto::kProtocolVersion,
              ckm::proto::ClientKind kind = ckm::proto::ClientKind::Ui) {
        ckm::proto::Hello hello;
        hello.proto_version = version;
        hello.build = "a test";
        hello.client_kind = kind;
        say(hello);
        return hear(answer);
    }
};

// One environment variable, put back however the case leaves — the cases below
// change where ckmux would look for things, and a suite in one process cannot
// let that outlive the case that wanted it.
class ScopedVariable {
public:
    explicit ScopedVariable(std::string name) : name_(std::move(name)) {
        const char* const current = std::getenv(name_.c_str());
        had_value_ = current != nullptr;
        if (had_value_) previous_ = current;
    }
    ~ScopedVariable() {
        if (had_value_) (void)::setenv(name_.c_str(), previous_.c_str(), 1);
        else (void)::unsetenv(name_.c_str());
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

    void set(const std::string& value) { (void)::setenv(name_.c_str(), value.c_str(), 1); }
    void clear() { (void)::unsetenv(name_.c_str()); }

private:
    std::string name_;
    std::string previous_;
    bool had_value_ = false;
};

bool is_close_on_exec(int fd) {
    if (fd < 0) return false;
    const int flags = ::fcntl(fd, F_GETFD, 0);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

bool is_non_blocking(int fd) {
    if (fd < 0) return false;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && (flags & O_NONBLOCK) != 0;
}

}  // namespace

CK_TEST(a_client_that_finds_no_server_starts_one_and_the_server_outlives_it) {
    // Lazy start, which is the behaviour a reader actually meets: they run
    // ckmux, and afterwards a server exists whether or not one did before. And
    // it is a DETACHED server — the whole point — so this is also the case that
    // proves the double fork: the process the client forked has already exited,
    // and the thing listening is a grandchild owned by nobody.
    const std::filesystem::path socket = private_socket("lazy");
    forget(socket);
    CK_CHECK(!binary_path().empty());
    if (binary_path().empty()) return;

    ckm::client::ServerConnection connection =
        ckm::client::connect_to_server(socket, binary_path(), ckm::proto::ClientKind::Cli, 4000);
    CK_CHECK(connection.ok());
    CK_CHECK(connection.problem.empty());
    CK_CHECK(connection.server_build == std::string(ckm::proto::kBuildIdentity));

    // The socket is owner-only, in an owner-only directory (the architecture spec's security
    // posture). A socket anybody can reach is a terminal anybody can type into.
    struct ::stat socket_info{};
    CK_CHECK(::stat(socket.c_str(), &socket_info) == 0);
    CK_CHECK((socket_info.st_mode & (S_IRWXG | S_IRWXO)) == 0);
    struct ::stat directory_info{};
    CK_CHECK(::stat(socket.parent_path().c_str(), &directory_info) == 0);
    CK_CHECK((directory_info.st_mode & (S_IRWXG | S_IRWXO)) == 0);

    // The client goes away; the server does not. This is the promise the whole
    // project exists for, checked at its smallest scale.
    connection.stream.close();
    CK_CHECK(wait_for_socket(socket, 1000));

    // And a second client finds the running one rather than starting another.
    ckm::client::ServerConnection second =
        ckm::client::connect_to_server(socket, binary_path(), ckm::proto::ClientKind::Cli, 2000);
    CK_CHECK(second.ok());
    second.stream.close();

    WireClient closer;
    CK_CHECK(closer.connect(socket));
    ckm::proto::Message answer;
    CK_CHECK(closer.greet(answer));
    closer.say(ckm::proto::KillServer{});
    (void)closer.stream.flush();
    ckm::proto::Message ignored;
    (void)closer.hear(ignored, 2000);
    // Asserted, and asserted BEFORE `forget()` unlinks the socket. This is the
    // whole of the orphan fix: the acknowledgement above is discarded, so
    // without this the test passes on a server that never died — and then
    // removes the only path anything could reach it by.
    CK_CHECK(wait_for_server_gone(socket));
    forget(socket);
}

CK_TEST(two_starters_race_and_exactly_one_server_ends_up_listening) {
    // The race that a bind alone cannot settle: a stale socket file has to be
    // unlinked before a bind can succeed, so two starters that each unlink and
    // bind BOTH succeed — and the first one is left listening on a socket file
    // nobody will ever reach again. The start lock is what makes the loser
    // notice; this drives it with real processes, several at once.
    const std::filesystem::path socket = private_socket("race");
    forget(socket);
    if (binary_path().empty()) return;

    // Eight of them at once, all told to listen on the same path. Seven have to
    // work out that they have lost.
    std::vector<::pid_t> starters;
    for (int index = 0; index < 8; ++index) starters.push_back(start_foreground_server(socket));
    CK_CHECK(wait_for_socket(socket, 4000));

    // Whoever won is answering, and answering as one server: the id a server
    // hands its clients starts at 1, so two connections that both get told
    // "you are client 1" would mean two servers behind one path. There is no
    // message that carries the id, so the check is the one that matters instead
    // — every connection reaches something that completes the handshake.
    for (int attempt = 0; attempt < 4; ++attempt) {
        WireClient client;
        CK_CHECK(client.connect(socket));
        ckm::proto::Message answer;
        CK_CHECK(client.greet(answer));
        CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(answer));
    }

    // Exactly one is still running. The seven that lost exited on their own,
    // which is what "not an error" means for this race: nothing was logged as a
    // failure, nothing needed retrying by hand.
    //
    // Waited for rather than sampled. A process that has decided to exit is not
    // instantly reapable, so counting once caught a loser mid-teardown and read
    // it as a second server — a flake that showed up about one run in twenty,
    // which is exactly the kind a lifecycle test must not have.
    std::vector<::pid_t> outstanding(starters);
    int alive = 0;
    const clock_type::time_point settled = clock_type::now() + std::chrono::milliseconds(3000);
    for (;;) {
        alive = 0;
        for (::pid_t& starter : outstanding) {
            if (starter <= 0) continue;
            int status = 0;
            if (::waitpid(starter, &status, WNOHANG) == starter) {
                CK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
                starter = -1;
                continue;
            }
            ++alive;
        }
        if (alive <= 1 || clock_type::now() >= settled) break;
        ::usleep(5000);
    }
    CK_CHECK(alive == 1);
    for (const ::pid_t starter : starters) end_server(starter);
    forget(socket);
}

CK_TEST(a_socket_file_left_by_a_dead_server_is_replaced_rather_than_believed) {
    // What a crashed server leaves behind, and what a reader would otherwise
    // have to delete by hand before ckmux would start again. `connect` fails
    // with ECONNREFUSED rather than ENOENT — the file is there and nothing is
    // listening — and that is the case a client must read as "start one".
    const std::filesystem::path socket = private_socket("stale");
    forget(socket);
    if (binary_path().empty()) return;

    // A socket file with nothing behind it: bound, then the process that bound
    // it is gone. Killing a server with SIGKILL is exactly how this happens for
    // real, so that is how it is made here.
    const ::pid_t doomed = start_foreground_server(socket);
    CK_CHECK(wait_for_socket(socket, 4000));
    (void)::kill(doomed, SIGKILL);
    CK_CHECK(reap_within(doomed, 3000));
    CK_CHECK(std::filesystem::exists(socket));  // the file survives the process

    const ckm::platform::ConnectResult refused = ckm::platform::connect_to_server(socket);
    CK_CHECK(refused.status == ckm::platform::ConnectStatus::NoServer);

    ckm::client::ServerConnection connection =
        ckm::client::connect_to_server(socket, binary_path(), ckm::proto::ClientKind::Cli, 4000);
    CK_CHECK(connection.ok());
    connection.stream.close();

    WireClient closer;
    CK_CHECK(closer.connect(socket));
    ckm::proto::Message answer;
    CK_CHECK(closer.greet(answer));
    closer.say(ckm::proto::KillServer{});
    (void)closer.stream.flush();
    ckm::proto::Message ignored;
    (void)closer.hear(ignored, 2000);
    // Asserted, and asserted BEFORE `forget()` unlinks the socket. This is the
    // whole of the orphan fix: the acknowledgement above is discarded, so
    // without this the test passes on a server that never died — and then
    // removes the only path anything could reach it by.
    CK_CHECK(wait_for_server_gone(socket));
    forget(socket);
}

CK_TEST(a_client_of_another_protocol_version_is_refused_with_the_remedy_in_words) {
    const std::filesystem::path socket = private_socket("version");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_foreground_server(socket);
    CK_CHECK(wait_for_socket(socket, 4000));

    WireClient stranger;
    CK_CHECK(stranger.connect(socket));
    ckm::proto::Message answer;
    CK_CHECK(stranger.greet(answer, ckm::proto::kProtocolVersion + 1));
    const auto* refuse = std::get_if<ckm::proto::Refuse>(&answer);
    CK_CHECK(refuse != nullptr);
    if (refuse != nullptr) {
        // Both numbers and something to do about it. "Version mismatch" on its
        // own is the message this test exists to prevent: it is the one a reader
        // meets after an upgrade, and it has to say which two things met and how
        // to get out of it.
        CK_CHECK(refuse->reason.find(std::to_string(ckm::proto::kProtocolVersion)) !=
                 std::string::npos);
        CK_CHECK(refuse->reason.find(std::to_string(ckm::proto::kProtocolVersion + 1)) !=
                 std::string::npos);
        CK_CHECK(refuse->reason.find("kill-server") != std::string::npos);
    }
    // And the connection is over, rather than left half-open in a state neither
    // end understands.
    ckm::proto::Message nothing;
    CK_CHECK(!stranger.hear(nothing, 500));

    // The server is unharmed: one refused client is not a reason to stop
    // serving the others.
    WireClient ordinary;
    CK_CHECK(ordinary.connect(socket));
    ckm::proto::Message greeting;
    CK_CHECK(ordinary.greet(greeting));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(greeting));

    end_server(server);
    forget(socket);
}

CK_TEST(kill_server_can_still_reach_a_server_of_another_protocol_version) {
    // Reported live (2026-08-18): a reader upgraded ckmux and could not run
    // `ckmux kill-server` against the old server this refusal names, because
    // the CLI's own Hello was refused and dropped before `KillServer` could
    // ever be sent — the fix this refusal names was the one thing it made
    // unreachable. `KillServer` carries no payload and needs no protocol
    // agreement, so a CLI client gets one bounded chance to send exactly that
    // after a mismatch, and the server acts on it.
    const std::filesystem::path socket = private_socket("kill-mismatch");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_foreground_server(socket);
    CK_CHECK(wait_for_socket(socket, 4000));

    WireClient cli;
    CK_CHECK(cli.connect(socket));
    ckm::proto::Message answer;
    CK_CHECK(cli.greet(answer, ckm::proto::kProtocolVersion + 1, ckm::proto::ClientKind::Cli));
    CK_CHECK(std::get_if<ckm::proto::Refuse>(&answer) != nullptr);

    // Held open rather than dropped — the whole point of this test is that
    // the connection survives the mismatch long enough to carry one more
    // message.
    cli.say(ckm::proto::KillServer{});
    CK_CHECK(reap_within(server, 4000));
    forget(socket);
}

CK_TEST(a_mismatched_cli_that_does_not_ask_to_kill_is_dropped_within_the_window) {
    // The other half of the same fix: the exception is for `KillServer`
    // specifically, not a general amnesty for a CLI client's version. A
    // connection that says nothing else, or asks for something else, does not
    // hold a slot open indefinitely — bounded and dropped, exactly as an
    // ordinary mismatch is (m-plat: no unbounded connection is free just
    // because nobody asked to close it).
    const std::filesystem::path socket = private_socket("kill-mismatch-silent");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_foreground_server(socket);
    CK_CHECK(wait_for_socket(socket, 4000));

    WireClient cli;
    CK_CHECK(cli.connect(socket));
    ckm::proto::Message answer;
    CK_CHECK(cli.greet(answer, ckm::proto::kProtocolVersion + 1, ckm::proto::ClientKind::Cli));
    CK_CHECK(std::get_if<ckm::proto::Refuse>(&answer) != nullptr);

    // Nothing sent. Past the server's own bounded window, the connection ends
    // on its own — the reader gets EOF, not an indefinite hang — and the
    // server is still standing to prove the wait did not cost it anything.
    ckm::proto::Message nothing;
    CK_CHECK(!cli.hear(nothing, 3000));

    WireClient ordinary;
    CK_CHECK(ordinary.connect(socket));
    ckm::proto::Message greeting;
    CK_CHECK(ordinary.greet(greeting));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(greeting));

    end_server(server);
    forget(socket);
}

CK_TEST(a_connection_that_does_not_say_hello_first_is_refused) {
    const std::filesystem::path socket = private_socket("nohello");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_foreground_server(socket);
    CK_CHECK(wait_for_socket(socket, 4000));

    WireClient rude;
    CK_CHECK(rude.connect(socket));
    rude.say(ckm::proto::Ping{7});
    ckm::proto::Message answer;
    CK_CHECK(rude.hear(answer));
    CK_CHECK(std::holds_alternative<ckm::proto::Refuse>(answer));

    end_server(server);
    forget(socket);
}

CK_TEST(kill_server_stops_it_and_leaves_no_socket_behind) {
    const std::filesystem::path socket = private_socket("kill");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_foreground_server(socket);
    CK_CHECK(wait_for_socket(socket, 4000));

    WireClient client;
    CK_CHECK(client.connect(socket));
    ckm::proto::Message answer;
    CK_CHECK(client.greet(answer));
    CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(answer));

    // A round trip first, so this test also says the loop is alive and not
    // merely that a process exists.
    client.say(ckm::proto::Ping{4242});
    ckm::proto::Message pong;
    CK_CHECK(client.hear(pong));
    const auto* answered = std::get_if<ckm::proto::Pong>(&pong);
    CK_CHECK(answered != nullptr);
    if (answered != nullptr) CK_CHECK(answered->nonce == 4242U);

    client.say(ckm::proto::KillServer{});
    (void)client.stream.flush();
    CK_CHECK(reap_within(server, 4000));

    // The socket goes with it: a server that left its socket behind would make
    // the next client wait out a connect to nothing before deciding to start
    // one, every time, forever.
    CK_CHECK(!std::filesystem::exists(socket));
    const ckm::platform::ConnectResult gone = ckm::platform::connect_to_server(socket);
    CK_CHECK(gone.status == ckm::platform::ConnectStatus::NoServer);
    forget(socket);
}

CK_TEST(asking_for_a_session_is_answered_with_the_sessions_there_now) {
    // A client waiting for a reply that never comes looks exactly like a server
    // that has hung, and a reader cannot tell those apart. Every request the
    // catalogue carries is answered; this one used to be answered with "not
    // implemented" and is now answered with the thing it asked for.
    const std::filesystem::path socket = private_socket("notyet");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_foreground_server(socket);
    CK_CHECK(wait_for_socket(socket, 4000));

    WireClient client;
    CK_CHECK(client.connect(socket));
    ckm::proto::Message answer;
    CK_CHECK(client.greet(answer));

    ckm::proto::NewSession request;
    request.name = "work";
    // Asked to be empty, explicitly: `spawn_first` is honoured now (49d1878),
    // and this case is about the list-reply contract, not the spawn policy —
    // `terminals == 0` was leaning on the field being silently ignored.
    // test_server_loop owns the spawn policy's own cases, both ways.
    request.spawn_first = 0;
    client.say(request);
    ckm::proto::Message reply;
    CK_CHECK(client.hear(reply));
    // The whole list, not just an id: making a session changes what every
    // client's picker should be showing, and the answer says what is there now.
    const auto* list = std::get_if<ckm::proto::SessionList>(&reply);
    CK_CHECK(list != nullptr);
    if (list != nullptr) {
        CK_CHECK(list->sessions.size() == 1U);
        CK_CHECK(list->sessions.front().name == "work");
        CK_CHECK(list->sessions.front().terminals == 0);
    }
    // And the connection carries on afterwards.
    client.say(ckm::proto::Ping{1});
    ckm::proto::Message pong;
    CK_CHECK(client.hear(pong));
    CK_CHECK(std::holds_alternative<ckm::proto::Pong>(pong));

    end_server(server);
    forget(socket);
}

CK_TEST(a_frame_the_server_cannot_read_ends_that_connection_and_no_other) {
    const std::filesystem::path socket = private_socket("garbage");
    forget(socket);
    if (binary_path().empty()) return;
    const ::pid_t server = start_foreground_server(socket);
    CK_CHECK(wait_for_socket(socket, 4000));

    WireClient good;
    CK_CHECK(good.connect(socket));
    ckm::proto::Message greeting;
    CK_CHECK(good.greet(greeting));

    WireClient bad;
    CK_CHECK(bad.connect(socket));
    ckm::proto::Message ack;
    CK_CHECK(bad.greet(ack));
    // A header claiming a type that does not exist. Pre-1.0 there is one version
    // integer, so an unknown type cannot legitimately occur and is a connection
    // error rather than something to skip (the protocol spec).
    const std::string nonsense("\x00\x00\x00\x00\xEE\xEE\x00\x00", 8);
    (void)bad.stream.send(nonsense);
    (void)bad.stream.flush();
    ckm::proto::Message nothing;
    CK_CHECK(!bad.hear(nothing, 500));  // dropped

    // The other connection is untouched, which is the whole claim: a decode
    // error is fatal for one connection and for nothing else.
    good.say(ckm::proto::Ping{99});
    ckm::proto::Message pong;
    CK_CHECK(good.hear(pong));
    const auto* answered = std::get_if<ckm::proto::Pong>(&pong);
    CK_CHECK(answered != nullptr);
    if (answered != nullptr) CK_CHECK(answered->nonce == 99U);

    end_server(server);
    forget(socket);
}

// --- What the descriptors and the directory are, before any of that ----
//
// The cases above drive real servers over the wire. These drive the layer
// underneath them directly, because the properties they are about — a flag on
// a descriptor, a mode on a directory, which of several reasons an accept
// returned nothing for — are invisible from the wire and are exactly the ones
// that fail quietly for months.

CK_TEST(the_control_plane_is_never_inherited_by_a_program_a_terminal_starts) {
    // Every terminal window ckmux opens execs a shell, and a shell inherits
    // every descriptor that is not close-on-exec. Without this the shell holds
    // the client's connection to the server, the server's listening socket and
    // the accepted connections of every other client — all of them usable, and
    // none of them reachable by anything that could refuse them. There is no
    // handshake to fail: the connection is already past it.
    const std::filesystem::path socket = private_socket("cloexec");
    forget(socket);

    ckm::platform::Listener listener;
    CK_CHECK(listener.listen(socket) == ckm::platform::Listener::Status::Listening);
    CK_CHECK(is_close_on_exec(listener.fd()));

    ckm::platform::ConnectResult connected = ckm::platform::connect_to_server(socket);
    CK_CHECK(connected.status == ckm::platform::ConnectStatus::Connected);
    CK_CHECK(is_close_on_exec(connected.fd));
    CK_CHECK(is_non_blocking(connected.fd));

    // The accepted end, which is the one the server keeps for the life of the
    // connection and the one every shell in every window would have inherited.
    ckm::platform::Listener::AcceptResult accepted;
    for (int attempt = 0; attempt < 200; ++attempt) {
        accepted = listener.accept_one();
        if (accepted.status != ckm::platform::Listener::AcceptStatus::Idle) break;
        ::usleep(2000);
    }
    CK_CHECK(accepted.status == ckm::platform::Listener::AcceptStatus::Accepted);
    CK_CHECK(accepted.fd >= 0);
    CK_CHECK(is_close_on_exec(accepted.fd));
    CK_CHECK(is_non_blocking(accepted.fd));
    if (accepted.fd >= 0) (void)::close(accepted.fd);

    (void)::close(connected.fd);
    listener.close();
    forget(socket);
}

CK_TEST(nothing_pending_is_a_different_answer_from_out_of_descriptors) {
    // Both used to be -1. A loop that reads them the same way keeps asking a
    // listener that stays readable for as long as the connection it cannot
    // accept is pending, which is 100 % of one core until somebody notices
    // (M-F5, and the server-side half of it in R2). The classification is what
    // makes the two answers separable at all; this pins the ordinary one.
    const std::filesystem::path socket = private_socket("idle");
    forget(socket);

    ckm::platform::Listener listener;
    CK_CHECK(listener.listen(socket) == ckm::platform::Listener::Status::Listening);

    const ckm::platform::Listener::AcceptResult idle = listener.accept_one();
    CK_CHECK(idle.status == ckm::platform::Listener::AcceptStatus::Idle);
    CK_CHECK(idle.fd == -1);
    // Nothing a reader could do about it, so nothing said to them about it.
    CK_CHECK(idle.problem.empty());

    // And the shape the server loop is written against is unchanged: -1, and a
    // refusal string left alone when there is nothing to refuse.
    std::string refusal;
    CK_CHECK(listener.accept_one(refusal) == -1);
    CK_CHECK(refusal.empty());

    // A closed listener is a failure rather than an idle moment: a caller that
    // could not tell those apart would poll a descriptor that no longer exists.
    listener.close();
    const ckm::platform::Listener::AcceptResult closed = listener.accept_one();
    CK_CHECK(closed.status == ckm::platform::Listener::AcceptStatus::Failed);
    CK_CHECK(!closed.problem.empty());
    forget(socket);
}

CK_TEST(the_socket_directory_is_made_owner_only_rather_than_made_and_corrected) {
    // A `mkdir` under the process umask followed by a `chmod` is owner-only
    // only after the chmod. In between it is whatever the umask said — 0755 on
    // most machines — and the socket that is about to appear in it is one
    // anybody on the host may replace with their own. Umask 0 here is the
    // permissive machine, made deterministic.
    const std::filesystem::path fresh =
        private_socket("mode").parent_path() / "made-here" / "default.sock";
    std::error_code ignored;
    std::filesystem::remove_all(fresh.parent_path(), ignored);

    const ::mode_t previous = ::umask(0);
    std::string problem;
    const bool prepared = ckm::platform::prepare_socket_directory(fresh, problem);
    (void)::umask(previous);

    CK_CHECK(prepared);
    CK_CHECK(problem.empty());
    struct ::stat info{};
    CK_CHECK(::lstat(fresh.parent_path().c_str(), &info) == 0);
    CK_CHECK(S_ISDIR(info.st_mode));
    CK_CHECK((info.st_mode & 0777U) == 0700U);

    std::filesystem::remove_all(fresh.parent_path(), ignored);
    forget(private_socket("mode"));
}

CK_TEST(a_socket_directory_reached_through_a_symbolic_link_works) {
    // macOS's /tmp is itself a symbolic link to /private/tmp, and a reader may
    // reach their own socket directory through a link of their own making.
    // Refusing every link refused both: a server asked for a socket whose
    // parent was spelled as a link answered "exists and is not a directory",
    // which was neither true nor actionable. A link made by root or by this
    // user took no privilege the checks do not already trust, so it is
    // followed — and every check still lands on the target, which is the
    // directory the socket is actually bound in.
    const std::filesystem::path base = private_socket("follow").parent_path();
    const std::filesystem::path target = base / "real-directory";
    const std::filesystem::path link = base / "linked-directory";
    std::error_code ignored;
    std::filesystem::remove_all(target, ignored);
    std::filesystem::remove(link, ignored);
    std::filesystem::create_directories(target, ignored);
    std::filesystem::create_directory_symlink(target, link, ignored);
    CK_CHECK(!ignored);
    if (ignored) return;

    std::string problem;
    CK_CHECK(ckm::platform::prepare_socket_directory(link / "s.sock", problem));
    CK_CHECK(problem.empty());

    // The checks were applied to the target, not to the link: the target was
    // made owner-only, and the link is still a link rather than having been
    // replaced by anything.
    struct ::stat info{};
    CK_CHECK(::lstat(target.c_str(), &info) == 0);
    CK_CHECK(S_ISDIR(info.st_mode));
    CK_CHECK((info.st_mode & 0777U) == 0700U);
    CK_CHECK(::lstat(link.c_str(), &info) == 0);
    CK_CHECK(S_ISLNK(info.st_mode));

    // And not merely prepared: a server listens on the linked spelling of the
    // path, and a client reaches it through the same spelling.
    ckm::platform::Listener listener;
    CK_CHECK(listener.listen(link / "s.sock") == ckm::platform::Listener::Status::Listening);
    const ckm::platform::ConnectResult reached =
        ckm::platform::connect_to_server(link / "s.sock");
    CK_CHECK(reached.status == ckm::platform::ConnectStatus::Connected);
    if (reached.fd >= 0) (void)::close(reached.fd);
    listener.close();

    std::filesystem::remove(link, ignored);
    std::filesystem::remove_all(target, ignored);
    forget(private_socket("follow"));
}

CK_TEST(a_socket_directory_link_that_resolves_to_a_file_is_still_refused) {
    // Following a trusted link is not trusting what it points at: the target
    // takes every check a plain directory takes, so a link that resolves to
    // something that is not a directory at all is refused — and now for the
    // true reason, rather than for being a link.
    const std::filesystem::path base = private_socket("linkfile").parent_path();
    const std::filesystem::path target = base / "real-file";
    const std::filesystem::path link = base / "linked-file";
    std::error_code ignored;
    std::filesystem::remove(target, ignored);
    std::filesystem::remove(link, ignored);
    {
        std::FILE* const made = std::fopen(target.c_str(), "w");
        CK_CHECK(made != nullptr);
        if (made != nullptr) (void)std::fclose(made);
    }
    std::filesystem::create_symlink(target, link, ignored);
    CK_CHECK(!ignored);
    if (ignored) return;

    std::string problem;
    CK_CHECK(!ckm::platform::prepare_socket_directory(link / "s.sock", problem));
    CK_CHECK(problem.find("is not a directory") != std::string::npos);

    std::filesystem::remove(link, ignored);
    std::filesystem::remove(target, ignored);
    forget(private_socket("linkfile"));
}

CK_TEST(an_environment_variable_that_is_empty_or_relative_names_nowhere) {
    // Set-but-empty is how a shell says nothing, and a relative path is what
    // the XDG specification says to ignore. Believing either gives a socket in
    // a different place for every directory ckmux is started from, which a
    // reader meets as "all my sessions are gone".
    ScopedVariable probe("CKMUX_PLATFORM_PROBE");
    probe.set("");
    CK_CHECK(ckm::platform::environment_value("CKMUX_PLATFORM_PROBE") == nullptr);
    CK_CHECK(ckm::platform::environment_directory("CKMUX_PLATFORM_PROBE") == nullptr);
    probe.set("relative/place");
    // Still a value — an explicit override is used exactly as given — but not
    // a directory to build a path under.
    CK_CHECK(ckm::platform::environment_value("CKMUX_PLATFORM_PROBE") != nullptr);
    CK_CHECK(ckm::platform::environment_directory("CKMUX_PLATFORM_PROBE") == nullptr);
    probe.set("/absolute/place");
    const char* const absolute = ckm::platform::environment_directory("CKMUX_PLATFORM_PROBE");
    CK_CHECK(absolute != nullptr);
    if (absolute != nullptr) CK_CHECK(std::string(absolute) == "/absolute/place");
    probe.clear();
    CK_CHECK(ckm::platform::environment_value("CKMUX_PLATFORM_PROBE") == nullptr);

    // And the rule reaches the socket path, which is what it is for.
    ScopedVariable explicit_socket("CKMUX_SOCKET");
    ScopedVariable runtime("XDG_RUNTIME_DIR");
    ScopedVariable temporary("TMPDIR");
    explicit_socket.clear();
    runtime.set("relative/runtime");
    temporary.set("/tmp");
    const std::filesystem::path chosen = ckm::platform::socket_path();
    CK_CHECK(chosen.is_absolute());
    CK_CHECK(chosen == std::filesystem::path("/tmp") /
                           ("ckmux-" + std::to_string(static_cast<unsigned long>(::geteuid()))) /
                           "default.sock");
}

CK_TEST(a_wait_that_found_nothing_says_why_it_found_nothing) {
    // Three different things return an empty set: a timeout, a signal, and a
    // descriptor poll() refuses. Only the first means "sleep and come back",
    // and a loop that treats the third that way spins at full speed for the
    // life of the process, because a refused descriptor is refused instantly
    // and stays refused.
    ckm::platform::Poller poller;
    CK_CHECK(poller.wait(0).empty());
    CK_CHECK(poller.outcome() == ckm::platform::Poller::Outcome::TimedOut);

    int ends[2] = {-1, -1};
    CK_CHECK(::pipe(ends) == 0);
    poller.clear();
    poller.watch(ends[0], ckm::platform::Interest::Read);
    CK_CHECK(poller.watched() == 1U);
    CK_CHECK(poller.wait(0).empty());
    CK_CHECK(poller.outcome() == ckm::platform::Poller::Outcome::TimedOut);

    CK_CHECK(::write(ends[1], "x", 1) == 1);
    const std::vector<ckm::platform::Ready>& ready = poller.wait(1000);
    CK_CHECK(ready.size() == 1U);
    if (!ready.empty()) {
        CK_CHECK(ready.front().fd == ends[0]);
        CK_CHECK(ready.front().readable);
    }
    CK_CHECK(poller.outcome() == ckm::platform::Poller::Outcome::Ready);

    // A descriptor that has been closed under the poller is reported, not
    // silently skipped: it comes back as a hangup, which is a state the caller
    // can act on, rather than as an empty set it would mistake for a timeout.
    (void)::close(ends[0]);
    (void)::close(ends[1]);
    const std::vector<ckm::platform::Ready>& refused = poller.wait(0);
    CK_CHECK(refused.size() == 1U);
    if (!refused.empty()) CK_CHECK(refused.front().hangup);
    CK_CHECK(poller.outcome() == ckm::platform::Poller::Outcome::Ready);
}

CK_TEST(a_send_whose_peer_has_gone_reports_that_rather_than_answering_fine) {
    // `send` queues and writes, and the write can fail. Discarding that failure
    // answered "fine" about a connection that had died: a server told that goes
    // on queueing a flooding session's entire output — megabyte after megabyte
    // — into a socket that will never take another byte, and finds out only
    // when it next tries to read.
    const std::filesystem::path socket = private_socket("gone");
    forget(socket);

    ckm::platform::Listener listener;
    CK_CHECK(listener.listen(socket) == ckm::platform::Listener::Status::Listening);
    ckm::platform::ConnectResult connected = ckm::platform::connect_to_server(socket);
    CK_CHECK(connected.status == ckm::platform::ConnectStatus::Connected);
    ckm::platform::Stream stream(connected.fd);

    ckm::platform::Listener::AcceptResult accepted;
    for (int attempt = 0; attempt < 200; ++attempt) {
        accepted = listener.accept_one();
        if (accepted.status != ckm::platform::Listener::AcceptStatus::Idle) break;
        ::usleep(2000);
    }
    CK_CHECK(accepted.status == ckm::platform::Listener::AcceptStatus::Accepted);
    // The peer goes, which is what a killed server or a crashed client looks
    // like from here. This must also not raise SIGPIPE — on macOS that is a
    // socket option every Stream sets for itself — because a process that died
    // on this line would be the client dying mid-frame with nothing on the
    // screen to say why.
    if (accepted.fd >= 0) (void)::close(accepted.fd);

    CK_CHECK(!stream.send("anything"));
    // The bytes are still queued: dropping half a frame would desynchronise a
    // stream that might yet be flushed, and "stop adding" is what false says.
    CK_CHECK(stream.queued() == 8U);

    stream.close();
    listener.close();
    forget(socket);
}

CK_TEST(a_path_too_long_to_be_a_socket_address_is_refused_before_anything_is_started) {
    // `sun_path` is 104 bytes on macOS and 108 on Linux, and the failure mode
    // when a path does not fit is a SILENT truncation: a server that listens
    // somewhere else entirely, and a client that waits out its whole budget for
    // a socket that will never appear at the path it asked for.
    std::filesystem::path absurd = "/tmp";
    while (absurd.string().size() < 200) absurd /= "a-directory-with-a-long-name";
    CK_CHECK(!ckm::platform::socket_path_fits(absurd));
    CK_CHECK(ckm::platform::socket_path_fits(private_socket("short")));

    const ckm::client::ServerConnection refused =
        ckm::client::connect_to_server(absurd, binary_path(), ckm::proto::ClientKind::Cli, 200);
    CK_CHECK(refused.status == ckm::client::ServerConnection::Status::Refused);
    CK_CHECK(refused.problem.find("too long") != std::string::npos);
}
