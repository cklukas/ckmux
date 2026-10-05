// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Actual ckmux client/server executables and the real system cmd.exe under
// ckVision's native ConPTY adapter. No echoed helper stands in for a shell.
#include "reader_harness.hpp"

namespace {
using ckmtest::Reader;
struct Fixture {
    std::filesystem::path endpoint;
    ckv::core::ProcessId server = -1;
    Reader reader;
    std::string_view startup_stage = "server launch";
    explicit Fixture(std::string_view name) : endpoint(ckmtest::private_socket(name)) {}
    ~Fixture() { reader.quit(); ckmtest::end_process(server); ckmtest::forget(endpoint); }
    bool start() {
        server = ckmtest::start_server(endpoint);
        if (server <= 0) return false;
        startup_stage = "endpoint readiness";
        if (!ckmtest::wait_for_socket(endpoint)) return false;
        startup_stage = "client launch";
        if (!reader.start(endpoint)) return false;
        startup_stage = "client desktop";
        if (!reader.sees("Session")) return false;
        // A rendered desktop is not shell readiness. cmd may still be
        // initializing/clearing its console input before it prints the prompt.
        // Observe actual shell output before sending the arithmetic witness.
        startup_stage = "native shell prompt";
        if (!reader.sees("Microsoft Windows") || !reader.sees(">")) return false;
        // The answer cannot appear in the command's input echo. This proves a
        // live cmd process, client input encoding, IPC, output and rendering.
        reader.press("set /a 731*19\r");
        startup_stage = "computed shell response";
        return reader.sees("13889");
    }
};
bool ready(Fixture& fixture) {
    const bool result = fixture.start();
    CK_CHECK(result);
    if (!result) {
        std::fprintf(stderr, "Native reader startup failed at %.*s\n",
            static_cast<int>(fixture.startup_stage.size()), fixture.startup_stage.data());
        if (fixture.reader.client) std::fprintf(stderr, "%s\n", fixture.reader.screen().c_str());
    }
    return result;
}
bool open_menu(Reader& reader, std::string_view item) {
    reader.press("\x02" "m");
    reader.press("\x1b[B");
    for (int steps = 0; steps < 8; ++steps) {
        if (reader.screen().find(item) != std::string::npos) return true;
        reader.press("\x1b[C");
    }
    return false;
}
bool frame_has_caption(Reader& reader, std::string_view title) {
    // A child command's input echo can contain the title too. Require it on
    // an actual window-frame row beside the close control, not anywhere in
    // the decoded screen or the child terminal's body.
    for (const auto& row : reader.rows())
        if (row.find("[■]") != std::string::npos && row.find(title) != std::string::npos)
            return true;
    return false;
}
} // namespace

CK_TEST(native_reader_routes_prefix_commands_and_real_shell_output) {
    Fixture fixture("commands");
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    reader.press("\x02" "c");
    reader.press("set /a 823*17\r");
    CK_CHECK(reader.sees("13991"));
    reader.press("\x02" "s");
    CK_CHECK(reader.sees("Sessions"));
    CK_CHECK(reader.screen().find("2 terminals") != std::string::npos);
    CK_CHECK(reader.screen().find("(this client)") != std::string::npos);
    reader.press("\x1b");
    reader.press("\x02" "w");
    CK_CHECK(reader.sees("Window List"));
    reader.press("\x1b");
    reader.press("\x02" "[");
    CK_CHECK(reader.sees("COPY"));
    reader.press("q");
    CK_CHECK(reader.stops_seeing("COPY"));
    reader.press("set /a 919*13\r");
    CK_CHECK(reader.sees("11947"));
}

CK_TEST(native_reader_reaches_the_real_menu_bar_and_terminal_report) {
    Fixture fixture("menus");
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    CK_CHECK(open_menu(reader, "New Terminal"));
    CK_CHECK(reader.screen().find("Copy Mode") != std::string::npos);
    CK_CHECK(reader.screen().find("^B [") != std::string::npos);
    CK_CHECK(reader.screen().find("Send Prefix to Program") != std::string::npos);
    reader.press("\x1b");
    CK_CHECK(open_menu(reader, "Terminal Report"));
    reader.press("t");
    CK_CHECK(reader.sees("Terminal report"));
    reader.press("\x1b");
    reader.press("set /a 977*11\r");
    CK_CHECK(reader.sees("10747"));
}

CK_TEST(native_reader_mouse_opens_a_real_menu) {
    Fixture fixture("mouse");
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    const auto terminal = reader.find_cell("Terminal");
    CK_CHECK(terminal.has_value());
    if (!terminal) return;
    reader.click(terminal->first, terminal->second);
    CK_CHECK(reader.sees("New Terminal"));
    CK_CHECK(reader.screen().find("Copy Mode") != std::string::npos);
    reader.press("\x1b");
    CK_CHECK(reader.stops_seeing("Copy Mode"));
    // Escape closes the dropdown one level at a time; the second leaves the
    // menu bar itself, exactly as ckVision's shared menu contract specifies.
    reader.press("\x1b");
    reader.press("set /a 991*7\r");
    CK_CHECK(reader.sees("6937"));
}

