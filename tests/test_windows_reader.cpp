// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Actual ckmux client/server executables and the real system cmd.exe under
// ckVision's native ConPTY adapter. No echoed helper stands in for a shell.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include "reader_harness.hpp"
#include "client/server_connection.hpp"
#include "client/server_session.hpp"
#include "platform/paths.hpp"
#include <fstream>
#include <iterator>
#include "windows_print_observer.hpp"
#include "windows_clipboard_fixture.hpp"

namespace {
using ckmtest::Reader;

bool actual_binary(ckv::core::ProcessId identity) {
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        static_cast<DWORD>(identity));
    if (!process) return false;
    std::array<wchar_t, 32768> image{};
    DWORD length = static_cast<DWORD>(image.size());
    const bool queried = ::QueryFullProcessImageNameW(process, 0, image.data(), &length) != FALSE;
    (void)::CloseHandle(process);
    if (!queried) return false;
    const std::filesystem::path actual(std::wstring(image.data(), length));
    std::error_code error;
    const bool matches = std::filesystem::equivalent(actual, ckmtest::binary_path(), error) && !error;
    std::fprintf(stderr, "Native acceptance PID %lld executable=%s selected-match=%d\n",
        static_cast<long long>(identity), ckm::platform::path_text(actual).c_str(), matches ? 1 : 0);
    return matches;
}

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
        startup_stage = "actual server executable";
        if (!actual_binary(server)) return false;
        startup_stage = "endpoint readiness";
        if (!ckmtest::wait_for_socket(endpoint)) return false;
        startup_stage = "client launch";
        if (!reader.start(endpoint, {110, 32}, environment)) return false;
        startup_stage = "actual client executable";
        if (!actual_binary(reader.client->process_id())) return false;
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

bool click_on_row(Reader& reader, std::string_view anchor, std::string_view label) {
    const auto location = reader.find_cell(anchor);
    if (!location) return false;
    const auto snapshot = reader.client->snapshot();
    std::string text;
    std::vector<int> columns;
    for (int x = 0; x < snapshot.cells.width; ++x) {
        const auto& cell = snapshot.cell_buffer[static_cast<std::size_t>(location->first) *
            static_cast<std::size_t>(snapshot.cells.width) + static_cast<std::size_t>(x)];
        if (cell.is_continuation()) continue;
        const auto grapheme = cell.grapheme();
        text += grapheme;
        for (std::size_t i = 0; i < grapheme.size(); ++i) columns.push_back(x);
    }
    const auto found = text.find(label);
    if (found == std::string::npos) return false;
    reader.click(location->first, columns[found] + 1);
    return true;
}

std::optional<std::filesystem::path> saved_capture(const std::filesystem::path& folder) {
    std::error_code error;
    std::optional<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().filename() == ".ckvision-write.lock") continue;
        // An unexpected second file or leaked intermediate is not a save.
        if (!entry.is_regular_file(error) || error || result) return std::nullopt;
        result = entry.path();
    }
    return error ? std::nullopt : result;
}

