// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "platform/process_stats.hpp"

#include <unordered_map>
#include <unordered_set>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach_time.h>
#endif

namespace ckm::platform {

ProcessTable ProcessTable::from_entries(std::vector<Entry> entries) {
    ProcessTable table;
    table.entries_ = std::move(entries);
    return table;
}

std::vector<int> ProcessTable::tree_of(int root) const {
    bool present = false;
    for (const Entry& entry : entries_) {
        if (entry.pid == root) {
            present = true;
            break;
        }
    }
    if (!present) return {};

    std::unordered_map<int, std::vector<int>> children;
    children.reserve(entries_.size());
    for (const Entry& entry : entries_) children[entry.ppid].push_back(entry.pid);

    // Each pid is visited once: a reused pid can print a loop into a ppid
    // column read mid-churn, and a walk that trusted it would never return.
    std::vector<int> result;
    std::unordered_set<int> visited;
    std::vector<int> frontier{root};
    visited.insert(root);
    while (!frontier.empty()) {
        const int pid = frontier.back();
        frontier.pop_back();
        result.push_back(pid);
        const auto found = children.find(pid);
        if (found == children.end()) continue;
        for (const int child : found->second) {
            if (visited.insert(child).second) frontier.push_back(child);
        }
    }
    return result;
}

TreeSample sample_tree(const ProcessTable& table, int root) {
    TreeSample total;
    for (const int pid : table.tree_of(root)) {
        const ProcessSample sample = sample_process(pid);
        if (!sample.alive) continue;
        total.cpu_time_nanos += sample.cpu_time_nanos;
        total.rss_bytes += sample.rss_bytes;
        if (sample.has_real) {
            total.real_bytes += sample.real_bytes;
            total.has_real = true;
        }
        ++total.process_count;
    }
    return total;
}

#if defined(__APPLE__)

namespace {

// `proc_pid_rusage` reports times in mach absolute time units, which are
// nanoseconds on Intel (1/1) and 24 MHz ticks on Apple silicon (125/3). The
// split arithmetic keeps the conversion exact without overflowing: whole
// multiples of the denominator first, the remainder scaled after.
std::uint64_t mach_ticks_to_nanos(std::uint64_t ticks) {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        (void)mach_timebase_info(&info);
        return info;
    }();
    if (timebase.numer == timebase.denom || timebase.denom == 0) return ticks;
    return (ticks / timebase.denom) * timebase.numer +
           (ticks % timebase.denom) * timebase.numer / timebase.denom;
}

}  // namespace

ProcessTable ProcessTable::snapshot() {
    ProcessTable table;
    const int bytes_needed = proc_listallpids(nullptr, 0);
    if (bytes_needed <= 0) return table;
    // Headroom for processes born between the two calls; the second call
    // returns how many it actually wrote.
    std::vector<pid_t> pids(static_cast<std::size_t>(bytes_needed) / sizeof(pid_t) + 64);
    const int written =
        proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (written <= 0) return table;
    pids.resize(static_cast<std::size_t>(written));

    table.entries_.reserve(pids.size());
    for (const pid_t pid : pids) {
        if (pid <= 0) continue;
        proc_bsdshortinfo info{};
        const int got = proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &info,
                                     PROC_PIDT_SHORTBSDINFO_SIZE);
        // Gone since the listing, or not ours to ask about: skipped, exactly
        // as the header promises.
        if (got != PROC_PIDT_SHORTBSDINFO_SIZE) continue;
        table.entries_.push_back(Entry{static_cast<int>(pid), static_cast<int>(info.pbsi_ppid)});
    }
    return table;
}

ProcessSample sample_process(int pid) {
    ProcessSample sample;
    if (pid <= 0) return sample;
    rusage_info_v2 usage{};
    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage)) != 0) {
        return sample;
    }
    sample.cpu_time_nanos =
        mach_ticks_to_nanos(usage.ri_user_time) + mach_ticks_to_nanos(usage.ri_system_time);
    sample.rss_bytes = usage.ri_resident_size;
    // What Activity Monitor calls Memory: the process's own dirty, compressed
    // and IOKit-mapped pages, without billing it for pages it merely shares.
    sample.real_bytes = usage.ri_phys_footprint;
    sample.has_real = true;
    sample.alive = true;
    return sample;
}

bool process_stats_supported() noexcept { return true; }

#else

ProcessTable ProcessTable::snapshot() { return {}; }

ProcessSample sample_process(int) { return {}; }

// The Linux fill (`/proc`, `smaps_rollup`) is WP-22's; Windows rides the
// parking lot's ConPTY entry. Until then this platform says so instead of
// returning zeros that look like measurements.
bool process_stats_supported() noexcept { return false; }

#endif

}  // namespace ckm::platform
