// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The process-stats seam (the work queue, WP-37): the tree walk on
// tables these tests state by hand, and the sampling on children they fork —
// bare forks, no exec and no PTY, because the thing under test is the
// measurement, not a terminal. A child here either spins, allocates, or
// pauses, and is killed by the test that made it.
//
// The Linux `/proc` parsers (WP-22's half of the seam) are tested here too,
// on fixture text stated by hand — they compile on every platform for
// exactly that reason, so this suite covers them without a `/proc`.
#include "platform/process_stats.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "cvision/testing/cktest.hpp"

namespace {

using ckm::platform::ProcessSample;
using ckm::platform::ProcessTable;
using ckm::platform::TreeSample;

using Entry = ProcessTable::Entry;

bool contains(const std::vector<int>& pids, int pid) {
    for (const int candidate : pids) {
        if (candidate == pid) return true;
    }
    return false;
}

// Forks a child that runs `body` forever and is reaped here. The body must be
// async-signal-safe-ish (a loop, an allocation, a pause) and must never
// return into the harness.
int fork_child(void (*body)()) {
    const pid_t child = ::fork();
    if (child == 0) {
        body();
        ::_exit(0);
    }
    return static_cast<int>(child);
}

void end_child(int pid) {
    if (pid <= 0) return;
    (void)::kill(static_cast<pid_t>(pid), SIGKILL);
    (void)::waitpid(static_cast<pid_t>(pid), nullptr, 0);
}

void spin_forever() {
    volatile unsigned long counter = 0;
    for (;;) counter = counter + 1;
}

void allocate_and_pause() {
    // Volatile stores, one per page: a plain memset of a buffer nothing ever
    // reads is a dead store the optimizer deletes wholesale — the first
    // version of this child paused with 960 KB resident and the test could
    // only fail honestly.
    constexpr std::size_t kBytes = 64u * 1024u * 1024u;
    volatile unsigned char* block = static_cast<unsigned char*>(::malloc(kBytes));
    if (block != nullptr) {
        for (std::size_t offset = 0; offset < kBytes; offset += 4096) block[offset] = 0xA5;
    }
    for (;;) ::pause();
}

void fork_grandchild_and_pause() {
    const pid_t grandchild = ::fork();
    if (grandchild == 0) {
        for (;;) ::pause();
    }
    for (;;) ::pause();
}

}  // namespace

CK_TEST(the_walk_returns_the_root_and_its_descendants_and_nobody_else) {
    const ProcessTable table = ProcessTable::from_entries({
        Entry{1, 0},
        Entry{10, 1},
        Entry{11, 10},
        Entry{12, 10},
        Entry{20, 12},
        Entry{99, 1},
    });
    const std::vector<int> tree = table.tree_of(10);
    CK_CHECK(tree.size() == 4U);
    CK_CHECK(tree.front() == 10);
    CK_CHECK(contains(tree, 11));
    CK_CHECK(contains(tree, 12));
    CK_CHECK(contains(tree, 20));
    CK_CHECK(!contains(tree, 99));
    CK_CHECK(!contains(tree, 1));
}

CK_TEST(a_root_the_table_does_not_hold_reports_an_empty_tree) {
    const ProcessTable table = ProcessTable::from_entries({Entry{10, 1}, Entry{11, 10}});
    CK_CHECK(table.tree_of(7).empty());
    // A leaf is its own whole tree.
    CK_CHECK(table.tree_of(11).size() == 1U);
}

CK_TEST(a_loop_a_reused_pid_printed_into_the_table_does_not_hang_the_walk) {
    // 20's parent claims to be 30 and 30's claims to be 20 — a shape a real
    // table can hold for a moment while pids are reused mid-snapshot.
    const ProcessTable table = ProcessTable::from_entries({
        Entry{10, 1},
        Entry{20, 10},
        Entry{30, 20},
        Entry{20, 30},
    });
    const std::vector<int> tree = table.tree_of(10);
    CK_CHECK(tree.size() == 3U);
    CK_CHECK(tree.front() == 10);
}

