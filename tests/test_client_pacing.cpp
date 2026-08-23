// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// What ckmux asks the reader's own terminal, and why it has to ask.
//
// Writing a frame proves its bytes left this process. It says nothing about
// whether the terminal on the other end has drawn them, and a host that
// accepts Sixel faster than it draws it queues the difference — so a client
// that keeps composing regardless is producing frames nobody will ever see,
// and what the reader watches is a picture running further behind every one
// of them. A multiplexer is exactly the application that cannot assume a
// modest frame rate: what it relays is whatever the child draws, and a child
// may draw a full-screen picture as fast as its terminal will take one.
//
// ckVision closes that loop when an application asks it to
// (`Application::set_frame_completion_tracking`), and the asking is ckmux's
// decision to make, so it is ckmux's to pin. The evidence is the bytes the
// reader's terminal is actually given: ckVision's own raw-output capture
// (CKVISION_OUTPUT_CAPTURE) records them from inside the real client process,
// which is the only place they exist.
#include <tuple>  // std::ignore
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cvision/term/posix_terminal_subsession.hpp"

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

std::filesystem::path private_directory(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-p" + std::to_string(static_cast<unsigned long>(::getpid()));
    directory /= name;
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

std::string file_contents(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

CK_TEST(the_client_asks_the_readers_terminal_whether_it_took_the_frame) {
    // The real client, in a real pseudo-terminal, writing to a real host — and
    // every byte of that written down beside it. Without the pacing decision
    // the capture is a stream of frames and nothing else: ckmux would hand the
    // host picture after picture and never once ask whether it had kept up,
    // which is how a reader ends up watching a child's animation play out at
    // the position its window left seconds ago, still going after the child
    // that drew it has exited (field report, 2026-08-19, ckvision_spin).
    if (binary_path().empty()) return;  // built without the binary: nothing to drive
    const std::filesystem::path directory = private_directory("pacing");
    const std::filesystem::path socket = directory / "default.sock";
    const std::filesystem::path capture = directory / "written-to-the-host";
    std::error_code ignored;
    std::filesystem::remove(capture, ignored);

    ckv::term::TerminalLaunchSpec spec =
        ckv::term::TerminalLaunchSpec::program(binary_path().string(), {});
    spec.working_directory = "/tmp";
    spec.environment = {{"TERM", "xterm-256color"},
                        {"PATH", "/usr/bin:/bin"},
                        {"SHELL", "/bin/sh"},
                        {"HOME", "/tmp"},
                        {"LC_ALL", "C"},
                        {"CKMUX_SOCKET", socket.string()},
                        {"CKVISION_OUTPUT_CAPTURE", capture.string()}};
    spec.profile = ckv::term::embedded_xterm_sixel_profile();
    spec.profile.cells = ckv::Size{110, 32};
    spec.profile.cell_pixels = ckv::Size{9, 18};
    spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
    ckv::term::TerminalSubsessionOptions options;
    options.max_output_bytes = 1u << 20u;
    options.max_parser_work_per_step = 256u << 10u;
    std::unique_ptr<ckv::term::PosixTerminalSubsession> client =
        ckv::term::PosixTerminalSubsession::launch(spec, options);
    CK_CHECK(client != nullptr);
    if (client == nullptr) return;

    // Drawn at all, first: a capture with no frames in it would pass the
    // absence of anything and prove nothing. The menu bar is on the very first
    // frame a reader ever sees.
    bool drew = false;
    const clock_type::time_point deadline = clock_type::now() + std::chrono::seconds(6);
    while (clock_type::now() < deadline) {
        (void)client->drain(64 * 1024);
        std::string text;
        for (const ckv::Cell& cell : client->snapshot().cell_buffer)
            if (!cell.is_continuation()) text += cell.grapheme();
        if (text.find("Session") != std::string::npos && text.find("Terminal") != std::string::npos) {
            drew = true;
            break;
        }
        ::usleep(20000);
    }
    CK_CHECK(drew);

    // And having drawn, it asked. `CSI 5 n` is the Device Status Report
    // ckVision appends to a presented frame when — and only when — an
    // application has asked it to pace against the terminal; a terminal reads
    // its input in order, so the reply says that frame has been taken in.
    const std::string written = file_contents(capture);
    CK_CHECK(!written.empty());
    CK_CHECK(written.find("\x1b[5n") != std::string::npos);

    // The positive partner to that: the capture really is this client's whole
    // output and not a fragment that happened to contain the marker, so a
    // frame is in it too.
    CK_CHECK(written.find("\x1b[") != std::string::npos);

    client->close();
    // The server this client started outlives it on purpose (that is what a
    // multiplexer is), so it is ended by hand rather than left behind.
    const std::string kill_server = binary_path().string() + " kill-server >/dev/null 2>&1";
    std::string command = "CKMUX_SOCKET='" + socket.string() + "' " + kill_server;
    std::ignore = std::system(command.c_str());
    std::filesystem::remove_all(directory, ignored);
}
