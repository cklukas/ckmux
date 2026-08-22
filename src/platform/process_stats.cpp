// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/process_stats.hpp"

#include <charconv>
#include <unordered_map>
#include <unordered_set>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach_time.h>
#elif defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>
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

namespace {

// The whole token or nothing: `from_chars` alone accepts `123kB` and hands
// back a number that looks parsed.
bool parse_u64(std::string_view token, std::uint64_t& out) {
    const char* const begin = token.data();
    const char* const end = begin + token.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

}  // namespace

ProcStatFields parse_proc_stat(std::string_view text) {
    ProcStatFields fields;
    const std::size_t open = text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        return fields;
    }

    int pid = -1;
    const auto pid_result = std::from_chars(text.data(), text.data() + open, pid);
    if (pid_result.ec != std::errc{} || pid <= 0) return fields;

    // Everything after the last `)` is space-separated, starting at field 3
    // (state) in proc(5)'s 1-based numbering.
    std::vector<std::string_view> tail;
    tail.reserve(24);
    std::size_t pos = close + 1;
    while (pos < text.size()) {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n')) ++pos;
        std::size_t end = pos;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n') ++end;
        if (end > pos) tail.push_back(text.substr(pos, end - pos));
        pos = end;
    }
    constexpr std::size_t kPpid = 4 - 3;
    constexpr std::size_t kUtime = 14 - 3;
    constexpr std::size_t kStime = 15 - 3;
    constexpr std::size_t kRss = 24 - 3;
    if (tail.size() <= kRss) return fields;

    // ppid 0 is real — it is what pid 1 and the kernel threads answer.
    std::uint64_t ppid = 0;
    if (!parse_u64(tail[kPpid], ppid)) return fields;
    if (!parse_u64(tail[kUtime], fields.utime_ticks)) return fields;
    if (!parse_u64(tail[kStime], fields.stime_ticks)) return fields;
    if (!parse_u64(tail[kRss], fields.rss_pages)) return fields;

    fields.pid = pid;
    fields.ppid = static_cast<int>(ppid);
    fields.valid = true;
    return fields;
}

std::int64_t parse_smaps_rollup_pss(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view line = text.substr(pos, eol - pos);
        if (line.size() > 4 && line.substr(0, 4) == "Pss:") {
            std::size_t at = 4;
            while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
            std::int64_t kilobytes = 0;
            const auto result =
                std::from_chars(line.data() + at, line.data() + line.size(), kilobytes);
            if (result.ec != std::errc{} || result.ptr == line.data() + at) return -1;
            return kilobytes * 1024;
        }
        pos = eol + 1;
    }
    return -1;
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

#elif defined(__linux__)

namespace {

// `/proc/<pid>/stat` is under a kilobyte (comm is capped at 16 chars) and
// `smaps_rollup` around two; one buffer covers both. False when the pid is
// gone or not ours to read — the caller's skip, never an error.
bool read_proc_file(const char* path, std::string& out) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    out.clear();
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof buffer);
        if (got < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (got == 0) break;
        out.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fd);
    return true;
}

// Same reasoning as the mach conversion above: whole multiples of the tick
// rate first, the remainder scaled after, so the conversion is exact without
// overflowing — and 10^9 is not divisible by every CONFIG_HZ.
std::uint64_t ticks_to_nanos(std::uint64_t ticks) {
    static const std::uint64_t ticks_per_second = [] {
        const long value = ::sysconf(_SC_CLK_TCK);
        return value > 0 ? static_cast<std::uint64_t>(value) : 100u;
    }();
    constexpr std::uint64_t kGiga = 1'000'000'000u;
    return (ticks / ticks_per_second) * kGiga +
           (ticks % ticks_per_second) * kGiga / ticks_per_second;
}

}  // namespace

ProcessTable ProcessTable::snapshot() {
    ProcessTable table;
    DIR* const proc = ::opendir("/proc");
    if (proc == nullptr) return table;
    std::string text;
    // "/proc/" + a directory name (NAME_MAX = 255) + "/stat" + NUL.
    // GCC's -Wformat-truncation reasons from d_name's full width, not
    // from the fact that these names are short pids, so the buffer is
    // sized for the worst case it can prove rather than the one we know.
    char path[6 + 255 + 5 + 1];
    while (const dirent* entry = ::readdir(proc)) {
        // Pids are the only entries whose names start with a digit; the
        // parse validates the rest.
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        std::snprintf(path, sizeof path, "/proc/%s/stat", entry->d_name);
        // Gone between readdir and here: skipped, exactly as the header
        // promises.
        if (!read_proc_file(path, text)) continue;
        const ProcStatFields fields = parse_proc_stat(text);
        if (!fields.valid) continue;
        table.entries_.push_back(Entry{fields.pid, fields.ppid});
    }
    ::closedir(proc);
    return table;
}

ProcessSample sample_process(int pid) {
    ProcessSample sample;
    if (pid <= 0) return sample;
    char path[64];
    std::string text;
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (!read_proc_file(path, text)) return sample;
    const ProcStatFields fields = parse_proc_stat(text);
    if (!fields.valid) return sample;

    sample.cpu_time_nanos = ticks_to_nanos(fields.utime_ticks + fields.stime_ticks);
    static const std::uint64_t page_bytes = [] {
        const long value = ::sysconf(_SC_PAGESIZE);
        return value > 0 ? static_cast<std::uint64_t>(value) : 4096u;
    }();
    sample.rss_bytes = fields.rss_pages * page_bytes;

    // The one expensive read — the kernel walks page tables to answer — which
    // is a reason nobody-watching does no work, not a reason to skip it.
    // Absent on pre-4.14 kernels and unreadable for zombies: `has_real`
    // false, RSS still honest.
    std::snprintf(path, sizeof path, "/proc/%d/smaps_rollup", pid);
    if (read_proc_file(path, text)) {
        const std::int64_t pss = parse_smaps_rollup_pss(text);
        if (pss >= 0) {
            sample.real_bytes = static_cast<std::uint64_t>(pss);
            sample.has_real = true;
        }
    }
    sample.alive = true;
    return sample;
}

bool process_stats_supported() noexcept { return true; }

#else

ProcessTable ProcessTable::snapshot() { return {}; }

ProcessSample sample_process(int) { return {}; }

// Windows rides the parking lot's ConPTY entry. Until then this platform
// says so instead of returning zeros that look like measurements.
bool process_stats_supported() noexcept { return false; }

#endif

}  // namespace ckm::platform
