// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The ckmux entry point, and the one place that decides which of the program's
// three roles this invocation is (the architecture spec):
//
//   ckmux                     the client
//   ckmux --server <socket>   the detached server; users never type this
//   ckmux kill-server         a CLI utility, over the same socket protocol
//
// `--help` and `--version` are answered here too, before any role begins:
// they touch no socket and start nothing.
//
// One binary keeps the client and the server the same build — they still shake
// hands, because after an upgrade an old server meets a new binary, and the
// handshake is what makes that say something a reader can act on rather than
// producing nonsense.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "client/cli.hpp"
#include "client/key_reference.hpp"
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

bool is_any_of(const char* argument, std::initializer_list<const char*> spellings) {
    for (const char* spelling : spellings)
        if (std::strcmp(argument, spelling) == 0) return true;
    return false;
}

// `ckmux --help`: the whole CLI on one page. The defaults the page states
// restate platform/socket.cpp and platform/paths.hpp; a change there edits
// this page in the same commit, or the help lies (the conventions).
// Typeset by hand for an 80-column terminal, and the CLI tests measure that.
void print_cli_help() {
    const std::string_view identity = ckm::proto::kBuildIdentity;
    std::printf(
        "%.*s — a terminal multiplexer, like tmux, with a face.\n"
        "\n"
        "Usage:\n"
        "  ckmux              Attach: your terminal windows, starting the server if\n"
        "                     none is listening. Quitting the client leaves the server\n"
        "                     and every program in it running; run ckmux again to\n"
        "                     return to them.\n"
        "  ckmux ls           The sessions a server holds, one per line. Says so\n"
        "                     rather than starting one when none is running.\n"
        "  ckmux new [-s name] [command]\n"
        "                     A session, made without attaching to it. Prints its\n"
        "                     name; the server names it when you do not. A command\n"
        "                     runs in its first terminal, through your shell.\n"
        "  ckmux attach [--share] [--watch] [--adopt-size] <name|id>\n"
        "                     Straight to one session, past the picker. Takes the\n"
        "                     name `ckmux ls` prints, or an id.\n"
        "                     --share       Join a session someone may already be\n"
        "                                   watching, rather than taking it over.\n"
        "                     --watch       Join it and only look: nothing you type\n"
        "                                   reaches it, and nothing you do changes\n"
        "                                   it. Implies --share.\n"
        "                     --adopt-size  Make the session's desktop this screen,\n"
        "                                   once, on arrival. Reflows every window\n"
        "                                   and resizes every child, for everyone.\n"
        "  ckmux kill-session <name|id>\n"
        "                     Ask every program in one session to end, then drop it.\n"
        "  ckmux check-config Parse the configuration and report, starting nothing.\n"
        "                     Exits non-zero when the file has problems.\n"
        "  ckmux kill-server  End the server and every terminal in it.\n"
        "  ckmux --help       This page (also -h, help).\n"
        "  ckmux --version    The build identity, which client and server compare\n"
        "                     when they meet (also -V).\n"
        "\n"
        "Internal:\n"
        "  ckmux --server <socket> [--foreground]\n"
        "                     The detached server. A client starts one by itself when\n"
        "                     none answers, so the only reason to type this is\n"
        "                     debugging: --foreground keeps it in front of you with\n"
        "                     its diagnostics on stderr; a detached server writes\n"
        "                     them to a log beside the socket.\n"
        "\n"
        "Environment:\n"
        "  CKMUX_SOCKET       The server socket, used exactly as given. Default:\n"
        "                     $XDG_RUNTIME_DIR/ckmux-<uid>/default.sock, with $TMPDIR\n"
        "                     and then /tmp standing in when there is no runtime\n"
        "                     directory.\n"
        "  CKMUX_CONFIG       The configuration file, used exactly as given. Default:\n"
        "                     $XDG_CONFIG_HOME/ckmux/ckmux.conf, ordinarily\n"
        "                     ~/.config/ckmux/ckmux.conf. A missing file is the\n"
        "                     ordinary case: every setting has a default.\n"
        "\n"
        "Inside the client, every command is in the menu bar and F1 opens the help\n"
        "pages; nothing has to be memorized before it can be found.\n",
        static_cast<int>(identity.size()), identity.data());
}

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
        // one they are talking to. Printed, not fatal: the server kept this
        // connection open for exactly one more message on the strength of that
        // sentence, and `KillServer` is that message.
        std::fprintf(stderr, "ckmux: %s\n", refuse->reason.c_str());
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
    if (argc == 2 && std::strcmp(argv[1], "--internal-key-appendix=markdown") == 0) {
        const std::string appendix = ckm::client::render_default_key_appendix(
            ckm::client::KeyAppendixFormat::Markdown);
        return std::fwrite(appendix.data(), 1, appendix.size(), stdout) == appendix.size() ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--internal-key-appendix=roff") == 0) {
        const std::string appendix =
            ckm::client::render_default_key_appendix(ckm::client::KeyAppendixFormat::Roff);
        return std::fwrite(appendix.data(), 1, appendix.size(), stdout) == appendix.size() ? 0 : 1;
    }
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
    if (argc >= 2 && is_any_of(argv[1], {"--help", "-h", "help"})) {
        print_cli_help();
        return 0;
    }
    if (argc >= 2 && is_any_of(argv[1], {"--version", "-V"})) {
        // The same identity the handshake sends, so the number in a bug
        // report is the number the wire compares.
        const std::string_view identity = ckm::proto::kBuildIdentity;
        std::printf("%.*s\n", static_cast<int>(identity.size()), identity.data());
        return 0;
    }
    // `ckmux attach <name>`'s session, resolved below before anything is
    // drawn. Zero is every other invocation, where the picker decides.
    std::uint64_t attach_to = 0;
    // `attach`'s flags, kept out here for the same reason as the id: the block
    // that parses them ends before the client is built.
    ckm::proto::AttachMode attach_mode = ckm::proto::AttachMode::TakeOver;
    bool attach_adopts_size = false;
    // The WP-11 subcommands. Parsed before anything connects, so a mistyped
    // flag is answered at the shell rather than after a socket round trip.
    if (argc >= 2 && ckm::client::is_cli_subcommand(argv[1])) {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
        const ckm::client::CliRequest request = ckm::client::parse_cli(arguments);
        if (!request.ok()) {
            std::fprintf(stderr, "ckmux: %s\nUsage: %s\n", request.problem.c_str(),
                         request.usage.c_str());
            return 2;
        }
        if (request.subcommand == "ls") return ckm::client::run_ls(socket);
        if (request.subcommand == "check-config")
            return ckm::client::run_check_config(ckm::platform::config_file_path());
        if (request.subcommand == "kill-session")
            return ckm::client::run_kill_session(socket, request);
        if (request.subcommand == "new")
            return ckm::client::run_new(socket, ckm::platform::executable_path(argv[0]), request);
        // `attach` falls through to the client below with its session already
        // chosen — it IS the ordinary client, which is the whole point. The
        // lookup happens HERE, before a clock or a terminal is constructed:
        // resolving it further down would put "no such session" behind
        // `PosixTerminal`'s demand for a real tty, so `ckmux attach typo` in a
        // script would abort on the terminal instead of answering the typo.
        if (request.subcommand == "attach") {
            attach_to = ckm::client::resolve_attach_target(socket, request);
            if (attach_to == 0) return 1;
            attach_mode = request.watch    ? ckm::proto::AttachMode::Watch
                          : request.share ? ckm::proto::AttachMode::Join
                                          : ckm::proto::AttachMode::TakeOver;
            attach_adopts_size = request.adopt_size;
        }
    }
    if (argc >= 2 && argv[1][0] != '\0' && !ckm::client::is_cli_subcommand(argv[1])) {
        // A word this build does not answer. Saying so beats a client starting
        // up and ignoring what was typed.
        std::fprintf(stderr,
                     "ckmux: `%s` is not a subcommand this build has yet.\n"
                     "`ckmux --help` lists what is.\n",
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
    // the ordinary case and changes nothing (the configuration spec). The
    // whole of it is handed over rather than a field at a time, so a key the
    // file gained is a key the client already has.
    ckm::LoadedSettings stored = ckm::load_settings(ckm::platform::config_file_path());
    options.settings = std::move(stored.settings);
    options.config_warnings = std::move(stored.warnings);

    // Copying to a helper program forks, which the client deliberately does
    // not do itself — the host supplies the one function that does.
    //
    // The helper's own output is captured and kept beside the call rather than
    // inherited: ckmux is drawing a screen on this process's stdout and stderr,
    // and a helper that prints "command not found" would print it into the
    // frame. The client asks for it when a copy fails, which is the only time
    // it means anything to a reader.
    auto clipboard_problem = std::make_shared<std::string>();
    options.clipboard_writer = [clipboard_problem](const std::string& command,
                                                   std::string_view text) {
        clipboard_problem->clear();
        return ckm::platform::write_to_command(command, text, clipboard_problem.get());
    };
    options.clipboard_problem = [clipboard_problem] { return *clipboard_problem; };

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

    // The Terminal Report's decoded-SGR-reports line: only the host holds the
    // terminal whose decoder counts them, so the host hands the count over
    // the same way it hands the clock. The terminal outlives the client run
    // below, so the reference is sound for as long as anything can call this.
    options.mouse_reports_probe = [&terminal] { return terminal.mouse_reports_seen(); };

    // Attached, which is what ckmux is for: the terminals live in a server that
    // outlives this process, and everything below the seam is the same interface
    // M1 had (WP-5, WP-6). The server is started if none is listening, and the
    // one thing a reader has to know about it is that quitting this window does
    // not end their programs.
    ckm::client::RunOptions run;
    run.socket = socket;
    run.executable = ckm::platform::executable_path(argv[0]);
    run.client = std::move(options);
    run.preselected_session = attach_to;
    run.attach_mode = attach_mode;
    run.adopt_session_size = attach_adopts_size;
    return ckm::client::run_attached_client(terminal, clock, std::move(run));
}
