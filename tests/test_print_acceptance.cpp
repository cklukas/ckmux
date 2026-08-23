// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-21 §5: the printing acceptance.
//
// The row's own words: *a program printing in a ckmux terminal produces a
// saved file without ckmux ever opening a device or running a command.*
//
// Three assertions, and §5 is emphatic that they belong together: **the
// negatives are worthless alone.** An assertion that no device was opened
// passes trivially against a build where printing does nothing at all.
//
// That is not hypothetical. On 2026-08-20 this build shipped with the
// printer's whole client route unwired — nine seams assigned nowhere, the job
// messages never dispatched, the deferred save never resumed — so no reader
// could open, view or save a single captured job. A §5 rig built from the two
// negatives would have passed against it, cleanly, every run. The positive is
// the clause that carries the weight; the negatives only say the positive was
// reached honestly.
//
// And the negatives are established by OBSERVING THE PROCESS, never by
// reading the source. "We grepped for `lpr`" is a statement about the code its
// author remembered writing, not about a run.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "reader_harness.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

using ckmtest::binary_path;
using ckmtest::end_process;
using ckmtest::forget;
using ckmtest::private_socket;
using ckmtest::Reader;
using ckmtest::start_server;
using ckmtest::wait_for_socket;

// Where an instrument lives is not the same question on two systems: lsof is
// /usr/sbin/lsof on macOS and /usr/bin/lsof on most Linux distributions, and a
// hardcoded macOS path simply found nothing on Linux — descriptors_seen stayed
// 0 and this file's own positive-partner assertions failed, which is precisely
// the failure the comment above Observation warns about, arriving exactly as
// predicted. Still an absolute path, because a test that shells out should not
// be steered by an inherited PATH; but chosen from the known locations at run
// time rather than assumed. An empty answer means the tool is genuinely absent,
// and the positive-partner assertions then fail loudly instead of the two
// emptiness checks passing vacuously on a machine that could not look.
std::string tool_path(const std::string& name) {
    for (const char* directory : {"/usr/bin/", "/bin/", "/usr/sbin/", "/sbin/", "/usr/local/bin/"}) {
        std::string candidate = std::string(directory) + name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return {};
}

std::string run_and_read(const std::string& command) {
    std::string output;
    if (std::FILE* const pipe = ::popen(command.c_str(), "r")) {
        char buffer[4096];
        while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) output += buffer;
        (void)::pclose(pipe);
    }
    return output;
}

// What ckmux was holding open and what it had forked, sampled from OUTSIDE the
// process — the same way anyone would check on a running program they did not
// write.
//
// Sampled repeatedly rather than once, because a device opened and closed
// between two samples is a device this test never saw. That is the honest
// limit of sampling and the reason the positive clause has to stand beside it:
// a run that produced the right file cannot also have quietly spooled through
// a device, because the bytes came from somewhere and there is only one path
// that produced them.
struct Observation {
    std::set<std::string> device_fds;
    std::set<std::string> children;
    // What the observation ITSELF managed to see, which is the only thing that
    // makes the two emptiness checks mean anything. `lsof` returning nothing
    // and ckmux holding no printer device are indistinguishable in
    // `device_fds`; so are `pgrep` failing and ckmux forking no spooler. Both
    // negatives need a positive partner or they pass on a machine where the
    // tools are absent, which is the exact failure §5 warns about one level up.
    std::size_t descriptors_seen = 0;
    std::size_t processes_seen = 0;
};

