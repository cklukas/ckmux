// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The ckmux client: menu bar, desktop of terminal windows, and the permanent
// context-sensitive footer (plans/06-ui-design.md).
//
// M1 scope (plans/10-roadmap.md): the terminals are launched in this process
// and die with it. Every seam that M2 needs in order to move them behind the
// detached server — how a terminal is created, how it is titled, how the
// footer and prefix reach it — is already routed through this class, so the
// split changes what a terminal IS without changing what the UI does with it.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "client/commands.hpp"
#include "client/copy_mode.hpp"
#include "client/prefix_overlay.hpp"
#include "common/config.hpp"
#include "cvision/term/terminal_subsession.hpp"
#include "cvision/ui/application.hpp"
#include "cvision/ui/standard_roles.hpp"
#include "cvision/widgets/common_components.hpp"
#include "cvision/widgets/desktop.hpp"
#include "cvision/widgets/help_viewer.hpp"
#include "cvision/widgets/status_line.hpp"
#include "cvision/widgets/window.hpp"

namespace ckm::client {

// The reader's local date and time, read together. One reading rather than
// two, so the clock on the menu bar and the calendar under it can never
// disagree about which day it is.
struct LocalMoment {
    ckv::widgets::DateValue date;
    ckv::widgets::TimeValue time;
};

// What ckmux would run, handed to whatever provides the terminal.
//
// The launch spec is composed from the reader's configuration whether or not
// anything forks: it carries the command, the working directory, the environment
// and — the part a remote source also needs — the capability profile, which is
// what decides the cell metric a picture is drawn at and what the child is told
// its terminal can do.
struct TerminalRequest {
    ckv::term::TerminalLaunchSpec launch;
    ckv::term::TerminalSubsessionOptions options;
    std::string title;
};

// One session, as a reader is shown it.
struct SessionRow {
    std::uint64_t id = 0;
    std::string name;
    int terminals = 0;
    // Whether somebody is watching it — which for the reader looking at this
    // list is themselves, and worth saying plainly.
    bool attached = false;
};

struct ClientOptions {
    // Everything the configuration file said, defaults included (plans/08).
    // The client reads its own settings out of this rather than holding a
    // second copy of each one, so a key added to the file is a key the client
    // can honour without a parallel field appearing here.
    //
    // What a new terminal runs lives in `settings.shell`: explicit rather
    // than read from the ambient environment, because ckVision requires a
    // launch spec to name its program and environment outright and a test
    // needs the same run to be reproducible. Empty asks for resolve_shell(),
    // which is what the real host does.
    Settings settings;
    // Whatever load_settings() had to say about the file this session's
    // settings came from. Carried rather than printed at the point of
    // reading: by then the terminal belongs to the UI, and a line written
    // behind the alternate screen is a line nobody sees. Empty is the
    // ordinary case (plans/08 — unknown keys warn, never abort).
    std::vector<std::string> config_warnings;
    std::vector<std::pair<std::string, std::string>> environment;
    // Where a new terminal starts. Empty leaves the launch spec's own
    // default; the client fills it with the reader's home directory, since
    // that is where a shell opens.
    std::string working_directory;
    // How a copy reaches a helper program — `pbcopy`, `xclip`, whatever
    // `[terminal] clipboard` named. Injected because the real one forks, and
    // a client that forks is a client a test cannot drive: the host passes
    // platform::write_to_command, a test passes a recorder. Empty means no
    // helper target is attempted at all.
    std::function<bool(const std::string& command, std::string_view text)> clipboard_writer;
    // What time it is where the reader is — the one wall-clock reading ckmux
    // takes, and injected for the same reason the clipboard writer is: reading
    // the host clock and its time zone is an application's job, never the
    // library's, and ckVision's own `Clock` is monotonic on purpose (AGENTS.md
    // determinism rule). The host passes the system reading, a test passes a
    // fixed moment. Empty means no clock at all: one that has to guess the
    // time is worse than none.
    std::function<LocalMoment()> local_now;
    // Where a terminal comes from.
    //
    // Unset launches one in this process, which is M1 and what a reader gets
    // today. A client attached to a server passes a source that adopts a
    // `RemoteTerminalSubsession` instead (WP-5) — and that substitution is the
    // ONLY difference between the two worlds. The window and its frame, the
    // close confirmation, the exited banner, the footer, the prefix and its
    // which-key popup, copy mode: all of it is the same code reading the same
    // seam, which is the whole reason M1 was built against
    // `core::TerminalSubsession` rather than against a PTY.
    //
    // Returns a reference because ownership belongs to the `Application` either
    // way — launched or adopted (ckVision U0-a) — so the lifetime rule "the
    // session outlives every view that borrows it" is stated in one place.
    //
    // The reference must outlive every view that borrows it, which is the same
    // rule ckVision states for a launched or adopted session. Where that
    // lifetime lives is the source's business: M1 hands it to the
    // `Application`, and an attached client keeps its mirrors in its own
    // session object because a mirror has no descriptors for the application to
    // wait on and nothing for it to drain.
    std::function<ckv::term::TerminalSubsession&(TerminalRequest)> terminal_source;
    // What "New Terminal" does.
    //
    // Unset opens one here and now, which is M1. A client attached to a server
    // ASKS for one and opens the window when the server says the terminal
    // exists — because a window with nothing behind it is a window that lies,
    // and because whether the terminal could be created at all is the server's
    // answer to give. Returning false means the request could not be sent, and
    // the client falls back to saying so rather than to opening one locally.
    std::function<bool()> request_new_terminal;
    // What Detach does, and what Quit does when there is a server to detach
    // FROM.
    //
    // A multiplexer's client quitting is not an application closing its
    // documents: the programs go on running, which is the entire point, so
    // quitting must not ask a reader whether to kill anything. Unset is the M1
    // world — no server, nothing to detach from, and quit means quit.
    std::function<void()> detach_from_server;
    // Asks the server what sessions exist. The answer arrives later, through
    // `ClientApp::show_session_list`: sessions come and go without this
    // client's involvement, so "what is running?" is a question with an answer
    // rather than a value a client can hold.
    std::function<void()> list_sessions;
    // Attaches to one — taking it over if somebody else has it, which is
    // granted immediately and never negotiated (plans/03).
    std::function<void(std::uint64_t session)> attach_to_session;
    // Creates one under a name the reader chose, and attaches to it.
    std::function<void(const std::string& name)> create_session;
    // Renames the session this client is attached to.
    std::function<void(const std::string& name)> rename_session;
    // Ends the session this client is watching: every program in it is asked to
    // quit, and — only if the reader says so — anything still running when the
    // grace period runs out is killed.
    std::function<void(bool force, int grace_seconds)> kill_session;
    // Whether to open a terminal at startup.
    //
    // True is M1: a client with no server has nothing to attach to, so an empty
    // desktop would be an empty program. An attached client says false and
    // decides after the snapshot arrives — because the session it joins usually
    // ALREADY has terminals, and opening one anyway means a reader gains a
    // stray shell every time they restart. That is not hypothetical: it is what
    // ckmux did until a reader restarted it twice.
    bool open_terminal_at_startup = true;
    // How long the prefix stays silent before the which-key popup appears.
    std::int64_t which_key_delay_nanos = 500'000'000;
    // How often window captions are re-read from the children. The emulator
    // records a title change without raising an event, so this is a poll; the
    // interval only has to be shorter than a reader would notice.
    std::int64_t title_poll_nanos = 100'000'000;
};

// The default child environment: TERM and friends per
// plans/04-terminal-emulation.md.
std::vector<std::pair<std::string, std::string>> default_environment();

class ClientApp {
public:
    explicit ClientApp(ckv::ui::Application& app, ClientOptions options = {});
    ~ClientApp();

