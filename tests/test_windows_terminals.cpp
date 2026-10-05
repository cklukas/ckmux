// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Native counterparts of test_server_terminals, using real ConPTY children.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "server/terminals.hpp"
#include "server_fixture.hpp"
#include "cvision/testing/cktest.hpp"

namespace {
using ckm::server::Terminal;
using ckm::server::Terminals;

ckm::Settings settings() {
    ckm::Settings result;
    result.shell = CKMUX_TEST_CHILD_PATH;
    result.login_shell = false;
    result.scrollback = 64;
    return result;
}

ckm::server::TerminalSpec spec(std::string command) {
    ckm::server::TerminalSpec result;
    result.command = std::move(command);
    result.working_directory = ckmtest::server_working_directory();
    result.columns = 40;
    result.rows = 10;
    return result;
}

std::string text(const Terminal& terminal) {
    std::string result;
    for (const auto& cell : terminal.snapshot().cell_buffer)
        if (!cell.is_continuation()) result += cell.grapheme();
    return result;
}

bool pump(Terminals& terminals, const std::function<bool()>& done,
          std::size_t budget = 64 * 1024) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < end) {
        (void)terminals.drain(budget);
        if (done()) return true;
        ::Sleep(2);
    }
    return done();
}

class ObservedProcess final {
public:
    explicit ObservedProcess(DWORD pid)
        : handle_(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        CK_CHECK(handle_ != nullptr);
    }
    ~ObservedProcess() { if (handle_) ::CloseHandle(handle_); }
    ObservedProcess(const ObservedProcess&) = delete;
    ObservedProcess& operator=(const ObservedProcess&) = delete;
    bool live() const { return handle_ && ::WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT; }
    bool exited() const { return handle_ && ::WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0; }
private:
    HANDLE handle_ = nullptr;
};

DWORD descendant_pid(const Terminal& terminal) {
    const std::string output = text(terminal);
    constexpr std::string_view marker = "OWNED-DESCENDANT-PID ";
    const auto at = output.find(marker);
    CK_CHECK(at != std::string::npos);
    if (at == std::string::npos) return 0;
    return static_cast<DWORD>(std::stoul(output.substr(at + marker.size())));
}
}

CK_TEST(native_terminal_ids_are_nonzero_never_recycled_and_stale_close_is_refused) {
    Terminals terminals(settings());
    const auto first = terminals.open(spec("--idle")).id();
    const auto second = terminals.open(spec("--idle")).id();
    CK_CHECK(first != 0U);
    CK_CHECK(second != first);
    CK_CHECK(terminals.close(first));
    CK_CHECK(terminals.find(first) == nullptr);
    const auto third = terminals.open(spec("--idle")).id();
    CK_CHECK(third != first && third != second);
    CK_CHECK(!terminals.close(first));
}

CK_TEST(native_terminal_exit_codes_and_silent_live_state_come_from_real_children) {
    Terminals terminals(settings());
    auto& zero = terminals.open(spec("--exit-zero"));
    auto& seven = terminals.open(spec("--exit-seven"));
    auto& quiet = terminals.open(spec("--read-exit"));
    CK_CHECK(quiet.process_id() > 0);
    CK_CHECK(quiet.live());
    CK_CHECK(!quiet.exit_status());
    CK_CHECK(pump(terminals, [&] { return zero.exit_status() && seven.exit_status(); }));
    CK_CHECK(zero.exit_status() == 0);
    CK_CHECK(seven.exit_status() == 7);
    CK_CHECK(!zero.live() && !seven.live());
    quiet.send_input("finish\r");
    CK_CHECK(pump(terminals, [&] { return quiet.exit_status().has_value(); }));
    CK_CHECK(quiet.exit_status() == 0);
}