void print_and_save(std::string_view format) {
    Fixture fixture(format == "txt" ? "print-text" : "print-ansi");
    const auto folder = fixture.reader.home.path() / std::filesystem::path(u8"Print Grüße 日本");
    const auto config = fixture.reader.home.path() / "print.ini";
    CK_CHECK(std::filesystem::create_directory(folder));
    const auto folder_utf8 = folder.u8string();
    {
        std::ofstream file(config, std::ios::binary);
        file << "[printer]\nmode=ask\nsave-format=" << format << "\nsave-folder=";
        file.write(reinterpret_cast<const char*>(folder_utf8.data()),
                   static_cast<std::streamsize>(folder_utf8.size()));
        file << "\nsave-ask-name=false\n";
        CK_CHECK(file.good());
    }
    fixture.environment = {{"CKMUX_CONFIG", config.string()}};
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    graphic_child(reader, "--print-ui-controller");
    CK_CHECK(reader.sees("PRINT-UI-READY"));
    // The legitimate shell/controller/ConPTY hosts are already running.
    // Observe the real client and server throughout printing, UI and saving.
    ckmtest::PrintObserver observer(reader, static_cast<DWORD>(fixture.server));
    CK_CHECK(observer.healthy);
    CK_CHECK(observer.baseline.size() > observer.roots.size());
    reader.press("1\r");
    CK_CHECK(reader.sees("PRINT-UI-DONE"));
    CK_CHECK(reader.sees("PRINT?"));
    const auto frame = reader.find_cell("PRINT?");
    CK_CHECK(frame.has_value());
    if (!frame) return;
    reader.click(frame->first, frame->second + 2);
    CK_CHECK(reader.sees("Keep capturing"));
    CK_CHECK(click_on_row(reader, "Keep capturing", "Keep"));
    CK_CHECK(click_on_row(reader, "Cancel", "OK"));
    CK_CHECK(reader.stops_seeing("Keep capturing"));
    // The child readiness marker also contains PRINT. Require the actual
    // framed button rather than clicking text inside the child's terminal.
    const auto kept = reader.find_cell("[ PRINT");
    CK_CHECK(kept.has_value());
    if (!kept) return;
    reader.click(kept->first, kept->second + 4);
    CK_CHECK(reader.sees("1 capture"));
    CK_CHECK(reader.sees("Printed by the program"));
    CK_CHECK(!saved_capture(folder));
    CK_CHECK(click_on_row(reader, "Discard all", "Save"));
    const auto deadline = ckmtest::clock_type::now() + std::chrono::seconds(6);
    auto saved = saved_capture(folder);
    while (!saved && ckmtest::clock_type::now() < deadline) {
        reader.settle(20);
        saved = saved_capture(folder);
    }
    if (!saved) std::fprintf(stderr, "Native print save screen: %s\n", reader.screen().c_str());
    CK_CHECK(saved.has_value());
    if (!saved) return;
    CK_CHECK(saved->parent_path() == folder);
    CK_CHECK(saved->extension() == (format == "txt" ? ".txt" : ".ansi"));
    std::ifstream file(*saved, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::fprintf(stderr, "Native print %.*s saved %zu bytes:",
        static_cast<int>(format.size()), format.data(), bytes.size());
    for (const unsigned char byte : bytes) std::fprintf(stderr, " %02x", static_cast<unsigned int>(byte));
    std::fprintf(stderr, "\n");
    CK_CHECK(bytes == (format == "txt" ? "UI-SPOOLBOLD\n" : "UI-SPOOL\x1b[1mBOLD\x1b[0m\r\n"));
    observer.sample();
    std::fprintf(stderr, "Native print observation: samples=%zu handle-samples=%zu "
        "healthy=%d new-children=%zu printer-endpoints=%zu\n", observer.samples,
        observer.named_handle_samples, observer.healthy ? 1 : 0,
        observer.unexpected_children.size(), observer.unexpected_endpoints.size());
    CK_CHECK(observer.healthy);
    CK_CHECK(observer.samples >= 10);
    CK_CHECK(observer.named_handle_samples == observer.samples * observer.roots.size());
    CK_CHECK(observer.unexpected_children.empty());
    CK_CHECK(observer.unexpected_endpoints.empty());
    // Sampled process/handle observation is not an exhaustive kernel trace or
    // proof about unobserved printer RPC mechanisms. Its controls are below.
}
} // namespace