    ClientApp(const ClientApp&) = delete;
    ClientApp& operator=(const ClientApp&) = delete;

    ckv::widgets::Desktop& desktop() noexcept { return *desktop_; }
    ckv::widgets::StatusLine& footer() noexcept { return *footer_; }

    // Opens a terminal window running the configured shell and focuses it.
    ckv::widgets::Window* new_terminal();
    // Opens a window for a terminal that already exists — what an attached
    // client does when the server announces one, including every terminal that
    // was already running when it attached.
    ckv::widgets::Window* open_terminal(std::string title);
    // Shows what the server answered. Called by the host when the list arrives;
    // `rows` is in the server's order, and the attached one is marked.
    // Which session this client is watching, as the server last said it. Zero
    // is a client with none — one started without a session, or one whose
    // session was ended under it — and every command that needs a session is
    // disabled for exactly as long as it stays zero.
    void set_attached_session(std::uint64_t id, std::string name);
    std::uint64_t attached_session() const noexcept { return attached_session_; }
    // Takes down every terminal window and leaves what is behind them running.
    // This is what LEAVING a session means: the programs in it carry on, and
    // this client simply stops showing them. Not `close_terminal`, which ends
    // them — a reader switching sessions asked for nothing of the kind.
    void forget_terminals();
    // The window showing this terminal is gone because the terminal is: the
    // server has said so, so nothing is asked of it on the way out.
    void close_window_for_terminal(const ckv::term::TerminalSubsession& terminal);
    // The sessions the server last named, kept without showing anything. What
    // the prompts read for names, and what the New Session suggestion counts.
    void remember_sessions(std::vector<SessionRow> rows);
    // The name this client last heard for a session, or empty. From the last
    // list rather than held per session: names change, and the list is the
    // only thing that says so.
    std::string session_name(std::uint64_t id) const;

