// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The ckmux entry point, and the one place that decides which of the program's
// three roles this invocation is (plans/02-architecture.md):
//
//   ckmux                     the client
//   ckmux --server <socket>   the detached server; users never type this
//   ckmux kill-server         a CLI utility, over the same socket protocol
//
// One binary keeps the client and the server the same build — they still shake
// hands, because after an upgrade an old server meets a new binary, and the
// handshake is what makes that say something a reader can act on rather than
// producing nonsense.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "client/client_app.hpp"
#include "client/run_client.hpp"
#include "client/server_connection.hpp"
#include "common/config.hpp"
#include "platform/clipboard.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"
#include "cvision/term/posix_clock.hpp"
#include "cvision/term/posix_terminal.hpp"
#include "cvision/term/terminal_clipboard.hpp"
#include "cvision/ui/application.hpp"

namespace {

// `ckmux kill-server`: ends the server and every terminal in it.
//
// Deliberately does NOT start a server in order to kill it, which is what
// sharing the client's connect path would have done — a reader who runs this
// twice would find themselves with a new server the second time.
int kill_server(const std::filesystem::path& socket) {
    const ckm::platform::ConnectResult connected = ckm::platform::connect_to_server(socket);
    if (connected.status != ckm::platform::ConnectStatus::Connected) {
        if (connected.status == ckm::platform::ConnectStatus::NoServer) {
            std::fprintf(stderr, "ckmux: no server is running at %s\n", socket.string().c_str());
            return 1;
        }
        std::fprintf(stderr, "ckmux: %s\n", connected.problem.c_str());
        return 1;
    }
    ckm::platform::Stream stream(connected.fd);
    ckm::proto::Hello hello;
    hello.build = std::string(ckm::proto::kBuildIdentity);
    hello.client_kind = ckm::proto::ClientKind::Cli;
    (void)stream.send(ckm::proto::encode(hello));

    ckm::proto::FrameReader reader;
    ckm::proto::Message answer;
    if (!ckm::client::await_message(stream, reader, answer)) {
        std::fprintf(stderr, "ckmux: the server did not answer the handshake\n");
        return 1;
    }
    if (const auto* refuse = std::get_if<ckm::proto::Refuse>(&answer)) {
        // A server of another version can still be killed — that is usually
        // exactly why somebody is running this — but it is worth saying which
        // one they are talking to.
        std::fprintf(stderr, "ckmux: %s\n", refuse->reason.c_str());
        return 1;
    }
    (void)stream.send(ckm::proto::encode(ckm::proto::KillServer{}));
    (void)stream.flush();
    // The server closes the connection as it goes down; waiting for that is how
    // this reports "done" rather than "asked".
    ckm::proto::Message ignored;
    (void)ckm::client::await_message(stream, reader, ignored, 2000);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path socket = ckm::platform::socket_path();
    if (argc >= 2 && std::strcmp(argv[1], "--server") == 0) {
        // The socket is passed rather than resolved again, so a server always
        // listens on the path the client that started it was looking at — one
        // environment read, not two.
        const std::filesystem::path listen_on =
            argc >= 3 ? std::filesystem::path(argv[2]) : socket;
        // `--foreground` is for tests and for a reader debugging a server: a
        // process that double-forks cannot be waited for, and a test that
        // cannot wait for its subject has to sleep and hope.
        bool foreground = false;
        for (int index = 3; index < argc; ++index)
            if (std::strcmp(argv[index], "--foreground") == 0) foreground = true;
        return ckm::server::run_server_process(listen_on, foreground);
    }
    if (argc >= 2 && std::strcmp(argv[1], "kill-server") == 0) return kill_server(socket);
    if (argc >= 2 && argv[1][0] != '\0') {
        // Every other subcommand arrives with WP-11. Saying so beats a client
        // starting up and ignoring what was typed.
        std::fprintf(stderr,
                     "ckmux: `%s` is not a subcommand this build has yet.\n"
                     "Available: ckmux (the client), ckmux kill-server.\n",
                     argv[1]);
        return 2;
    }

    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);

    ckm::client::ClientOptions options;
    // Reading the environment is an application's job, never the library's:
    // ckVision requires a launch spec to state its program and directory
    // outright, and only a host knows which shell its user runs and where
    // they expect it to open. `shell` is deliberately left empty — ClientApp
    // resolves it the way tmux does (client/shell.hpp).
    if (const char* const home = std::getenv("HOME"); home != nullptr && *home != '\0')
        options.working_directory = home;

    // The stored settings, on top of the built-in defaults. A missing file is
    // the ordinary case and changes nothing (plans/08-configuration.md). The
    // whole of it is handed over rather than a field at a time, so a key the
    // file gained is a key the client already has.
    ckm::LoadedSettings stored = ckm::load_settings(ckm::platform::config_file_path());
    options.settings = std::move(stored.settings);
    options.config_warnings = std::move(stored.warnings);

    // Copying to a helper program forks, which the client deliberately does
    // not do itself — the host supplies the one function that does.
    options.clipboard_writer = [](const std::string& command, std::string_view text) {
        return ckm::platform::write_to_command(command, text);
    };

    // The menu-bar clock's time, and the calendar's today. This is the only
    // wall-clock reading ckmux takes, and it is taken here for the same reason
    // the environment is: the host knows which machine and which time zone
    // this is, ckVision's Clock is monotonic by design, and a test hands the
    // client a fixed moment instead of this function.
    options.local_now = [] {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        ::localtime_r(&now, &local);
        return ckm::client::LocalMoment{
            .date = {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday},
            .time = {local.tm_hour, local.tm_min, local.tm_sec}};
    };

    // Attached, which is what ckmux is for: the terminals live in a server that
    // outlives this process, and everything below the seam is the same interface
    // M1 had (WP-5, WP-6). The server is started if none is listening, and the
    // one thing a reader has to know about it is that quitting this window does
    // not end their programs.
    ckm::client::RunOptions run;
    run.socket = socket;
    run.executable = ckm::platform::executable_path(argv[0]);
    run.client = std::move(options);
    return ckm::client::run_attached_client(terminal, clock, std::move(run));
}
