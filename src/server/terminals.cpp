// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "server/terminals.hpp"

#include <algorithm>
#include <utility>

#include "common/shell.hpp"
#include "server/diff_engine.hpp"

namespace ckm::server {
namespace {

// A terminal's own environment. ckmux states TERM and the two variables that
// say what a child is running under, and inherits everything else — HOME,
// PATH, LANG, the reader's own settings — because a terminal is a place to run
// the programs one already has, and a child given only what a host thought to
// list is on a machine its owner did not configure.
std::vector<std::pair<std::string, std::string>> child_environment(const Settings& settings) {
    std::vector<std::pair<std::string, std::string>> environment{
        {"TERM", settings.term == "auto" ? std::string("xterm-256color") : settings.term},
        {"COLORTERM", "truecolor"},
        {"TERM_PROGRAM", "ckmux"},
    };
    return environment;
}

}  // namespace

ckv::term::TerminalSubsessionOptions subsession_options_for(const Settings& settings) {
    ckv::term::TerminalSubsessionOptions options;
    // The history is the server's, and its size is the reader's choice —
    // including zero, which really means "remember nothing" (U0-c).
    options.max_scrollback_lines = static_cast<std::size_t>(std::max(0, settings.scrollback));
    options.max_image_pixels =
        static_cast<std::size_t>(std::max(1, settings.sixel_max_megapixels)) * 1024U * 1024U;
    options.max_printer_spool_bytes = settings.printer_spool_limit_bytes;
    // ckVision's own defaults here (64 KiB queued, 32 KiB parsed per step)
    // are sized for an untrusted child inside an embedded widget. A ckmux
    // terminal is neither: it is a full, trusted, interactive program, and
    // max_graphics_payload_bytes already allows a picture up to 4 MiB — but
    // a real one, well under that (a downscaled dialog logo measured at
    // ~490 KiB of Sixel), still had to cross this queue in a dozen-plus
    // rounds at the old default, each one a full pass through this
    // terminal's turn in the server loop. Raised to what
    // ckvision/benchmarks/bench_terminal.cpp already calls the realistic
    // figure for actual application output, not the conservative one for
    // input nobody has vetted.
    options.max_output_bytes = 1U << 20U;          // 1 MiB
    options.max_parser_work_per_step = 128U << 10U;  // 128 KiB
    return options;
}

ckv::term::TerminalLaunchSpec launch_spec_for(const Settings& settings, const TerminalSpec& spec) {
    // A command the client asked for runs through the reader's shell, so that
    // `ckmux new 'make -j8 && say done'` means what it looks like. Without a
    // command it is a login shell, resolved the way tmux resolves one.
    ShellLaunch shell = shell_launch(settings.shell, settings.login_shell);
    if (!spec.command.empty()) {
        // `-c <command>` rather than a login shell: a client that asked for
        // `make -j8 && say done` means the whole thing, operators included, so
        // it goes through a shell rather than being split into argv here.
        shell.argv0.clear();
        shell.arguments = {"-c", spec.command};
    }

    ckv::term::TerminalLaunchSpec launch =
        ckv::term::TerminalLaunchSpec::program(shell.executable, shell.arguments);
    launch.argv0 = shell.argv0;
    launch.working_directory = spec.working_directory.empty() ? std::string("/") : spec.working_directory;
    launch.environment = spec.environment.empty() ? child_environment(settings) : spec.environment;

    launch.profile = ckv::term::embedded_xterm_sixel_profile();
    launch.profile.cells = ckv::Size{std::max(1, spec.columns), std::max(1, spec.rows)};
    // The same division as `Terminal::cell_pixels()`: the spec carries the
    // text area, the profile wants one cell.
    if (spec.pixel_width > 0 && spec.pixel_height > 0)
        launch.profile.cell_pixels = ckv::Size{spec.pixel_width / std::max(1, spec.columns),
                                              spec.pixel_height / std::max(1, spec.rows)};
    // What this terminal will tell its child it can do. The server owns the
    // emulator, so the server owns these — a client cannot be trusted with
    // them, and a program asks a terminal what it supports and then behaves as
    // though the answer were true.
    launch.profile.osc_policy = ckv::core::TerminalOscPolicy::StoreMetadata;
    launch.profile.mouse_reporting = settings.mouse;
    launch.profile.alternate_scroll = settings.alternate_scroll;
    // `sixel = auto` means "advertise what the host can show" (the terminal-emulation spec,
    // The configuration spec): the setting opens the door, the opening client's own host
    // capability decides whether anything is behind it.
    launch.profile.sixel = settings.sixel == SixelMode::Auto && spec.host_sixel;
    launch.profile.clipboard_policy = settings.osc52 ? ckv::core::TerminalClipboardPolicy::AllowWrite
                                                     : ckv::core::TerminalClipboardPolicy::Deny;
    launch.profile.printer_policy = settings.printer_mode == PrinterMode::Off
                                        ? ckv::core::TerminalPrinterPolicy::Deny
                                        : ckv::core::TerminalPrinterPolicy::Capture;
    // The child is terminated after a grace period rather than waited for
    // indefinitely: a server that hangs on one program that ignores SIGTERM
    // has stopped being a server (the session model).
    launch.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
    return launch;
}

Terminal::Terminal(TerminalId id, std::unique_ptr<ckv::term::PosixTerminalSubsession> session,
                   const TerminalSpec& spec)
    : id_(id),
      session_(std::move(session)),
      columns_(std::max(1, spec.columns)),
      rows_(std::max(1, spec.rows)),
      pixel_width_(std::max(0, spec.pixel_width)),
      pixel_height_(std::max(0, spec.pixel_height)),
      spec_(spec) {}

TerminalSpec Terminal::respawn_spec() const {
    TerminalSpec again = spec_;
    again.columns = columns_;
    again.rows = rows_;
    again.pixel_width = pixel_width_;
    again.pixel_height = pixel_height_;
    return again;
}

void Terminal::relaunch(std::unique_ptr<ckv::term::PosixTerminalSubsession> session) noexcept {
    session_ = std::move(session);
    // Everything the exited child left behind. `exit_announced_` especially:
    // left true, the new child's own exit would never be announced, and the
    // reader would sit in front of a window that had quietly stopped saying
    // anything about the program in it.
    exit_status_.reset();
    exit_announced_ = false;
    closed_ = false;
    // The screen is new, so whatever the client believes about this terminal
    // is now wrong in every cell. Saying so is the caller's job — `Server`
    // drops the differ — but the marks are ours: a bell from the dead child is
    // not a bell from this one.
    bell_marked_ = false;
    activity_marked_ = false;
    marks_announced_ = true;
}

void Terminal::resize(int columns, int rows, int pixel_width, int pixel_height) {
    // Whatever the emulator reports as changed after this is the resize, not
    // the child. Set even when the size turns out to be the one it already had:
    // the ONE caller is a client asking, so this says "the screen is about to
    // change because a reader moved a window".
    resized_by_a_client_ = true;
    columns_ = std::max(1, columns);
    rows_ = std::max(1, rows);
    // Zero pixels is "I do not know", and the last metric that made sense is
    // kept rather than overwritten with nothing — a child told its terminal is
    // a few dozen pixels across will never send a picture again.
    if (pixel_width > 0 && pixel_height > 0) {
        pixel_width_ = pixel_width;
        pixel_height_ = pixel_height;
    }
    session_->resize(ckv::Size{columns_, rows_}, cell_pixels());
}

ckv::Size Terminal::cell_pixels() const noexcept {
    // A cell is the text area divided by the grid. ckVision multiplies back up
    // for TIOCSWINSZ and for the child's XTWINOPS answers, so handing it the
    // window size here would tell a child its terminal is columns-times too
    // wide — and every picture it drew would be sized from that.
    //
    // A division that floors to zero is a text area smaller than its own grid,
    // which is not a measurement; it reads as "unknown", which is what zero
    // means to ckVision too.
    if (pixel_width_ <= 0 || pixel_height_ <= 0) return ckv::Size{0, 0};
    return ckv::Size{pixel_width_ / std::max(1, columns_), pixel_height_ / std::max(1, rows_)};
}

bool Terminal::live() const noexcept {
    using State = ckv::core::TerminalSubsessionState;
    const State state = session_->state();
    // Ready counts as live: a session becomes Running only once its child has
    // produced output, so a program that has printed nothing yet — a shell at
    // a prompt it has not drawn, anything reading stdin — is Ready and very
    // much alive.
    return state == State::Ready || state == State::Running;
}

void Terminal::observe_exit() {
    if (exit_status_.has_value()) return;
    using State = ckv::core::TerminalSubsessionState;
    if (session_->state() != State::Exited) return;
    // Whatever the child actually exited with, not a stand-in. "exited 1" and
    // "exited 0" are different windows to a reader — one holds with a banner
    // and one closes (the session model on-exit) — so a server that could not tell them
    // apart would have to guess, and would guess wrong half the time.
    exit_status_ = session_->exit_code();
}

void Terminal::mark_bell() noexcept {
    if (bell_marked_) return;
    bell_marked_ = true;
    marks_announced_ = false;
}

void Terminal::mark_activity() noexcept {
    if (activity_marked_) return;
    activity_marked_ = true;
    marks_announced_ = false;
}

bool Terminal::set_layout(const WindowLayout& layout) noexcept {
    // Stored exactly as reported, and in particular NOT clamped the way
    // `Terminal::resize` clamps a grid. Nothing here is sized from it: a window
    // rect is a place on a reader's desktop, and one that hangs off an edge is a
    // real arrangement somebody made rather than a number this server has to
    // make safe. What to do about it when the desktop is a different size next
    // time is a restoration policy, and this package deliberately has none
    // (WP-28 stores and states; WP-30 decides).
    if (layout_ == layout) return false;
    layout_ = layout;
    layout_announced_ = false;
    return true;
}

bool Terminal::set_custom_title(std::string title) {
    // Clamped here rather than at the two places that send it, because this is
    // where the name is KEPT: a server that stored four megabytes of caption
    // and trimmed it on the way out would still be holding four megabytes per
    // terminal for the life of the session. Through the same bound AND the
    // same function the child's own title is clamped by, so the two producers
    // cannot disagree by a byte — and so the cut lands on a character boundary
    // rather than in the middle of a UTF-8 sequence, which is what a plain
    // resize() would do to a caption with an umlaut near the limit.
    const std::string_view kept = clamp_utf8(title, proto::kMaxTitleBytes);
    if (kept.size() != title.size()) title.resize(kept.size());
    if (custom_title_ == title) return false;
    custom_title_ = std::move(title);
    custom_title_announced_ = false;
    return true;
}

namespace {

// The wire's word for what produced a job. Stated here rather than derived by
// a cast, because the two enums are separate on purpose: ckVision's is what
// the emulator means and this is what the protocol carries, and a cast would
// make the wire follow a change in the library's enum silently (the same rule
// `proto::Rect` exists for).
proto::PrintJobKind wire_job_kind(ckv::core::TerminalPrinterJob::Origin origin) {
    switch (origin) {
        case ckv::core::TerminalPrinterJob::Origin::Screen: return proto::PrintJobKind::Screen;
        case ckv::core::TerminalPrinterJob::Origin::Line: return proto::PrintJobKind::Line;
        case ckv::core::TerminalPrinterJob::Origin::Controller:
            return proto::PrintJobKind::Controller;
        case ckv::core::TerminalPrinterJob::Origin::Autoprint:
            return proto::PrintJobKind::Autoprint;
    }
    return proto::PrintJobKind::Controller;
}

// Lines as a reader would count them: the number of newline-separated pieces,
// so a job with no trailing newline still counts its last line. Empty text is
// zero rather than one — an overflowed job printed nothing this server can
// show, and claiming "1 line" for it would be the same lie as showing a
// truncated one.
std::uint32_t count_lines(std::string_view text) {
    if (text.empty()) return 0;
    std::uint32_t lines = 1;
    for (const char c : text)
        if (c == '\n') ++lines;
    // A trailing newline ends the last line rather than starting another.
    if (text.back() == '\n') --lines;
    return lines;
}

}  // namespace

std::vector<std::uint64_t> Terminal::collect_print_jobs(std::int64_t now) {
    std::vector<std::uint64_t> added;
    for (ckv::core::TerminalPrinterJob& job : session_->take_printer_jobs()) {
        HeldJob held;
        held.id = next_job_id_++;
        held.kind = wire_job_kind(job.origin);
        held.lines = count_lines(job.text);
        held.at = now;
        held.overflowed = job.overflowed;
        held.text = std::move(job.text);
        added.push_back(held.id);
        jobs_.push_back(std::move(held));
    }
    return added;
}

const Terminal::HeldJob* Terminal::print_job(std::uint64_t id) const noexcept {
    for (const HeldJob& job : jobs_)
        if (job.id == id) return &job;
    return nullptr;
}

bool Terminal::discard_print_jobs(std::uint64_t id) noexcept {
    if (id == 0) {
        if (jobs_.empty()) return false;
        jobs_.clear();
        return true;
    }
    const auto found = std::find_if(jobs_.begin(), jobs_.end(),
                                    [id](const HeldJob& job) { return job.id == id; });
    if (found == jobs_.end()) return false;
    jobs_.erase(found);
    return true;
}

void Terminal::set_printer_policy(ckv::term::TerminalPrinterPolicy policy) {
    session_->set_printer_policy(policy);
}

void Terminal::set_printer_spool_limit(std::size_t bytes) {
    session_->set_printer_spool_limit(bytes);
}

bool Terminal::take_client_resize() noexcept {
    const bool resized = resized_by_a_client_;
    resized_by_a_client_ = false;
    return resized;
}

void Terminal::clear_marks() noexcept {
    if (!bell_marked_ && !activity_marked_) return;
    bell_marked_ = false;
    activity_marked_ = false;
    // A mark going out is news exactly as a mark coming on is: the window
    // marker a reader is looking at has to stop saying "something happened
    // here" once they have been here.
    marks_announced_ = false;
}

void Terminal::flush_replies_to_child() {
    std::string replies = session_->take_pending_input();
    if (replies.empty()) return;
    // The emulator answers its child's queries — DA1, DSR, XTWINOPS — and
    // those answers are for the child, not for a client. A server that sent
    // them onward would leave every probing program waiting forever for a
    // reply that went to the wrong end of the socket.
    session_->send_input(replies);
}

void Terminal::close() noexcept {
    if (closed_) return;
    closed_ = true;
    session_->close();
}

Terminals::Terminals(Settings settings) : settings_(std::move(settings)) {}

Terminals::~Terminals() { close_all(); }

bool Terminals::respawn(TerminalId id) {
    Terminal* const terminal = find(id);
    if (terminal == nullptr) return false;
    // A living child is refused rather than replaced. Relaunching over one
    // would drop the last reference to its subsession, and with it the only
    // handle anything has on that process group — a program still running with
    // nobody able to reach it, which is the opposite of what this server is for.
    if (terminal->live()) return false;

    // The spec it was launched from, with the size it has NOW: a reader who
    // resized the window before pressing Enter meant the new child to fill it,
    // and the original spec remembers the size the old one started at.
    const TerminalSpec again = terminal->respawn_spec();
    terminal->relaunch(ckv::term::PosixTerminalSubsession::launch(
        launch_spec_for(settings_, again), subsession_options_for(settings_)));
    return true;
}

Terminal& Terminals::open(const TerminalSpec& spec) {
    std::unique_ptr<ckv::term::PosixTerminalSubsession> session =
        ckv::term::PosixTerminalSubsession::launch(launch_spec_for(settings_, spec),
                                                   subsession_options_for(settings_));
    // ckVision returns a session in Failed state rather than nothing when a
    // launch fails, which is what lets a client show the failure in the window
    // it had already opened.
    terminals_.push_back(std::make_unique<Terminal>(next_id_++, std::move(session), spec));
    return *terminals_.back();
}

Terminal* Terminals::find(TerminalId id) noexcept {
    for (const std::unique_ptr<Terminal>& terminal : terminals_)
        if (terminal->id() == id) return terminal.get();
    return nullptr;
}

const Terminal* Terminals::find(TerminalId id) const noexcept {
    for (const std::unique_ptr<Terminal>& terminal : terminals_)
        if (terminal->id() == id) return terminal.get();
    return nullptr;
}

std::vector<TerminalId> Terminals::ids() const {
    std::vector<TerminalId> result;
    result.reserve(terminals_.size());
    for (const std::unique_ptr<Terminal>& terminal : terminals_) result.push_back(terminal->id());
    return result;
}

std::size_t Terminals::drain(std::size_t byte_budget_for_the_pass) {
    std::size_t worked = 0;
    const std::size_t count = terminals_.size();
    if (count == 0) return 0;
    // Shared out, with a floor. What a drain costs is parsing, and parsing is
    // the whole of a loop's pass: giving every terminal the full budget means a
    // pass that grows with the number of busy children until nothing else in the
    // loop gets a turn.
    const std::size_t share = std::max(kMinimumDrainShare, byte_budget_for_the_pass / count);
    // By index, because a drain reads child output and nothing here may assume
    // the collection is unchanged afterwards — a future exit handler that
    // closes a terminal must not invalidate this walk. Starting where the last
    // pass stopped is what keeps the first terminal from being the only one
    // served when a budget runs short.
    const std::size_t start = count == 0 ? 0 : drain_cursor_ % count;
    drain_cursor_ = (start + 1) % count;
    for (std::size_t step = 0; step < count; ++step) {
        const std::size_t index = (start + step) % count;
        if (index >= terminals_.size()) break;
        Terminal* const terminal = terminals_[index].get();
        if (terminal == nullptr) continue;
        if (terminal->drain(share)) ++worked;
        // A child's exit is observed by the same read that found the end of
        // its output, so this is the moment the status exists.
        terminal->observe_exit();
        // Whatever the emulator owes its child goes back down the PTY now,
        // while the terminal is in hand.
        terminal->flush_replies_to_child();
    }
    return worked;
}

std::vector<ckv::term::WaitHandle> Terminals::wait_handles() const {
    std::vector<ckv::term::WaitHandle> handles;
    handles.reserve(terminals_.size());
    for (const std::unique_ptr<Terminal>& terminal : terminals_) {
        const std::span<const ckv::term::WaitHandle> own = terminal->wait_handles();
        handles.insert(handles.end(), own.begin(), own.end());
    }
    return handles;
}

bool Terminals::close(TerminalId id) {
    for (std::size_t index = 0; index < terminals_.size(); ++index) {
        if (terminals_[index]->id() != id) continue;
        terminals_[index]->close();
        terminals_.erase(terminals_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }
    return false;
}

void Terminals::close_all() {
    // Asked all at once, then closed one at a time. The waiting inside `close()`
    // is per terminal, so a server shutting down with ten terminals used to pay
    // ten graces one after another; asking first means the graces run down
    // together and the last child is not still being asked when the first one's
    // has already expired.
    for (const std::unique_ptr<Terminal>& terminal : terminals_) terminal->request_termination();
    // Closed before the vector is emptied, and in open order, so that every
    // child has had SIGHUP→SIGTERM→SIGKILL applied to its process group before
    // this function returns. A destructor that merely released the sessions
    // would leave the children to be inherited by init, still running, with
    // nobody's terminal to write to.
    for (const std::unique_ptr<Terminal>& terminal : terminals_) terminal->close();
    terminals_.clear();
}

}  // namespace ckm::server