CK_TEST(native_respawn_keeps_identity_command_and_current_child_observed_geometry) {
    Terminals terminals(settings());
    auto& terminal = terminals.open(spec("--geometry-exit"));
    const auto id = terminal.id();
    CK_CHECK(pump(terminals, [&] { return terminal.exit_status().has_value(); }));
    CK_CHECK(terminal.exit_status() == 0);
    CK_CHECK(text(terminal).find("CHILD-GEOMETRY 40 10") != std::string::npos);
    terminal.resize(72, 20, 720, 400);
    CK_CHECK(terminals.respawn(id));
    CK_CHECK(terminals.find(id) == &terminal);
    CK_CHECK(!terminal.exit_status());
    CK_CHECK(pump(terminals, [&] { return terminal.exit_status().has_value(); }));
    CK_CHECK(terminal.exit_status() == 0);
    // The child queried its actual native console, not the emulator's copy.
    CK_CHECK(text(terminal).find("CHILD-GEOMETRY 72 20") != std::string::npos);
    CK_CHECK(terminal.snapshot().cells == (ckv::Size{72, 20}));
    CK_CHECK(terminal.cell_pixels() == (ckv::PixelSize{10, 20}));
}

CK_TEST(native_live_resize_reaches_the_child_and_respawn_refuses_a_living_child) {
    Terminals terminals(settings());
    auto& terminal = terminals.open(spec("--geometry"));
    CK_CHECK(pump(terminals, [&] { return text(terminal).find("CHILD-GEOMETRY 40 10") != std::string::npos; }));
    const auto pid = terminal.process_id();
    terminal.resize(100, 30, 900, 540);
    terminal.send_input("g\r");
    CK_CHECK(pump(terminals, [&] { return text(terminal).find("CHILD-GEOMETRY 100 30") != std::string::npos; }));
    CK_CHECK(terminal.cell_pixels() == (ckv::PixelSize{9, 18}));
    CK_CHECK(!terminals.respawn(terminal.id()));
    CK_CHECK(!terminals.respawn(terminal.id() + 1000U));
    CK_CHECK(terminal.live() && terminal.process_id() == pid);
}

CK_TEST(native_respawn_repeats_the_exit_command_without_stale_status) {
    Terminals terminals(settings());
    auto& terminal = terminals.open(spec("--ran-exit-three"));
    CK_CHECK(pump(terminals, [&] { return terminal.exit_status().has_value(); }));
    CK_CHECK(terminal.exit_status() == 3);
    CK_CHECK(text(terminal).find("ran") != std::string::npos);
    CK_CHECK(terminals.respawn(terminal.id()));
    CK_CHECK(!terminal.exit_status());
    CK_CHECK(pump(terminals, [&] { return terminal.exit_status().has_value(); }));
    CK_CHECK(terminal.exit_status() == 3);
    CK_CHECK(text(terminal).find("ran") != std::string::npos);
}

CK_TEST(native_actual_child_print_capture_is_once_only_and_runtime_deny_preserves_the_spool) {
    auto config = settings();
    config.printer_mode = ckm::PrinterMode::Capture;
    Terminals terminals(config);
    auto& terminal = terminals.open(spec("--print-controller"));
    CK_CHECK(pump(terminals, [&] { return text(terminal).find("PRINT-FIXTURE-READY") != std::string::npos; }));
    CK_CHECK(terminal.print_jobs().empty());
    terminal.send_input("1\r");
    CK_CHECK(pump(terminals, [&] { return terminal.status().printer_jobs_ready > 0; }));
    const auto first = terminal.collect_print_jobs(1234);
    CK_CHECK(first.size() == 1U);
    CK_CHECK(terminal.collect_print_jobs(1235).empty());
    if (first.size() != 1U) return;
    const auto* job = terminal.print_job(first.front());
    CK_CHECK(job != nullptr);
    if (!job) return;
    CK_CHECK(job->text.find("document-1") != std::string::npos);
    CK_CHECK(job->kind == ckm::proto::PrintJobKind::Controller);
    CK_CHECK(job->at == 1234 && job->lines >= 1 && !job->overflowed);
    terminal.send_input("2\r");
    CK_CHECK(pump(terminals, [&] { return terminal.status().printer_jobs_ready > 0; }));
    const auto second = terminal.collect_print_jobs(1236);
    CK_CHECK(second.size() == 1U);
    if (second.size() != 1U) return;
    CK_CHECK(second.front() != first.front());
    CK_CHECK(terminal.discard_print_jobs(first.front()));
    CK_CHECK(terminal.print_job(first.front()) == nullptr);
    CK_CHECK(!terminal.discard_print_jobs(first.front()));
    terminal.set_printer_policy(ckv::term::TerminalPrinterPolicy::Deny);
    terminal.send_input("3\r");
    CK_CHECK(pump(terminals, [&] { return text(terminal).find("PRINT-DONE-3") != std::string::npos; }));
    CK_CHECK(terminal.collect_print_jobs(1237).empty());
    CK_CHECK(terminal.print_jobs().size() == 1U);
    CK_CHECK(terminal.print_job(second.front()) != nullptr);
    CK_CHECK(terminal.discard_print_jobs(0));
    CK_CHECK(terminal.print_jobs().empty());
    CK_CHECK(!terminal.discard_print_jobs(0));
}