CK_TEST(native_print_observer_detects_a_real_pipe_and_a_real_owned_child) {
    const std::wstring name = L"\\\\.\\pipe\\ckmux-observer-printer-" +
        std::to_wstring(::GetCurrentProcessId());
    const HANDLE pipe = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 64, 64, 0, nullptr);
    CK_CHECK(pipe != INVALID_HANDLE_VALUE);
    if (pipe == INVALID_HANDLE_VALUE) return;
    const auto witness_name = ckmtest::file_object_name(::GetCurrentProcess(), pipe);
    CK_CHECK(witness_name && ckmtest::printer_endpoint(*witness_name));
    // An invalid process is an observer failure, never a vanished endpoint.
    CK_CHECK(!ckmtest::file_object_name(nullptr, pipe));
    const auto before = ckmtest::named_handles(::GetCurrentProcessId());
    CK_CHECK(before.has_value());
    CK_CHECK(before && std::any_of(before->begin(), before->end(), ckmtest::printer_endpoint));
    CK_CHECK(::CloseHandle(pipe) != FALSE);
    // Reproduce a source handle closing after a census, before duplication.
    const auto closed_name = ckmtest::file_object_name(::GetCurrentProcess(), pipe);
    CK_CHECK(closed_name && closed_name->empty());
    const auto after = ckmtest::named_handles(::GetCurrentProcessId());
    CK_CHECK(after.has_value());
    CK_CHECK(after && std::none_of(after->begin(), after->end(), ckmtest::printer_endpoint));
    CK_CHECK(ckmtest::printer_endpoint(L"\\Device\\Parallel0"));
    CK_CHECK(ckmtest::printer_endpoint(L"\\Device\\UsbPrint0"));
    CK_CHECK(ckmtest::printer_endpoint(L"\\Device\\NamedPipe\\SPOOLSS"));
    CK_CHECK(!ckmtest::printer_endpoint(L"\\Device\\NamedPipe\\ckmux-ipc"));

    const auto baseline = ckmtest::process_parents();
    CK_CHECK(baseline.has_value());
    if (!baseline) return;
    Reader child;
    auto launch = ckv::term::TerminalLaunchSpec::program(CKMUX_TEST_CHILD_PATH, {"--echo"});
    launch.working_directory = child.home.path().string();
    launch.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
    child.client = ckv::term::launch_terminal_subsession(std::move(launch));
    CK_CHECK(child.sees("CKMUX-ECHO-READY"));
    const auto running = ckmtest::process_parents();
    CK_CHECK(running.has_value());
    if (!running) return;
    const std::set<DWORD> roots{::GetCurrentProcessId()};
    const auto old = ckmtest::descendants(*baseline, roots);
    const auto current = ckmtest::descendants(*running, roots);
    const auto pid = static_cast<DWORD>(child.client->process_id());
    CK_CHECK(!old.contains(pid));
    CK_CHECK(current.contains(pid));
    std::fprintf(stderr, "Native observer controls: real pipe seen and released; "
        "owned child PID=%lu is newly observed\n", static_cast<unsigned long>(pid));
    child.quit();
}

CK_TEST(native_clipboard_context_requires_the_actual_noninteractive_logon) {
    const ckmtest::ClipboardIsolation isolation;
    CK_CHECK(isolation.safe);
    if (!isolation.safe) return; // Failed assertion, never an interactive fallback.
    CK_CHECK(isolation.includes(::GetCurrentProcessId()));
    auto rejected = isolation;
    rejected.safe = false;
    CK_CHECK(!rejected.includes(::GetCurrentProcessId()));
    LUID distinct = *isolation.logon;
    distinct.LowPart ^= 1;
    CK_CHECK(!ckmtest::same_authentication(*isolation.logon, distinct));
    Fixture fixture("clipboard-context");
    if (!ready(fixture)) return;
    CK_CHECK(isolation.includes(static_cast<DWORD>(fixture.server)));
    CK_CHECK(isolation.includes(static_cast<DWORD>(fixture.reader.client->process_id())));
}

