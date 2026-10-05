// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Actual ckmux client/server executables and the real system cmd.exe under
// ckVision's native ConPTY adapter. No echoed helper stands in for a shell.
#include "reader_harness.hpp"
#include <fstream>

namespace {
using ckmtest::Reader;
struct Fixture {
    std::filesystem::path endpoint;
    ckv::core::ProcessId server = -1;
    Reader reader;
    std::vector<std::pair<std::string, std::string>> environment;
    std::string_view startup_stage = "server launch";
    explicit Fixture(std::string_view name) : endpoint(ckmtest::private_socket(name)) {}
    ~Fixture() { reader.quit(); ckmtest::end_process(server); ckmtest::forget(endpoint); }
    bool start() {
        server = ckmtest::start_server(endpoint);
        if (server <= 0) return false;
        startup_stage = "endpoint readiness";
        if (!ckmtest::wait_for_socket(endpoint)) return false;
        startup_stage = "client launch";
        if (!reader.start(endpoint, {110, 32}, environment)) return false;
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

void graphic_child(Reader& reader, std::string_view mode) {
    reader.press("\"" + std::string(CKMUX_TEST_CHILD_PATH) + "\" " + std::string(mode) + "\r");
}

bool red_picture(Reader& reader) {
    const auto snapshot = reader.client->snapshot();
    for (const auto& raster : snapshot.rasters) {
        if (!raster.image) continue;
        const auto pixel = raster.image->pixel(0, 0);
        if (pixel.r == 255 && pixel.g == 0 && pixel.b == 0 && pixel.a == 255) return true;
    }
    return false;
}

void check_picture_containment(Reader& reader) {
    const auto snapshot = reader.client->snapshot();
    CK_CHECK(!snapshot.rasters.empty());
    for (const auto& raster : snapshot.rasters) {
        // The actual client's emitted frame is decoded here. Its menu/footer
        // must remain outside every picture, before and after host resizing.
        CK_CHECK(raster.anchor.x >= 0 && raster.anchor.y > 0);
        CK_CHECK(raster.anchor.x + raster.cell_extent.width <= snapshot.cells.width);
        CK_CHECK(raster.anchor.y + raster.cell_extent.height < snapshot.cells.height);
    }
}
} // namespace

CK_TEST(native_actual_client_renders_child_sixel_and_keeps_it_contained_during_resize) {
    Fixture fixture("graphics");
    const auto trace_path = fixture.reader.home.path() / "graphics.log";
    fixture.environment = {{"CKVISION_GRAPHICS_LOG", trace_path.string()}};
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    CK_CHECK(reader.client->profile().sixel);
    graphic_child(reader, "--graphics-generator");
    CK_CHECK(reader.sees("NATIVE-GRAPHIC-DONE"));
    const auto deadline = ckmtest::clock_type::now() + std::chrono::seconds(6);
    while (!red_picture(reader) && ckmtest::clock_type::now() < deadline) reader.settle(20);
    if (!red_picture(reader)) {
        std::ifstream trace(trace_path);
        for (std::string line; std::getline(trace, line);)
            std::fprintf(stderr, "Native graphics trace: %s\n", line.c_str());
        for (const auto& row : reader.rows()) std::fprintf(stderr, "%s\n", row.c_str());
    }
    CK_CHECK(red_picture(reader));
    check_picture_containment(reader);
    if (!red_picture(reader)) {
        reader.press("\x02" "c");
        reader.press("set /a 929*59\r");
        const bool responding = reader.sees("54811");
        graphic_child(reader, "--graphics-generator");
        reader.settle(1200);
        std::fprintf(stderr, "Native second-terminal diagnostics: responding=%d red-picture=%d\n",
            responding ? 1 : 0, red_picture(reader) ? 1 : 0);
    }
    reader.client->resize({80, 24}, {9, 18});
    reader.settle(800);
    CK_CHECK(reader.client->snapshot().cells == (ckv::Size{80, 24}));
    CK_CHECK(red_picture(reader));
    check_picture_containment(reader);
    CK_CHECK(open_menu(reader, "Terminal Report"));
    reader.press("t");
    CK_CHECK(reader.sees("Terminal report"));
    reader.press("\x1b");
    reader.press("set /a 947*43\r");
    CK_CHECK(reader.sees("40721"));
}

CK_TEST(native_actual_client_protects_a_text_only_host_from_unconditional_child_sixel) {
    Fixture fixture("no-graphics");
    fixture.reader.host_profile.sixel = false;
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    CK_CHECK(!reader.client->profile().sixel);
    graphic_child(reader, "--graphics-visible");
    // A positive partner proves the reader can show the payload characters.
    CK_CHECK(reader.sees("VISIBLE-PAYLOAD #0;2;100;0;0 !36~"));
    graphic_child(reader, "--graphics-generator");
    CK_CHECK(reader.sees("NATIVE-GRAPHIC-DONE"));
    CK_CHECK(reader.client->snapshot().rasters.empty());
    CK_CHECK(reader.stops_seeing("#0;2;100;0;0"));
    CK_CHECK(reader.stops_seeing("!36~"));
    CK_CHECK(reader.stops_seeing("\"1;1;36;18"));
    reader.press("set /a 941*47\r");
    CK_CHECK(reader.sees("44227"));
}

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
