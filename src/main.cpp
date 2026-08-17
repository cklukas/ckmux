// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The ckmux terminal host. M1 runs everything in this one process; M2 moves
// the terminals behind a detached server and leaves this file a client.
#include <cstdlib>
#include <string>

#include "client/client_app.hpp"
#include "common/config.hpp"
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

    ckm::client::ClientApp client(app, std::move(options));
    app.run();
    return 0;
}