namespace {
bool native_yank(Reader& reader) {
    reader.press("\x02" "[");
    if (!reader.sees("COPY ")) {
        std::fprintf(stderr, "Actual copy-mode entry screen:\n%s\n", reader.screen().c_str());
        return false;
    }
    reader.press("/NATIVE-COPY-\r");
    reader.press("y");
    const bool left = reader.stops_seeing("COPY ", 3000);
    if (!left) std::fprintf(stderr, "Actual copy-mode exit screen:\n%s\n", reader.screen().c_str());
    return left;
}

void actual_clipboard_copy(bool locked) {
    const ckmtest::ClipboardIsolation isolation;
    CK_CHECK(isolation.safe);
    if (!isolation.safe) return;
    Fixture fixture(locked ? "clipboard-locked" : "clipboard-unicode");
    const auto config = fixture.reader.home.path() / "clipboard.ini";
    {
        std::ofstream file(config, std::ios::binary);
        file << "[terminal]\nclipboard=osc52\n";
        CK_CHECK(file.good());
    }
    fixture.environment = {{"CKMUX_CONFIG", config.string()}};
    if (!ready(fixture)) return;
    const bool server_safe = isolation.includes(static_cast<DWORD>(fixture.server));
    const bool client_safe = isolation.includes(static_cast<DWORD>(fixture.reader.client->process_id()));
    CK_CHECK(server_safe && client_safe);
    if (!server_safe || !client_safe) return;
    // Both actual child tokens match the caller's noninteractive logon. The
    // ckVision launch uses no inherited handles or explicit lpDesktop, so its
    // first USER32 connection selects that logon's default service station.
    auto& reader = fixture.reader;
    graphic_child(reader, "--copy-controller");
    CK_CHECK(reader.sees("COPY-CONTROLLER-READY"));
    ckv::term::WindowsClipboardWriter sentinel;
    CK_CHECK(sentinel.write_text("prior isolated clipboard").accepted());
    CK_CHECK(ckmtest::native_clipboard_text() == L"prior isolated clipboard");
    if (locked) {
        ckmtest::ClipboardLock lock;
        CK_CHECK(lock.held);
        if (!lock.held) return;
        CK_CHECK(native_yank(reader));
        CK_CHECK(reader.sees("The copy did not reach host clipboard"));
        CK_CHECK(reader.sees("ckmux kept it"));
        CK_CHECK(lock.release());
        CK_CHECK(ckmtest::native_clipboard_text() == L"prior isolated clipboard");
        reader.press("\r");
        reader.press("\x02" "]");
        reader.press("\r");
        CK_CHECK(reader.sees("INTERNAL-PASTE-VERIFIED"));
    }
    CK_CHECK(native_yank(reader));
    const auto copied = ckmtest::native_clipboard_text();
    const bool unicode_copied = copied == L"NATIVE-COPY-\u03A9\u4E2D\U0001F600";
    CK_CHECK(unicode_copied);
    if (!unicode_copied) std::fprintf(stderr, "Actual clipboard text: %ls\n",
                                     copied ? copied->c_str() : L"<unavailable>");
    CK_CHECK(reader.screen().find("The copy did not reach") == std::string::npos);
    const auto handles = ckmtest::named_handles(static_cast<DWORD>(reader.client->process_id()));
    CK_CHECK(handles.has_value());
    bool actual_station_seen = false;
    if (handles) for (const auto& name : *handles) {
        if (name.ends_with(L"\\" + isolation.station_name)) actual_station_seen = true;
        if (name.find(L"WindowStations") != std::wstring::npos)
            std::fprintf(stderr, "Actual client window station handle: %ls\n", name.c_str());
    }
    CK_CHECK(actual_station_seen);
    std::fprintf(stderr, "Actual native copy: unicode=%d locked-case=%d station-handle-match=%d\n",
                 unicode_copied ? 1 : 0, locked ? 1 : 0, actual_station_seen ? 1 : 0);
}
} // namespace

CK_TEST(native_actual_client_clipboard_copy_publishes_unicode) { actual_clipboard_copy(false); }
CK_TEST(native_actual_client_clipboard_copy_preserves_locked_external_and_internal_text_then_recovers) {
    actual_clipboard_copy(true);
}

CK_TEST(native_actual_client_print_capture_saves_plain_text_in_a_unicode_folder) {
    print_and_save("txt");
}

CK_TEST(native_actual_client_print_capture_saves_ansi_bytes_in_a_unicode_folder) {
    print_and_save("ansi");
}

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