CK_TEST(this_platform_is_supported_and_sees_this_very_process) {
    CK_CHECK(ckm::platform::process_stats_supported());
    const ProcessTable table = ProcessTable::snapshot();
    CK_CHECK(!table.empty());
    const int self = static_cast<int>(::getpid());
    CK_CHECK(contains(table.tree_of(self), self));

    const ProcessSample sample = ckm::platform::sample_process(self);
    CK_CHECK(sample.alive);
    CK_CHECK(sample.rss_bytes > 0U);
    CK_CHECK(sample.has_real);
    CK_CHECK(sample.real_bytes > 0U);
}

CK_TEST(a_spinning_child_accumulates_cpu_time_at_roughly_the_rate_it_spins) {
    const int child = fork_child(spin_forever);
    CK_CHECK(child > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const ProcessSample sample = ckm::platform::sample_process(child);
    end_child(child);

    CK_CHECK(sample.alive);
    // 300 ms of spinning is at least 50 ms of CPU on any machine that can run
    // this suite, and at most a few seconds. The upper bound is the real
    // assertion: a mach-timebase conversion wrong by 125/3 in either
    // direction lands orders of magnitude outside it.
    CK_CHECK(sample.cpu_time_nanos > 50'000'000ULL);
    CK_CHECK(sample.cpu_time_nanos < 5'000'000'000ULL);
    std::printf("  [cpu] %llu ms of CPU across 300 ms of spinning\n",
                static_cast<unsigned long long>(sample.cpu_time_nanos / 1'000'000ULL));
}

CK_TEST(a_child_that_touched_64_megabytes_shows_them_in_both_memory_numbers) {
    const int child = fork_child(allocate_and_pause);
    CK_CHECK(child > 0);

    // Polled rather than slept-for: the child is allocating on its own
    // schedule, and the deadline is a guard against hanging, not the wait.
    constexpr std::uint64_t kExpected = 48u * 1024u * 1024u;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    ProcessSample sample;
    for (;;) {
        sample = ckm::platform::sample_process(child);
        if (sample.alive && sample.rss_bytes >= kExpected && sample.has_real &&
            sample.real_bytes >= kExpected) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    end_child(child);

    CK_CHECK(sample.alive);
    CK_CHECK(sample.rss_bytes >= kExpected);
    CK_CHECK(sample.has_real);
    CK_CHECK(sample.real_bytes >= kExpected);
}

CK_TEST(a_grandchild_is_part_of_the_tree_and_of_its_total) {
    const int child = fork_child(fork_grandchild_and_pause);
    CK_CHECK(child > 0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::vector<int> tree;
    for (;;) {
        tree = ProcessTable::snapshot().tree_of(child);
        if (tree.size() >= 2U) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CK_CHECK(tree.size() == 2U);

    const TreeSample total = ckm::platform::sample_tree(ProcessTable::snapshot(), child);
    CK_CHECK(total.process_count == 2);
    CK_CHECK(total.rss_bytes > 0U);

    // The grandchild reparents when its parent dies and is not ours to reap;
    // ending it by pid is exactly what the test can do.
    for (const int pid : tree) {
        if (pid != child) (void)::kill(static_cast<pid_t>(pid), SIGKILL);
    }
    end_child(child);
}

CK_TEST(a_tree_whose_root_has_exited_reports_zero_processes) {
    const pid_t child = ::fork();
    if (child == 0) ::_exit(0);
    CK_CHECK(child > 0);
    (void)::waitpid(child, nullptr, 0);

    const ProcessTable table = ProcessTable::snapshot();
    CK_CHECK(table.tree_of(static_cast<int>(child)).empty());
    CK_CHECK(ckm::platform::sample_tree(table, static_cast<int>(child)).process_count == 0);
    CK_CHECK(!ckm::platform::sample_process(static_cast<int>(child)).alive);
}

CK_TEST(one_pass_over_the_real_table_is_cheap_enough_for_a_once_a_second_tick) {
    const int self = static_cast<int>(::getpid());
    const auto started = std::chrono::steady_clock::now();
    const ProcessTable table = ProcessTable::snapshot();
    const TreeSample total = ckm::platform::sample_tree(table, self);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    CK_CHECK(total.process_count >= 1);
    std::printf("  [cost] table of %zu processes snapshotted and one tree summed in %lld us\n",
                table.size(), static_cast<long long>(micros));
    // Generous on purpose: the number above is the measurement, this bound
    // only refuses a pass that could not ride a one-second tick.
    CK_CHECK(micros < 500'000);
}

CK_TEST(the_stat_parser_reads_a_line_the_way_proc_5_says_to) {
    const ckm::platform::ProcStatFields fields = ckm::platform::parse_proc_stat(
        "4242 (spin worker) R 4200 4242 4200 34816 4242 4194304 100 0 0 0 "
        "750 250 0 0 20 0 3 0 8888888 104857600 2560\n");
    CK_CHECK(fields.valid);
    CK_CHECK(fields.pid == 4242);
    CK_CHECK(fields.ppid == 4200);
    CK_CHECK(fields.utime_ticks == 750U);
    CK_CHECK(fields.stime_ticks == 250U);
    CK_CHECK(fields.rss_pages == 2560U);
}

CK_TEST(a_comm_containing_spaces_and_parens_cannot_forge_the_fields) {
    // The comm below is `evil) R 9999 9999 9999` — a process is free to name
    // itself a fake tail. Anchored on the FIRST `)` the parser would read
    // ppid 9999 out of a process name; anchored on the last, it reads the
    // kernel's fields.
    const ckm::platform::ProcStatFields fields = ckm::platform::parse_proc_stat(
        "88 (evil) R 9999 9999 9999) S 1 88 88 0 88 4194304 0 0 0 0 "
        "10 20 0 0 20 0 1 0 5555 1048576 128\n");
    CK_CHECK(fields.valid);
    CK_CHECK(fields.pid == 88);
    CK_CHECK(fields.ppid == 1);
    CK_CHECK(fields.utime_ticks == 10U);
    CK_CHECK(fields.stime_ticks == 20U);
    CK_CHECK(fields.rss_pages == 128U);
}

CK_TEST(stat_text_that_is_not_a_stat_line_reports_invalid_not_garbage) {
    CK_CHECK(!ckm::platform::parse_proc_stat("").valid);
    CK_CHECK(!ckm::platform::parse_proc_stat("not a stat line at all").valid);
    // A truncated read: parens fine, tail too short to reach rss.
    CK_CHECK(!ckm::platform::parse_proc_stat("123 (sh) S 1 123 123 0 123\n").valid);
    // A negative where the seam needs a count.
    CK_CHECK(!ckm::platform::parse_proc_stat(
                  "77 (sh) R 1 77 77 0 77 4194304 0 0 0 0 "
                  "10 20 0 0 20 0 1 0 5555 1048576 -5\n")
                  .valid);
}

CK_TEST(the_pss_line_is_matched_as_a_token_not_a_substring) {
    // The real file, in the real order: `Pss:` present among four siblings
    // that merely start or end with it.
    const std::int64_t pss = ckm::platform::parse_smaps_rollup_pss(
        "00400000-7ffffffff000 ---p 00000000 00:00 0            [rollup]\n"
        "Rss:                5120 kB\n"
        "Pss:                2048 kB\n"
        "Pss_Anon:           1024 kB\n"
        "Pss_File:           1024 kB\n"
        "Pss_Shmem:             0 kB\n"
        "Swap:                  0 kB\n"
        "SwapPss:              64 kB\n");
    CK_CHECK(pss == 2048 * 1024);

    // The same file without its `Pss:` line. `SwapPss:` CONTAINS the token
    // and `Pss_Anon:` starts with it — a substring or prefix matcher returns
    // one of their values here instead of admitting the answer is absent.
    const std::int64_t absent = ckm::platform::parse_smaps_rollup_pss(
        "Rss:                5120 kB\n"
        "Pss_Anon:           1024 kB\n"
        "SwapPss:              64 kB\n");
    CK_CHECK(absent == -1);

    CK_CHECK(ckm::platform::parse_smaps_rollup_pss("") == -1);
}