void observe_into(Observation& seen, ::pid_t pid) {
    if (pid <= 0) return;
    const std::string lsof = tool_path("lsof");
    const std::string fds =
        lsof.empty() ? std::string()
                     : run_and_read(lsof + " -p " + std::to_string(pid) +
                                    " 2>/dev/null | awk '{print $NF}'");
    std::size_t at = 0;
    while (at < fds.size()) {
        const std::size_t end = fds.find('\n', at);
        const std::string path = fds.substr(at, end == std::string::npos ? end : end - at);
        // Every shape a printer device takes on the platforms ckmux claims,
        // plus the CUPS socket, which is how a modern one is reached without
        // opening a device node at all.
        if (!path.empty() && path != "NAME") ++seen.descriptors_seen;
        if (path.rfind("/dev/lp", 0) == 0 || path.rfind("/dev/usb/lp", 0) == 0 ||
            path.rfind("/dev/ulpt", 0) == 0 || path.rfind("/dev/printer", 0) == 0 ||
            path.find("cups") != std::string::npos)
            seen.device_fds.insert(path);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    const std::string pgrep = tool_path("pgrep");
    const std::string kids =
        pgrep.empty() ? std::string()
                      : run_and_read(pgrep + " -P " + std::to_string(pid) +
                                     " 2>/dev/null | while read p; do ps -o comm= -p $p; done");
    at = 0;
    while (at < kids.size()) {
        const std::size_t end = kids.find('\n', at);
        std::string name = kids.substr(at, end == std::string::npos ? end : end - at);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (!name.empty()) {
            ++seen.processes_seen;
            seen.children.insert(name);
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
}

bool names_a_spooler(const std::string& command) {
    // Normalised before comparing, because a process name is not the tidy word
    // it looks like. A login shell arrives as `-zsh` — argv[0] carries a
    // leading dash — and a spooler may arrive with its full path. Mutation
    // testing found this: naming the shell as a spooler did NOT fail the test,
    // which meant the check could not fire on the one child that was actually
    // there. It would still have caught a bare `lpr`, but a check that misses
    // whole shapes of name is one nobody should rely on.
    std::string leaf = command;
    const std::size_t slash = leaf.find_last_of('/');
    if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
    if (!leaf.empty() && leaf.front() == '-') leaf.erase(0, 1);
    for (const char* spooler : {"lpr", "lp", "lpq", "lpstat", "cupsd", "cups-lpd", "enscript"})
        if (leaf == spooler) return true;
    return false;
}

// A shell that is accepting input, gated on its own output rather than on
// ckmux having drawn its chrome. Unique per probe and retrying, for the
// reasons reader_harness's users keep rediscovering: a fixed sentinel matches
// its own history, and a single probe is typed into a shell that has not
// started.
int probe_counter = 0;
bool shell_is_ready(Reader& reader, int attempts = 6) {
    const std::string tag = "R5ADY" + std::to_string(++probe_counter);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        reader.press("echo " + tag.substr(0, 5) + "\"\"" + tag.substr(5) + "\r");
        if (reader.sees(tag, 2500)) return true;
    }
    return false;
}

// Clicks a label in the SAME ROW as an anchor. A bare search finds the first
// match anywhere on screen, and the Print Output dialog says "Saved as .txt in
// ~/Documents." three rows above its Save button — so a search for "Save"
// clicks a sentence.
bool click_in_row_of(Reader& reader, const char* anchor, const char* label, int offset) {
    const std::optional<std::pair<int, int>> at = reader.find_cell(anchor);
    if (!at.has_value()) return false;
    const std::vector<std::string> lines = reader.rows();
    if (static_cast<std::size_t>(at->first) >= lines.size()) return false;
    const std::string& row = lines[static_cast<std::size_t>(at->first)];
    const std::size_t byte_at = row.find(label);
    if (byte_at == std::string::npos) return false;
    int column = 0;
    for (std::size_t i = 0; i < byte_at;) {
        const unsigned char lead = static_cast<unsigned char>(row[i]);
        std::size_t width = 1;
        if ((lead & 0xE0u) == 0xC0u) width = 2;
        else if ((lead & 0xF0u) == 0xE0u) width = 3;
        else if ((lead & 0xF8u) == 0xF0u) width = 4;
        i += width;
        ++column;
    }
    reader.click(at->first, column + offset);
    reader.settle(2000);
    return true;
}

std::string read_file(const std::filesystem::path& path) {
    std::string text;
    if (std::FILE* const file = std::fopen(path.string().c_str(), "rb")) {
        char buffer[4096];
        std::size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, got);
        (void)std::fclose(file);
    }
    return text;
}

// The one file a save produced, or nothing. Named rather than globbed: §5's
// clause is that the reader gets the document WHERE THEY ASKED, and a rig that
// searched the filesystem for it would have passed against the tilde defect
// this feature shipped with — a byte-perfect file in a directory literally
// named `~`, which no reader would ever have found.
std::optional<std::filesystem::path> only_file_in(const std::filesystem::path& folder) {
    std::error_code ignored;
    if (!std::filesystem::exists(folder, ignored)) return std::nullopt;
    std::optional<std::filesystem::path> found;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(folder, ignored)) {
        if (!entry.is_regular_file(ignored)) continue;
        if (found.has_value()) return std::nullopt;  // more than one: not "the" file
        found = entry.path();
    }
    return found;
}