CK_TEST(native_noisy_children_share_one_drain_budget_without_starving_a_quiet_child) {
    Terminals terminals(settings());
    auto& first = terminals.open(spec("--flood-first"));
    auto& second = terminals.open(spec("--flood-second"));
    auto& third = terminals.open(spec("--flood-third"));
    auto& quiet = terminals.open(spec(""));
    quiet.send_input("quiet-before\r");
    CK_CHECK(pump(terminals, [&] {
        return text(first).find("first-child") != std::string::npos &&
            text(second).find("second-child") != std::string::npos &&
            text(third).find("third-child") != std::string::npos &&
            text(quiet).find("QUIET-BEFORE") != std::string::npos;
    }, 4096));
    quiet.send_input("quiet-progress\r");
    CK_CHECK(pump(terminals, [&] { return text(quiet).find("QUIET-PROGRESS") != std::string::npos; }, 4096));
    constexpr std::size_t budget = 64 * 1024;
    std::size_t largest = 0;
    for (int pass = 0; pass < 6; ++pass) {
        ::Sleep(20);
        for (const auto id : terminals.ids()) terminals.find(id)->clear_damage();
        (void)terminals.drain(budget);
        std::size_t lines = 0;
        for (const auto id : terminals.ids()) lines += terminals.find(id)->damage().scrollback_pushed;
        largest = std::max(largest, lines);
    }
    CK_CHECK(largest > 0U);
    CK_CHECK(largest <= budget / 12 + 4U * 30U);
    CK_CHECK(largest < 2U * (budget / 12));
}

CK_TEST(native_terminal_grace_keeps_other_children_working_then_kills_the_whole_owned_tree) {
    Terminals terminals(settings());
    auto& stubborn = terminals.open(spec("--stubborn-root"));
    auto& quiet = terminals.open(spec(""));
    quiet.send_input("before-grace\r");
    CK_CHECK(pump(terminals, [&] { return text(stubborn).find("OWNED-DESCENDANT-PID ") != std::string::npos &&
        text(quiet).find("BEFORE-GRACE") != std::string::npos; }));
    ObservedProcess root(static_cast<DWORD>(stubborn.process_id()));
    ObservedProcess descendant(descendant_pid(stubborn));
    CK_CHECK(root.live() && descendant.live());
    stubborn.request_termination();
    quiet.send_input("working-during-grace\r");
    CK_CHECK(pump(terminals, [&] { return text(quiet).find("WORKING-DURING-GRACE") != std::string::npos; }));
    CK_CHECK(root.live() && descendant.live());
    CK_CHECK(quiet.live());
    stubborn.request_kill();
    CK_CHECK(pump(terminals, [&] { return root.exited() && descendant.exited(); }));
    CK_CHECK(quiet.live());
}

CK_TEST(native_terminal_collection_destruction_reaps_observed_root_and_descendant_handles) {
    auto terminals = std::make_unique<Terminals>(settings());
    auto& terminal = terminals->open(spec("--stubborn-root"));
    CK_CHECK(pump(*terminals, [&] { return text(terminal).find("OWNED-DESCENDANT-PID ") != std::string::npos; }));
    ObservedProcess root(static_cast<DWORD>(terminal.process_id()));
    ObservedProcess descendant(descendant_pid(terminal));
    CK_CHECK(root.live() && descendant.live());
    terminals.reset();
    // Job termination is asynchronous. Observe the retained handles within
    // the same bounded deadline as explicit termination, not by PID lookup
    // or an immediate scheduler-race assertion.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((!root.exited() || !descendant.exited()) && std::chrono::steady_clock::now() < end)
        ::Sleep(2);
    CK_CHECK(root.exited() && descendant.exited());
}
