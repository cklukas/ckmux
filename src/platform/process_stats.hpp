// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// What the processes under a terminal cost (the work queue, WP-37).
//
// Two calls, deliberately separate: snapshot the process table once — pid and
// parent pid for everything alive — and sample one pid's counters. Tree
// resolution is a pure walk over the one snapshot, so N terminals cost one
// table read per sample pass rather than N, and the walk is testable with a
// hand-built table that never saw a kernel.
//
// The counters are CUMULATIVE, and there is no clock anywhere in this seam. A
// rate — "37% of one core" — is the delta of two samples over the wall time
// between them, and that division belongs to the consumer, whose clock is
// injectable. A seam that computed rates would need to remember when it was
// last asked, and everything downstream of it would need a real second to
// pass per assertion.
//
// Two edges, so nobody discovers them in a bug report:
//
//   * A pid can die between the table and the sample. That is a skip, never
//     an error — `alive` is false and the caller counts what it could read. A
//     tree whose ROOT is gone reports zero processes, which is the honest
//     answer to "what is this terminal costing" about a terminal whose child
//     has exited.
//   * A process that daemonized has reparented away and is deliberately no
//     longer counted: it left the tree, and it is no longer this terminal's
//     cost. The walk reads the parent relation as it is, not as it was.
//
// Platforms: macOS (libproc; the server's children are the same uid, so no
// entitlement is involved) and Linux (`/proc` stat lines for the table and
// the counters, `smaps_rollup` for PSS — the one expensive read, and absent
// on pre-4.14 kernels or unreadable for a zombie, in which case `has_real`
// is false and RSS still answers). Windows rides the parking lot's ConPTY
// entry. On a platform without a fill, `process_stats_supported()` says so,
// snapshots are empty, and samples are dead — callers show nothing rather
// than zeros pretending to be measurements.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace ckm::platform {

// One process's counters at one moment.
struct ProcessSample {
    // User plus system time since the process started, in nanoseconds.
    std::uint64_t cpu_time_nanos = 0;
    // Resident set size, in bytes. Comparable across platforms and across
    // tools, and honest about its one lie: summed over a tree it counts a
    // shared page once per process that maps it.
    std::uint64_t rss_bytes = 0;
    // The platform's own "what does this actually cost": `phys_footprint` on
    // macOS (what Activity Monitor calls Memory), PSS on Linux when WP-22
    // lands. Meaningful only when `has_real` is true.
    std::uint64_t real_bytes = 0;
    bool has_real = false;
    // False: the pid was gone, or not ours to read. The other fields are
    // zero and mean nothing.
    bool alive = false;
};

// The parent relation of every live process, read at one moment.
class ProcessTable {
public:
    struct Entry {
        int pid = -1;
        int ppid = -1;
    };

    // The live system's table. Empty on a platform with no fill yet.
    static ProcessTable snapshot();

    // A table the caller states — what makes the walk testable without a
    // kernel, and the fixture for every structural case below.
    static ProcessTable from_entries(std::vector<Entry> entries);

    // `root` and every descendant present in this table, root first. Empty
    // when `root` itself is not in the table — a tree needs its root. Safe
    // against the loops a reused pid can print into a ppid column: each pid
    // is visited once.
    std::vector<int> tree_of(int root) const;

    bool empty() const noexcept { return entries_.empty(); }
    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<Entry> entries_;
};

// One pid's counters now. Dead (`alive == false`) for a pid that is gone or
// unreadable.
ProcessSample sample_process(int pid);

// Everything `tree_of(root)` could actually read, summed.
struct TreeSample {
    std::uint64_t cpu_time_nanos = 0;
    std::uint64_t rss_bytes = 0;
    std::uint64_t real_bytes = 0;
    bool has_real = false;
    // Processes that were in the tree AND readable when sampled. Zero means
    // the root is gone — the caller's cue to show nothing.
    int process_count = 0;
};
TreeSample sample_tree(const ProcessTable& table, int root);

// False on a platform whose fill has not landed. Callers show nothing rather
// than a zero pretending to be a measurement.
bool process_stats_supported() noexcept;

// --- Linux `/proc` text, parsed anywhere ---------------------------------
//
// The two file formats below are stable kernel ABI (proc(5)); only the
// READS are platform work. Splitting parse from read keeps this half of the
// Linux fill compiled and tested on every platform with fixture text — the
// same reasoning that keeps the clock out of the seam.

// What this seam needs from one `/proc/<pid>/stat` line, in the units the
// file speaks: times in clock ticks, resident set in pages. The Linux
// sampler converts with sysconf answers, which are runtime measurements and
// have no place in a parser.
struct ProcStatFields {
    int pid = -1;
    int ppid = -1;
    std::uint64_t utime_ticks = 0;
    std::uint64_t stime_ticks = 0;
    std::uint64_t rss_pages = 0;
    bool valid = false;
};

// `valid == false` on anything malformed. The trap this parser exists to
// step over: comm (field 2) is an UNESCAPED process name that may contain
// spaces and parentheses, so the fields are anchored on the LAST `)` —
// splitting on whitespace alone misreads every process named like
// `(sd-pam)` or `tmux: server`.
ProcStatFields parse_proc_stat(std::string_view text);

// The `Pss:` total from `/proc/<pid>/smaps_rollup` text, in bytes, or -1
// when absent. Matched as a whole token at line start, because the file
// also carries `Pss_Anon:`/`Pss_File:`/`Pss_Shmem:` and — the sharper trap —
// `SwapPss:`, which a substring search finds when `Pss:` itself is missing.
std::int64_t parse_smaps_rollup_pss(std::string_view text);

}  // namespace ckm::platform