struct Rig {
    std::filesystem::path root;
    std::filesystem::path config;
    std::filesystem::path saves;
};

Rig make_rig(const std::string& name, const std::string& format) {
    Rig rig;
    // Per process, exactly as private_socket() does. Without the pid two runs on
    // one machine share this directory, only_file_in() finds two files where it
    // requires one, and saved.has_value() fails for a reason that has nothing to
    // do with printing — which is what made this suite unmeasurable on a shared
    // machine and sent three sessions after the wrong defect.
    rig.root = std::filesystem::temp_directory_path() /
               ("ckmux-print-" + name + "-" + std::to_string(static_cast<unsigned long>(::getpid())));
    std::error_code ignored;
    std::filesystem::remove_all(rig.root, ignored);
    std::filesystem::create_directories(rig.root, ignored);
    rig.saves = rig.root / "saves";
    std::filesystem::create_directories(rig.saves, ignored);
    rig.config = rig.root / "ckmux.conf";
    // `save-ask-name = false` so the save is one click and the test is about
    // printing rather than about a name field.
    if (std::FILE* const file = std::fopen(rig.config.string().c_str(), "wb")) {
        const std::string text = "[printer]\nmode = ask\nsave-format = " + format +
                                 "\nsave-folder = " + rig.saves.string() +
                                 "\nsave-ask-name = false\n";
        (void)std::fwrite(text.data(), 1, text.size(), file);
        (void)std::fclose(file);
    }
    return rig;
}

}  // namespace