CK_TEST(native_reader_detaches_and_reattaches_to_the_same_live_shell) {
    Fixture fixture("detach");
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    reader.press("set CKMUX_NATIVE_SURVIVOR=927\r");
    reader.press("\x02" "d");
    CK_CHECK(reader.gone());
    Reader replacement;
    const bool launched = replacement.start(fixture.endpoint);
    CK_CHECK(launched);
    if (!launched) return;
    CK_CHECK(replacement.sees("13889"));
    replacement.press("set /a CKMUX_NATIVE_SURVIVOR*23\r");
    CK_CHECK(replacement.sees("21321"));
    replacement.press("\x02" "d");
    CK_CHECK(replacement.gone());
}

CK_TEST(native_reader_abrupt_client_job_close_preserves_the_live_shell) {
    Fixture fixture("close");
    if (!ready(fixture)) return;
    fixture.reader.press("set CKMUX_NATIVE_SURVIVOR=953\r");
    // Unlike Detach this terminates the actual client's owned process tree.
    // The fixture-owned server is outside that tree; this is not a proof of
    // Windows Terminal's GUI close behavior or a detached-server launch.
    fixture.reader.client->request_kill();
    CK_CHECK(fixture.reader.gone());
    Reader replacement;
    const bool launched = replacement.start(fixture.endpoint);
    CK_CHECK(launched);
    if (!launched) return;
    CK_CHECK(replacement.sees("13889"));
    replacement.press("set /a CKMUX_NATIVE_SURVIVOR*29\r");
    CK_CHECK(replacement.sees("27637"));
    replacement.quit();
}

CK_TEST(native_reader_handles_a_host_resize_while_running) {
    Fixture fixture("resize");
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    for (const auto size : {ckv::Size{80, 24}, ckv::Size{120, 36}}) {
        reader.client->resize(size, {9, 18});
        reader.settle(700);
        const auto snapshot = reader.client->snapshot();
        CK_CHECK(snapshot.cells.width == size.width);
        CK_CHECK(snapshot.cells.height == size.height);
        CK_CHECK(reader.sees("Session"));
        const bool small = size.width == 80;
        reader.press(small ? "set /a 997*31\r" : "set /a 997*41\r");
        CK_CHECK(reader.sees(small ? "30907" : "40877"));
        CK_CHECK(open_menu(reader, "Terminal Report"));
        reader.press("t");
        CK_CHECK(reader.sees("Terminal report"));
        reader.press("\x1b");
    }
}

CK_TEST(native_reader_can_name_a_terminal_while_a_child_changes_its_title) {
    Fixture fixture("title");
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    reader.press("\x02" ",");
    CK_CHECK(reader.sees("Use Default Title"));
    for (int count = 0; count < 160; ++count) reader.client->send_input("\x7f");
    reader.settle(300);
    reader.press("Native pinned title\r");
    CK_CHECK(reader.sees("Native pinned title"));
    CK_CHECK(frame_has_caption(reader, "Native pinned title"));
    reader.press("title CKMUX-NATIVE-CHILD-TITLE\r");
    reader.press("set /a 983*37\r");
    CK_CHECK(reader.sees("36371"));
    CK_CHECK(frame_has_caption(reader, "Native pinned title"));
    reader.press("\x02" "w");
    CK_CHECK(reader.sees("Window List"));
    CK_CHECK(reader.screen().find("Native pinned title") != std::string::npos);
    reader.press("\x1b");
    reader.press("\x02" ",");
    CK_CHECK(reader.sees("Use Default Title"));
    reader.press("\x1b" "d");
    CK_CHECK(reader.stops_seeing("Native pinned title"));
    CK_CHECK(reader.sees("CKMUX-NATIVE-CHILD-TITLE"));
    CK_CHECK(frame_has_caption(reader, "CKMUX-NATIVE-CHILD-TITLE"));
}

CK_TEST(native_reader_ends_the_last_session_without_killing_the_client) {
    Fixture fixture("end");
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    reader.press("\x02" "K");
    CK_CHECK(reader.sees("End session"));
    CK_CHECK(reader.screen().find("Kill anything still running") != std::string::npos);
    reader.press("\r");
    const bool empty_picker = reader.sees("No sessions are running yet", 10000);
    CK_CHECK(empty_picker);
    if (!empty_picker) {
        std::fprintf(stderr, "Last-session client state=%d pid=%lld\n",
            static_cast<int>(reader.client->state()),
            static_cast<long long>(reader.client->process_id()));
        for (const auto& row : reader.rows()) std::fprintf(stderr, "%s\n", row.c_str());
    }
    CK_CHECK(reader.screen().find("New Session") != std::string::npos);
    CK_CHECK(reader.client->state() != ckv::core::TerminalSubsessionState::Exited);
    auto& servers = ckmtest::harness_servers();
    bool exited = false;
    const auto deadline = ckmtest::clock_type::now() + std::chrono::seconds(5);
    do {
        for (auto& server : servers) {
            (void)server.session->drain(64 * 1024);
            if (server.identity == fixture.server &&
                server.session->state() == ckv::core::TerminalSubsessionState::Exited) exited = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (!exited && ckmtest::clock_type::now() < deadline);
    CK_CHECK(exited);
}
