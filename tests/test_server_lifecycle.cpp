// Copyright (c) 2026 C. Klukas. All rights reserved.
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
    bool greet(ckm::proto::Message& answer, std::uint32_t version = ckm::proto::kProtocolVersion) {
        ckm::proto::Hello hello;
        hello.proto_version = version;
        hello.build = "a test";
        say(hello);
        return hear(answer);
    }
};

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

    // The socket is owner-only, in an owner-only directory (plans/02's security
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

CK_TEST(a_message_the_server_does_not_implement_yet_is_answered_rather_than_ignored) {
    // A client waiting for a reply that never comes looks exactly like a server
    // that has hung, and a reader cannot tell those apart. Sessions arrive with
    // WP-8; until they do, asking for one gets an answer that says so.
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
    client.say(request);
    ckm::proto::Message reply;
    CK_CHECK(client.hear(reply));
    const auto* error = std::get_if<ckm::proto::Error>(&reply);
    CK_CHECK(error != nullptr);
    if (error != nullptr) {
        CK_CHECK(error->code == static_cast<std::uint16_t>(ckm::proto::ErrorCode::NotImplemented));
        CK_CHECK(error->context == "NewSession");
        CK_CHECK(!error->human.empty());
    }
    // And the connection survives it: an unimplemented request is not a
    // protocol error.
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
    // error rather than something to skip (plans/05).
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