CK_TEST(a_child_that_prints_gets_a_saved_file_and_ckmux_opens_no_device) {
    if (binary_path().empty()) return;
    const Rig rig = make_rig("txt", "txt");
    const std::filesystem::path socket = private_socket("print5");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30},
                          {{"CKMUX_CONFIG", rig.config.string()}}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    Observation seen;
    observe_into(seen, server);
    observe_into(seen, reader.client->process_id());

    // The child opens the printer, prints, and closes it. The payload carries a
    // real escape sequence so that the two formats have something to differ
    // about: `.txt` strips it, `.ansi` keeps it byte for byte.
    reader.press("printf '\\033[5iSPOOL\\033[1mBOLD\\033[0m\\033[4i'\r");
    reader.settle(2500);
    observe_into(seen, server);
    observe_into(seen, reader.client->process_id());

    // The frame button, with the ask outstanding — the reader has been told
    // something is printing and has not yet answered.
    CK_CHECK(reader.sees("PRINT?"));
    const std::optional<std::pair<int, int>> button = reader.find_cell("PRINT?");
    CK_CHECK(button.has_value());
    if (!button.has_value()) return;
    reader.click(button->first, button->second + 2);
    reader.settle(2000);

    // Answered: keep capturing for this terminal.
    CK_CHECK(reader.sees("Keep capturing"));
    (void)click_in_row_of(reader, "Keep capturing — this terminal", "Keep", 1);
    (void)click_in_row_of(reader, "Cancel", "OK", 1);
    observe_into(seen, server);
    observe_into(seen, reader.client->process_id());

    // The job list — the reader's only route to a captured document.
    CK_CHECK(reader.sees("PRINT"));
    const std::optional<std::pair<int, int>> frame = reader.find_cell("PRINT");
    CK_CHECK(frame.has_value());
    if (!frame.has_value()) return;
    reader.click(frame->first, frame->second + 2);
    reader.settle(2000);
    // Not "a window appeared": the job itself, with the byte count only the
    // real capture path could have produced.
    // "1 capture", singular and exact. Not "captures", which this test asserted
    // first time and which matches nothing — and not a bare "Print output",
    // which is the window's title and is drawn whether or not a job reached it.
    CK_CHECK(reader.sees("1 capture"));
    // The size only the real capture path could have produced: `SPOOL` plus
    // two four-byte SGR sequences plus `BOLD` is seventeen bytes on the wire.
    CK_CHECK(reader.sees("Printed by the program · 17 B"));

    CK_CHECK(click_in_row_of(reader, "Discard all", "Save", 1));
    reader.settle(4000);
    observe_into(seen, server);
    observe_into(seen, reader.client->process_id());

    // THE POSITIVE. The file exists, where the reader asked, holding what the
    // child printed with the escapes taken out.
    const std::optional<std::filesystem::path> saved = only_file_in(rig.saves);
    CK_CHECK(saved.has_value());
    if (saved.has_value()) {
        CK_CHECK(saved->extension() == ".txt");
        const std::string bytes = read_file(*saved);
        CK_CHECK(bytes == "SPOOLBOLD");
    }

    // THE NEGATIVES, and only now that the positive above has established the
    // document really was produced through the path under test.
    // The partners first: this run really did look at ckmux's descriptors and
    // really did enumerate its children. Without these two, both assertions
    // below pass on a machine with no `lsof` and no `pgrep`, having observed
    // nothing at all.
    CK_CHECK(seen.descriptors_seen > 0);
    CK_CHECK(seen.processes_seen > 0);
    // The server forks the terminal's own program, so at least one child is
    // expected and its presence is what proves the enumeration works.
    CK_CHECK(!seen.children.empty());

    for (const std::string& device : seen.device_fds) std::printf("  [device] %s\n", device.c_str());
    // Printed rather than merely asserted, so a reader of the log can see what
    // this run actually looked at — a negative is only as good as the
    // observation behind it.
    std::printf("  [WP-21 §5] observed %zu descriptors and %zu child processes across the run\n",
                seen.descriptors_seen, seen.processes_seen);

    CK_CHECK(seen.device_fds.empty());
    for (const std::string& child : seen.children)
        CK_CHECK(!names_a_spooler(child));

    reader.quit();
    end_process(server);
    forget(socket);
    std::error_code ignored;
    std::filesystem::remove_all(rig.root, ignored);
}