    // The picker: what is running, and what a reader may do about it. Shown at
    // startup when the choice is not obvious — when the only session is already
    // being watched somewhere else, or when there is more than one — and by
    // `Session ▸ Sessions…` at any time.
    void show_session_picker(std::vector<SessionRow> rows);
    // The name prompt, pre-filled with the server's suggestion.
    void show_new_session_dialog(std::string suggested_name);
    void show_rename_session_dialog(std::string current_name);
    // Ending a session, with the one decision only a reader can make in front
    // of them: whether a program that ignores the asking is killed.
    void show_kill_session_dialog();

    // --- State the UI derives from, exposed so tests can assert on the
    // same values the rendering does rather than scraping cells ----------

    // Which hint set the footer is showing, derived from focus.
    Context context() const;
    bool prefix_pending() const noexcept { return overlay_ != nullptr; }
    PrefixOverlay* prefix_overlay() noexcept { return overlay_; }
    // The copy-mode surface over the focused terminal, or nullptr. Copy mode
    // is client-local by design (plans/03: dialogs, the picker and copy mode
    // are never synced), which is also why it needs nothing from the server.
    CopyModeView* copy_mode() noexcept { return copy_mode_; }
    // The clock at the right end of the menu bar, or nullptr when `[general]
    // clock` is off or nothing can tell it the time.
    ckv::widgets::ClockView* clock() noexcept { return clock_; }
    // What `^B ]` would paste: text yanked in copy mode, held here so it
    // survives a copy that no system clipboard accepted.
    const std::string& internal_clipboard() const noexcept { return internal_clipboard_; }
    const ckv::KeyChord& prefix() const noexcept { return options_.settings.prefix; }
    // The bindings in force, a reader's `bind` lines applied. Every surface
    // that mentions a key reads this one table.
    const Keymap& keymap() const noexcept { return keymap_; }
    // The footer's rendered item labels, in order.
    std::vector<std::string> footer_labels() const;