#if CKMUX_TEST_STRESS
CK_TEST(native_flooding_child_keeps_attached_and_idle_wire_readers_and_real_menus_responsive) {
    Fixture fixture("flood-readiness");
    if (!ready(fixture)) return;
    auto& reader = fixture.reader;
    graphic_child(reader, "--flood-first");
    CK_CHECK(reader.sees("first-child"));

    // Both protocol readers use the actual server process and native named
    // pipes. The attached one decodes real deltas into the production mirror;
    // the idle one receives no screen traffic. The original UI remains joined.
    struct WireReader {
        ckm::client::ServerConnection connection;
        ckm::proto::FrameReader frames;
        ckm::client::ServerSession session;
        std::uint64_t last_pong = 0;
        std::size_t received = 0;
        WireReader() : session([this](const ckm::proto::Message& message) {
            CK_CHECK(connection.stream.send(ckm::proto::encode(message)));
        }) {}
        bool connect(const std::filesystem::path& endpoint, bool attached) {
            connection = ckm::client::connect_to_server(endpoint, ckmtest::binary_path(),
                attached ? ckm::proto::ClientKind::Ui : ckm::proto::ClientKind::Cli);
            if (!connection.ok()) return false;
            if (attached) {
                session.set_attach_mode(ckm::proto::AttachMode::Join);
                session.attach(0, {110, 32}, {9, 18});
            }
            return true;
        }
        void pump() {
            CK_CHECK(connection.stream.flush());
            std::string bytes;
            CK_CHECK(connection.stream.receive(bytes));
            received += bytes.size();
            if (!bytes.empty()) CK_CHECK(frames.append(bytes));
            for (;;) {
                ckm::proto::Message message;
                const auto decoded = frames.next(message);
                if (decoded == ckm::proto::DecodeError::Incomplete) break;
                CK_CHECK(decoded == ckm::proto::DecodeError::None);
                if (decoded != ckm::proto::DecodeError::None) break;
                (void)session.handle(message);
                if (const auto* pong = std::get_if<ckm::proto::Pong>(&message))
                    last_pong = pong->nonce;
            }
            session.heal_if_needed();
        }
        bool shows_flood() {
            for (const auto id : session.terminal_ids()) {
                const auto* mirror = session.terminal(id);
                if (!mirror) continue;
                std::string text;
                for (const auto& cell : mirror->cells())
                    if (!cell.is_continuation()) text += cell.grapheme();
                if (text.find("first-child") != std::string::npos) return true;
            }
            return false;
        }
    };
    WireReader attached, idle;
    const bool attached_connected = attached.connect(fixture.endpoint, true);
    const bool idle_connected = idle.connect(fixture.endpoint, false);
    CK_CHECK(attached_connected);
    CK_CHECK(idle_connected);
    if (!attached_connected || !idle_connected) return;
    const auto warmup_end = ckmtest::clock_type::now() + std::chrono::seconds(6);
    while ((!attached.session.attached() || !attached.shows_flood()) &&
           ckmtest::clock_type::now() < warmup_end) {
        attached.pump();
        idle.pump();
        reader.settle(10);
    }
    CK_CHECK(attached.session.attached());
    CK_CHECK(attached.shows_flood());
    if (!attached.session.attached() || !attached.shows_flood()) return;
    const auto bytes_before = attached.received;
#if defined(__SANITIZE_ADDRESS__)
    constexpr double budget_ms = 5000.0;
#else
    constexpr double budget_ms = 3000.0;
#endif
    double attached_worst = 0.0, idle_worst = 0.0;
    for (std::uint64_t attempt = 1; attempt <= 10; ++attempt) {
        for (auto* wire : {&attached, &idle}) {
            const auto nonce = attempt + (wire == &idle ? 100 : 0);
            const auto sent = ckmtest::clock_type::now();
            wire->session.request(ckm::proto::Ping{nonce});
            const auto deadline = sent + std::chrono::milliseconds(
                static_cast<long long>(budget_ms * 2.0));
            while (wire->last_pong != nonce && ckmtest::clock_type::now() < deadline) {
                attached.pump();
                idle.pump();
                reader.settle(10);
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(
                ckmtest::clock_type::now() - sent).count();
            CK_CHECK(wire->last_pong == nonce);
            CK_CHECK(elapsed < budget_ms);
            auto& worst = wire == &attached ? attached_worst : idle_worst;
            worst = std::max(worst, elapsed);
            if (wire->last_pong != nonce) return;
        }
    }
    CK_CHECK(attached.received > bytes_before);
    CK_CHECK(attached.shows_flood());
    std::fprintf(stderr, "Native actual-server flood: attached-worst=%.1fms idle-worst=%.1fms received=%zu bytes\n",
        attached_worst, idle_worst, attached.received - bytes_before);
    reader.press("\x02" "w");
    CK_CHECK(reader.sees("Window List"));
    reader.press("\x1b");
    CK_CHECK(reader.stops_seeing("w windows", 8000));
}

#endif

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
    // More output than fits on screen. Computed answers are absent from the
    // input echo, so searching them after reattach proves retained history,
    // not the command string or the search prompt itself.
    reader.press("for /L %n in (1,1,75) do @(set /a 700000+%n*13& echo.)\r");
    CK_CHECK(reader.sees("700975"));
    CK_CHECK(reader.screen().find("700013") == std::string::npos);
    reader.press("\x02" "d");
    CK_CHECK(reader.gone());
    Reader replacement;
    const bool launched = replacement.start(fixture.endpoint);
    CK_CHECK(launched);
    if (!launched) return;
    CK_CHECK(replacement.sees("700975"));
    replacement.press("\x02" "[");
    CK_CHECK(replacement.sees("COPY "));
    replacement.press("/NATIVE-NO-SUCH-HISTORY-ITEM\r");
    CK_CHECK(replacement.sees("not found"));
    replacement.press("/700013\r");
    CK_CHECK(replacement.stops_seeing("not found"));
    // Search text is also present on the status row. Require a separate
    // payload row containing the computed oldest answer too.
    const auto rows = replacement.rows();
    CK_CHECK(std::count_if(rows.begin(), rows.end(), [](const std::string& row) {
        return row.find("700013") != std::string::npos;
    }) >= 2);
    replacement.press("q");
    CK_CHECK(replacement.stops_seeing("COPY "));
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