CK_TEST(the_ansi_format_saves_the_childs_bytes_exactly_as_sent) {
    if (binary_path().empty()) return;
    // The same run as above with one setting changed, because the pair is the
    // point: `.txt` is the document a reader can read and `.ansi` is the one
    // that can be sent back to a terminal. A rig that checked only one would
    // pass against a build that had only one formatter and used it for both.
    const Rig rig = make_rig("ansi", "ansi");
    const std::filesystem::path socket = private_socket("print5ansi");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30}, {{"CKMUX_CONFIG", rig.config.string()}}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    Observation seen;
    reader.press("printf '\\033[5iSPOOL\\033[1mBOLD\\033[0m\\033[4i'\r");
    reader.settle(2500);
    observe_into(seen, server);
    observe_into(seen, reader.client->process_id());

    CK_CHECK(reader.sees("PRINT?"));
    const std::optional<std::pair<int, int>> button = reader.find_cell("PRINT?");
    CK_CHECK(button.has_value());
    if (!button.has_value()) return;
    reader.click(button->first, button->second + 2);
    reader.settle(2000);
    (void)click_in_row_of(reader, "Keep capturing — this terminal", "Keep", 1);
    (void)click_in_row_of(reader, "Cancel", "OK", 1);

    const std::optional<std::pair<int, int>> frame = reader.find_cell("PRINT");
    CK_CHECK(frame.has_value());
    if (!frame.has_value()) return;
    reader.click(frame->first, frame->second + 2);
    reader.settle(2000);
    CK_CHECK(reader.sees("1 capture"));
    CK_CHECK(click_in_row_of(reader, "Discard all", "Save", 1));
    reader.settle(4000);
    observe_into(seen, server);
    observe_into(seen, reader.client->process_id());

    const std::optional<std::filesystem::path> saved = only_file_in(rig.saves);
    CK_CHECK(saved.has_value());
    if (saved.has_value()) {
        CK_CHECK(saved->extension() == ".ansi");
        // Byte-identical to what the child sent between CSI 5 i and CSI 4 i —
        // escapes and all. Compared against a literal rather than against
        // whatever the formatter produced, so a formatter that changed its mind
        // fails here instead of agreeing with itself.
        CK_CHECK(read_file(*saved) == std::string("SPOOL\x1b[1mBOLD\x1b[0m"));
    }
    CK_CHECK(seen.descriptors_seen > 0);
    CK_CHECK(seen.processes_seen > 0);
    CK_CHECK(seen.device_fds.empty());
    for (const std::string& child : seen.children) CK_CHECK(!names_a_spooler(child));

    reader.quit();
    end_process(server);
    forget(socket);
    std::error_code ignored;
    std::filesystem::remove_all(rig.root, ignored);
}

CK_TEST(a_printer_left_open_costs_the_ask_cache_and_spawns_nothing) {
    if (binary_path().empty()) return;
    // §5's case that must not be forgotten, because it is where a printer
    // implementation historically starts running commands: a child that opens
    // `CSI 5 i` and never closes it. It must cost the ask cache and not one
    // byte more, the screen must resume on `CSI 4 i`, and nothing may be
    // spawned to "flush" anything.
    const Rig rig = make_rig("open", "txt");
    const std::filesystem::path socket = private_socket("print5open");
    forget(socket);
    const ::pid_t server = start_server(socket);
    CK_CHECK(wait_for_socket(socket));

    Reader reader;
    CK_CHECK(reader.start(socket, ckv::Size{100, 30}, {{"CKMUX_CONFIG", rig.config.string()}}));
    CK_CHECK(reader.sees("new term"));
    CK_CHECK(shell_is_ready(reader));

    Observation seen;
    // Opened and left open. Everything the shell writes from here goes to the
    // printer rather than to the screen, which is what the control means.
    reader.press("printf '\\033[5i'\r");
    reader.settle(1500);
    CK_CHECK(reader.sees("PRINT?"));
    observe_into(seen, server);
    observe_into(seen, reader.client->process_id());

    // Keep feeding it, without ever closing.
    for (int round = 0; round < 3; ++round) {
        reader.press("printf 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'\r");
        reader.settle(900);
        observe_into(seen, server);
        observe_into(seen, reader.client->process_id());
    }
    // Nothing was spawned to deal with any of it, and no device was opened —
    // the two things this case exists to check.
    CK_CHECK(seen.descriptors_seen > 0);
    CK_CHECK(seen.processes_seen > 0);
    CK_CHECK(seen.device_fds.empty());
    for (const std::string& child : seen.children) CK_CHECK(!names_a_spooler(child));

    // And the screen comes back when the child finally closes the printer.
    reader.press("printf '\\033[4i'\r");
    reader.settle(1500);
    CK_CHECK(shell_is_ready(reader));

    reader.quit();
    end_process(server);
    forget(socket);
    std::error_code ignored;
    std::filesystem::remove_all(rig.root, ignored);
}
