// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "server/terminals.hpp"

#include <algorithm>
#include <utility>

#include "common/shell.hpp"

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
    // `sixel = auto` means "advertise what the host can show" (plans/04,
    // plans/08): the setting opens the door, the opening client's own host
    // capability decides whether anything is behind it.
    launch.profile.sixel = settings.sixel == SixelMode::Auto && spec.host_sixel;
    launch.profile.clipboard_policy = settings.osc52 ? ckv::core::TerminalClipboardPolicy::AllowWrite
                                                     : ckv::core::TerminalClipboardPolicy::Deny;
    launch.profile.printer_policy = settings.printer_mode == PrinterMode::Off
                                        ? ckv::core::TerminalPrinterPolicy::Deny
                                        : ckv::core::TerminalPrinterPolicy::Capture;
    // The child is terminated after a grace period rather than waited for
    // indefinitely: a server that hangs on one program that ignores SIGTERM
    // has stopped being a server (plans/03).
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
      pixel_height_(std::max(0, spec.pixel_height)) {}

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
    // and one closes (plans/03 on-exit) — so a server that could not tell them
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
