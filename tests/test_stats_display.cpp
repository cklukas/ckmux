// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The View readouts on screen (WP-39): toggling one puts the line on EVERY
// terminal window's frame footer at once and writes the config file; a client
// started with the key already true comes up showing it; all-off restores
// today's frame. Driven through `execute_command` — the registry the menu
// items themselves dispatch through — against a real ClientApp whose children
// are real processes, because the numbers on the frame are real too.
#include <stdlib.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "client/client_app.hpp"
#include "client/commands.hpp"

#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/widgets/menu.hpp"

namespace {

using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;
using ckm::client::ClientApp;
using ckm::client::ClientOptions;

// A private config directory, told to the client through CKMUX_CONFIG so the
// toggles' write-back lands here and never in the machine's real file. The
// env var is process state: set in the constructor, cleared in the destructor,
// so a case that fails cannot leak it into its neighbours.
class ScratchConfigEnv {
public:
    explicit ScratchConfigEnv(const std::string& name)
        : directory_(std::filesystem::temp_directory_path() /
                     ("ckmux-stats-" + name + "-" +
                      std::to_string(static_cast<long long>(::getpid())))) {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
        std::filesystem::create_directories(directory_, ignored);
        ::setenv("CKMUX_CONFIG", path().c_str(), 1);
    }
    ~ScratchConfigEnv() {
        ::unsetenv("CKMUX_CONFIG");
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    ScratchConfigEnv(const ScratchConfigEnv&) = delete;
    ScratchConfigEnv& operator=(const ScratchConfigEnv&) = delete;

    std::filesystem::path path() const { return directory_ / "ckmux.conf"; }
    std::string contents() const {
        std::ifstream in(path());
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

private:
    std::filesystem::path directory_;
};

ClientOptions test_options() {
    ClientOptions options;
    // A program that simply stays alive, so a window has a live child without
    // the run depending on whose shell is installed.
    options.settings.shell = "/bin/cat";
    return options;
}

ckv::ui::CommandId id_of(Application& app, std::string_view key) {
    return app.commands().id_for(key).value_or(ckv::ui::kInvalidCommand);
}

}  // namespace

CK_TEST(toggling_cpu_marks_every_window_at_once_and_writes_the_config) {
    ScratchConfigEnv scratch("toggle");
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientApp client{app, test_options()};
    app.step(0);
    CK_CHECK(app.execute_command(id_of(app, ckm::client::commands::kNewTerminal)));
    app.step(0);
    CK_CHECK(client.desktop().windows().size() == 2U);
    for (const auto* window : client.desktop().windows()) CK_CHECK(window->footer().empty());

    // On: every window carries the readout at once — a live child's CPU may
    // honestly be 0%, so the claim is the line's presence and shape, not its
    // number.
    CK_CHECK(app.execute_command(id_of(app, ckm::client::commands::kShowCpuUsage)));
    for (const auto* window : client.desktop().windows())
        CK_CHECK(window->footer().rfind("CPU ", 0) == 0);
    // Written back the moment it changed, like the Settings dialog's boxes.
    CK_CHECK(scratch.contents().find("show-cpu = true") != std::string::npos);
    // And the menu's checkmark agrees: the View menu is index 2, its first
    // item is Show CPU Usage, and its mark is a value the toggle rebuilt.
    {
        ckv::widgets::MenuBar* const bar =
            dynamic_cast<ckv::widgets::MenuBar*>(client.desktop().top_dock());
        CK_CHECK(bar != nullptr);
        CK_CHECK(bar->menus()[2].items[0].mark() == ckv::widgets::MenuMark::Checked);
    }

    // A second readout joins the same line rather than replacing it.
    CK_CHECK(app.execute_command(id_of(app, ckm::client::commands::kShowMemoryRss)));
    for (const auto* window : client.desktop().windows()) {
        CK_CHECK(window->footer().find("CPU ") != std::string::npos);
        CK_CHECK(window->footer().find("RSS ") != std::string::npos);
    }

    // All off restores today's frame byte for byte: an empty footer draws
    // nothing.
    CK_CHECK(app.execute_command(id_of(app, ckm::client::commands::kShowCpuUsage)));
    CK_CHECK(app.execute_command(id_of(app, ckm::client::commands::kShowMemoryRss)));
    for (const auto* window : client.desktop().windows()) CK_CHECK(window->footer().empty());
    CK_CHECK(scratch.contents().find("show-cpu = false") != std::string::npos);
    CK_CHECK(scratch.contents().find("show-memory-rss = false") != std::string::npos);
}

CK_TEST(a_client_started_with_a_readout_on_shows_it_from_the_first_frame) {
    // The persistence claim's other half: the checkbox survived because the
    // reader flipped it, so a restarted client comes up already showing the
    // readout — no toggle, no wait, the constructor applied what the file
    // said.
    ScratchConfigEnv scratch("startup");
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    ClientOptions options = test_options();
    options.settings.show_memory_rss = true;
    ClientApp client{app, std::move(options)};
    app.step(0);
    CK_CHECK(client.desktop().windows().size() == 1U);
    const std::string footer{client.desktop().windows()[0]->footer()};
    CK_CHECK(footer.rfind("RSS ", 0) == 0);
    // A real unit on a real number: the child was measured, not invented.
    CK_CHECK(footer.find(" KB") != std::string::npos || footer.find(" MB") != std::string::npos);
}