    // Arms the prefix exactly as pressing the prefix key in a focused
    // terminal does. The terminal view reaches this through its
    // parent-escape callback; a test reaches it directly.
    void arm_prefix();
    // Resolves an armed prefix with `chord` ("c", "1", "" to cancel).
    void resolve_prefix(const std::string& chord);

private:
    void register_commands();
    void build_chrome();
    ckv::widgets::MenuBar* menu_bar();
    // Makes the menu bar's right end match `[general] clock`: a clock with or
    // without seconds, or nothing there at all. Called at startup and again
    // whenever the setting changes, so it states the whole answer rather than
    // adding to whatever is already there.
    void install_clock();
    void refresh_footer();
    void close_terminal(ckv::widgets::Window* window);
    void confirm_then_close(ckv::widgets::Window* window);
    void focus_active_terminal();
    ckv::widgets::Window* active_terminal() const;
    // Applies whatever titles the child programs have asked for since the
    // last frame. Polled rather than pushed: the emulator records the title
    // in its snapshot and raises no event, and a frame is exactly as often as
    // a caption can visibly change.
    void refresh_terminal_titles();
    void show_key_reference();
    // The name the New Session prompt offers, from the sessions last seen.
    std::string suggested_session_name() const;
    std::vector<SessionRow> last_sessions_;
    // Which window is showing which terminal, so that a terminal the server
    // has ended takes its window with it.
    std::map<const ckv::term::TerminalSubsession*, ckv::widgets::Window*> terminal_windows_;
    // Set while windows are being taken down for a session this client is
    // leaving, so the close path knows not to ask the server to end anything.
    bool forgetting_terminals_ = false;
    std::uint64_t attached_session_ = 0;
    std::string attached_session_name_;
    // The ids behind the picker's rows, in the order it lists them, so a
    // selection index means a session.
    std::vector<std::uint64_t> attach_choice_;
    void show_about();
    // Drops a calendar out of the menu-bar clock, the way a menu title drops
    // its menu.
    void open_calendar();
    void show_settings();
    // Freezes the focused terminal's history into a copy-mode surface over
    // its window, and takes it away again.
    void enter_copy_mode();
    void leave_copy_mode();
    // Sends the internal clipboard to the focused terminal, bracketed if the
    // program in it asked for bracketed paste.
    void paste_into_terminal();
    // Yanked text to every target `[terminal] clipboard` names, in order.
    void copy_to_targets(std::string text);
    // The caption a terminal window carries while its history is being read:
    // its own title plus the COPY badge and the position (plans/06).
    void refresh_copy_mode_caption();
    // Adopt a new value and store it. Terminals already open keep what they
    // started with; both return whether the store succeeded, so a dialog
    // changing two settings at once explains a read-only configuration file
    // once rather than once per setting.
    bool apply_login_shell(bool login_shell);
    bool apply_sixel_max_megapixels(int megapixels);
    bool apply_kill_grace_seconds(int seconds);
    // These two also change what is on screen at once — the clock and the
    // colours are what the reader is looking at while they choose.
    bool apply_clock_mode(ClockMode mode);
    bool apply_theme(Theme theme);
    void report_settings_not_saved();
    void report_config_warnings();
    void set_theme(ckv::ui::Theme theme);
    void populate_help();
    void dismiss_prefix_overlay();

    ckv::ui::Application& app_;
    ClientOptions options_;
    // Built from the defaults with the file's `bind`/`unbind` lines applied,
    // once, at construction: a rebinding is a startup-time decision (plans/08
    // — client-side options are read at attach) and every surface reads the
    // result rather than re-deriving it.
    Keymap keymap_;
    ckv::ui::StandardRoles roles_{};
    ckv::widgets::Desktop* desktop_ = nullptr;
    ckv::widgets::StatusLine* footer_ = nullptr;
    // The menu bar's trailing view, owned by the bar. Held because the
    // calendar hangs from it and has to tell it when it closes again.
    ckv::widgets::ClockView* clock_ = nullptr;
    // Per terminal window: the name ckmux gave it, and the last title its
    // child asked for. The first is what the window falls back to when the
    // child stops asking; the second is what stops us rewriting the caption
    // on every single frame.
    struct TerminalTitle {
        std::string fallback;
        std::string child;
    };
    std::unordered_map<ckv::widgets::Window*, TerminalTitle> titles_;
    ckv::widgets::MemoryHelpProvider help_;
    PrefixOverlay* overlay_ = nullptr;
    CopyModeView* copy_mode_ = nullptr;
    // The window whose history copy mode is showing, so its caption can be
    // put back and the surface can follow it if it moves.
    ckv::widgets::Window* copy_mode_window_ = nullptr;
    // ckmux's own clipboard: what `^B ]` pastes. Separate from the system
    // clipboard because a reader copying inside ckmux over SSH may have no
    // system clipboard that ckmux can reach at all.
    std::string internal_clipboard_;
    ckv::ui::Application::TimerId which_key_timer_ = 0;
    ckv::ui::Application::TimerId title_timer_ = 0;
    // The window whose confirmation the reader has already answered, so the
    // second close() pass does not ask again.
    ckv::widgets::Window* confirmed_close_ = nullptr;
    std::size_t next_terminal_number_ = 1;
    // Retained so a dialog's completion handler outlives the command that
    // opened it (ckVision's non-blocking dialog contract).
    std::vector<std::shared_ptr<void>> pending_dialogs_;
};

}  // namespace ckm::client
