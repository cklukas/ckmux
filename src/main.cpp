// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The ckmux terminal host. M1 runs everything in this one process; M2 moves
// the terminals behind a detached server and leaves this file a client.
#include <cstdlib>
#include <ctime>
#include <string>

#include "client/client_app.hpp"
#include "common/config.hpp"
#include "platform/clipboard.hpp"
#include "platform/paths.hpp"
#include "cvision/term/posix_clock.hpp"
#include "cvision/term/posix_terminal.hpp"
#include "cvision/term/terminal_clipboard.hpp"
#include "cvision/ui/application.hpp"

int main() {
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::term::TerminalClipboardWriter clipboard(terminal);
    ckv::ui::Application app(terminal, clock, clipboard);

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

    ckm::client::ClientApp client(app, std::move(options));
    app.run();
    return 0;
}
