// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The ckmux client: menu bar, desktop of terminal windows, and the permanent
// context-sensitive footer (the interface spec).
//
// It drives terminals it does not own. Every seam that decides where a
// terminal comes from, how it is titled, how it is closed, and how the footer
// and the prefix reach it is a `std::function` in `ClientOptions` — so an
// attached client substitutes the server for the fork and nothing below that
// line changes. Unset, those seams still do the M1 thing (launch here, close
// here), which is what keeps a ckmux with no server a working program and what
// lets a test drive the whole user interface with no socket in sight.
//
// The rule that holds all of it together: this class knows about WINDOWS, and
// the session layer knows about TERMINALS. Neither reaches into the other —
// `run_client.cpp` is the one place that has both and is where they are tied
// together.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "client/commands.hpp"
#include "client/copy_mode.hpp"
#include "client/prefix_overlay.hpp"
#include "client/printer_button.hpp"
#include "client/printer_output.hpp"
#include "client/stats_format.hpp"
#include "common/config.hpp"
#include "common/proto.hpp"
#include "cvision/term/terminal_subsession.hpp"
#include "cvision/ui/application.hpp"
#include "cvision/ui/layout.hpp"
#include "cvision/ui/standard_roles.hpp"
#include "cvision/widgets/common_components.hpp"
#include "cvision/widgets/desktop.hpp"
#include "cvision/widgets/help_viewer.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/status_line.hpp"
#include "cvision/widgets/window.hpp"
#include "cvision/widgets/window_switcher_bar.hpp"

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
    // HOW MANY readers are watching it, not whether — `SessionInfo::attached`
    // has been a count since WP-44, and narrowing it to a bool at this edge
    // was the last of the three places the second reader went unmentioned
    // (WP-48). One of them may be this client; the picker knows which.
    int readers = 0;
};

// One picker row's line: what is running in that session, and who is in it.
//
// A free function rather than dialog-building code, because it is the whole of
// what WP-48's reader count is FOR and the dialog is an awkward place to ask a
// question of. `watched` is the session this client holds, or 0 — the row for
// it counts this reader among its own and must not report itself as company.
std::string session_row_label(const SessionRow& row, std::uint64_t watched);

// One window's share of a FILLED TILING — ckVision's
// `Desktop::filled_tile_fractions()` for a single window, carried in the same
// units it answers in: offsets from `content_area()`'s own origin, each in
// [0, 1], with x + width and y + height at most 1 (WP-30).
//
// The window layer holds this rather than the wire's fixed-point form for the
// same reason it holds `ckv::Rect` rather than `proto::Rect`: this class knows
// desktops, not wires, and `run_client.cpp` is where one becomes the other.
struct TileShare {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    // Whether this window was part of a filled tiling at all. Zero extent is
    // the ordinary floating window — the arrangement was not a tiling, or this
    // window was not one of its participants.
    bool filled() const noexcept { return width > 0.0 && height > 0.0; }

    friend bool operator==(const TileShare&, const TileShare&) = default;
};

// One terminal window's place in the arrangement, as the WINDOW layer sees it
// (WP-29). The terminal is named by the subsession the window is showing rather
// than by a wire id, because this class does not know wire ids and must not
// learn them: it knows windows, the session layer knows terminals, and
// `run_client.cpp` is the one place that has both (see the file comment above).
//
// `z_order` counts from the bottom of the stack and is only meaningful against
// the other entries of the same report — a stack position is a comparison, not
// a coordinate — which is why a layout is always captured and reported as a
// whole arrangement rather than one window at a time. Raising one window
// renumbers the ones it passed, and a window that CLOSES leaves the arrangement
// without appearing in it at all.
struct WindowPlacement {
    const ckv::term::TerminalSubsession* terminal = nullptr;
    // The window on the desktop, frame included — NOT the terminal's own grid,
    // which is `content_rect()` and travels on an entirely different message
    // (the protocol spec, "Two geometries, and they are not the same number").
    ckv::Rect rect;
    std::uint16_t z_order = 0;
    bool zoomed = false;
    // And what fraction of the desktop it was, when the arrangement it belongs
    // to is a filled tiling (WP-30). Beside the rect rather than instead of it,
    // because the two are answers to different questions: the rect says where
    // the window was on the desktop it was measured on, and this says what
    // share of ANY desktop it should have — which is the only one of the two
    // that survives a reader reattaching at a different size.
    TileShare tile;

    friend bool operator==(const WindowPlacement&, const WindowPlacement&) = default;
};

// The two things a terminal can be saying to a reader who is elsewhere. Both
// are the SERVER's marks: it sets them when the child rings or writes, and
// clears them once it has told the client, so what a client holds is "since
// you last heard", not "ever".
struct TerminalMarks {
    bool bell = false;
    bool activity = false;
    // How many times this terminal has rung and written, counted by the server
    // and never reset (WP-41). The bools above are a LEVEL — a mark is up, and
    // the server decides when to put it down, which cannot be right for two
    // readers at once and cannot say "rang AGAIN" even to one. These say both.
    std::uint32_t bell_serial = 0;
    std::uint32_t activity_serial = 0;

    bool any() const noexcept { return bell || activity; }
};

struct ClientOptions {
    // Everything the configuration file said, defaults included (the configuration spec).
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
    // ordinary case (the configuration spec — unknown keys warn, never abort).
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
    // Why the last helper failed, in its own words, when the host can say.
    //
    // A clipboard helper's output must never reach the screen — ckmux is
    // drawing on those descriptors, and "xclip: command not found" printed
    // into a window frame stays there until something redraws it — so
    // `platform::write_to_command` captures it and the host hands it over
    // here. Unset (a test, a host with nothing to report) simply leaves the
    // explanation out of what the reader is told.
    std::function<std::string()> clipboard_problem;
    // What time it is where the reader is — the one wall-clock reading ckmux
    // takes, and injected for the same reason the clipboard writer is: reading
    // the host clock and its time zone is an application's job, never the
    // library's, and ckVision's own `Clock` is monotonic on purpose (the engineering standard
    // determinism rule). The host passes the system reading, a test passes a
    // fixed moment. Empty means no clock at all: one that has to guess the
    // time is worse than none.
    std::function<LocalMoment()> local_now;
    // How many SGR mouse reports the outer terminal's decoder recognized in
    // the byte stream — the host's count, for the Help ▸ Terminal Report
    // dialog to show beside what dispatch delivered. Empty (a test's
    // headless terminal) simply omits that line of the report.
    std::function<std::size_t()> mouse_reports_probe;
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
    // granted immediately and never negotiated (the session model).
    std::function<void(std::uint64_t session, proto::AttachMode mode)> attach_to_session;
    // Change what a reader may do in the session this client is attached to
    // (WP-49/50): `Me` is a self-restriction, `Others` is what this reader
    // imposes on the company. Unset for a client with no server, where there is
    // no company and nothing to restrict.
    std::function<void(proto::ReaderScope, proto::AttachMode)> set_reader_mode;
    // Creates one under a name the reader chose, and attaches to it.
    std::function<void(const std::string& name)> create_session;
    // Renames the session this client is attached to.
    std::function<void(const std::string& name)> rename_session;
    // Ends the session this client is watching: every program in it is asked to
    // quit, and — only if the reader says so — anything still running when the
    // grace period runs out is killed.
    std::function<void(bool force, int grace_seconds)> kill_session;
    // Ends one terminal the way the close dialog promises: its program is
    // asked to quit, and killed after `grace_seconds` only when `force` says
    // so. The window stays until the server says the terminal ended — a
    // window that closed itself early would be claiming something it cannot
    // know yet. Unset (M1: the terminal lives in this process), closing the
    // window closes the terminal directly and ckVision escalates for
    // whatever ignored the asking.
    std::function<void(ckv::term::TerminalSubsession& terminal, bool force, int grace_seconds)>
        close_terminal_in_session;
    // The session model's `kill-terminal`: SIGKILL the process group, drop the terminal.
    // No `force`, no `grace_seconds` — the absence of both IS this operation,
    // and giving it parameters it does not have would invite somebody to pass
    // them. Unset without a server, where there is no process group to reach
    // past ckVision's own escalation.
    std::function<void(ckv::term::TerminalSubsession& terminal)> kill_terminal_in_session;
    // Pins one terminal's caption to a name the reader chose, or hands it
    // back — an empty name means "use the default title again", which is
    // exactly what the rename dialog's third button asks for.
    //
    // Unset is M1: the client owns its terminals, so it owns the override too
    // and remembers it in `titles_`. An attached client sends `RenameTerminal`
    // and lets the answer come back, because a custom title is SESSION state
    // (the session model): it has to survive a detach, and the other client watching
    // the same session has to see it. A client that pinned a caption locally
    // would be showing a name that vanished the moment the reader reattached,
    // which is the one thing a multiplexer must not do with a name.
    std::function<void(ckv::term::TerminalSubsession& terminal, const std::string& name)>
        rename_terminal;
    // What the session layer says one terminal's custom title currently is.
    // Unset is M1 again: nobody else holds one, so the client's own record is
    // the answer and this is not asked.
    //
    // A query rather than a notification because that is how a caption already
    // reaches this class — `refresh_terminal_titles()` polls the child's title
    // on a timer, and asking one more question on the same tick costs a string
    // compare. A push would need its own route from the session layer into the
    // window layer, which is the seam this file exists to keep closed.
    std::function<std::string(const ckv::term::TerminalSubsession& terminal)> custom_title;
    // What a terminal's child exited with, where the host can say. The seam
    // carries `state()` but no number — `TerminalSubsession` has no
    // `exit_status()` — and the banner's whole point is to say WHICH status,
    // because `[exit 0]` and `[exit 1]` are different news to a reader
    // (the interface spec). A server-backed client reads it off the mirror; a client
    // with no server reads it off its own subsession; neither is a fact this
    // layer can get for itself, which is why it is asked for rather than
    // derived. No callback means no number, and the badge says `[exited]`.
    std::function<std::optional<int>(const ckv::term::TerminalSubsession& terminal)> exit_status;
    // Run this terminal's command again, in the window it is already in
    // (WP-13, the interface spec's `Enter restart`). Set only where something can
    // honour it: an attached client sends `RespawnTerminal`, and a client with
    // no server has nobody to ask — so an unset callback is what takes the
    // hint off the footer rather than offering a key that does nothing.
    std::function<void(const ckv::term::TerminalSubsession& terminal)> request_respawn;
    // Sends a paste into a REMOTE terminal, credit-paced (WP-18): the client
    // hands over the whole text and the session cuts it into chunks, keeping
    // only a couple on the wire at a time so a clipboard cannot outrun what
    // the child can drain. Returns false for a terminal this seam does not
    // own — a local one, or a mirror whose session has gone — and the caller
    // then writes the bytes straight into the subsession, which is right for
    // a PTY this process owns and is the only path a serverless ckmux has.
    //
    // The bytes arrive already encoded, bracketed-paste markers and all: what
    // a paste LOOKS like to a child is the terminal's business (the terminal-emulation spec) and
    // is decided against mirrored mode state, so nothing on the way to the
    // PTY re-encodes it.
    std::function<bool(const ckv::term::TerminalSubsession& terminal, std::string bytes)>
        paste_credited;
    // Asks the server to make the SESSION's desktop this size (WP-40) — the
    // one path by which a reader changes the coordinate space their windows,
    // and anybody else's, are arranged in. Unset without a server: a local
    // ckmux has one desktop and it is the screen.
    std::function<void(ckv::Size desktop)> fit_session_desktop;
    // What a terminal has done that a reader who is not looking at it would
    // want to know: it rang the bell, or it produced output (WP-19,
    // The interface spec). Asked for rather than derived, exactly as `exit_status`
    // is: the server owns both marks and sets them on `TermMeta`, the mirror
    // decodes them, and this layer has no way to reach that from a
    // `TerminalSubsession` alone. Unset means a client with nothing to ask —
    // the marks are simply never raised, which is what an unattached client
    // should show, since a reader with one desktop is looking at every
    // terminal it has.
    std::function<TerminalMarks(const ckv::term::TerminalSubsession& terminal)> terminal_marks;
    // Rings the reader's OWN terminal, for `[general] audible-bell`. Injected
    // because a client must not reach past ckVision to its host's output —
    // the same reason `clipboard_writer` is injected — and because a headless
    // test needs to count rings without a real terminal anywhere.
    std::function<void()> ring_host_bell;
    // Moves one terminal to another session, so its program keeps running
    // somewhere this desktop is not showing. `to_new_session` outranks
    // `destination`: the reader asked for a fresh one.
    std::function<void(ckv::term::TerminalSubsession& terminal, std::uint64_t destination,
                       bool to_new_session)>
        move_terminal;
    // --- The virtual printer (PRINT-3…6) -----------------------------
    //
    // Setting the policy at one of the three scopes. The client never resolves
    // precedence itself: the server owns the terminal → session → global rule
    // and answers with what is in force, so a dialog showing "effective here"
    // is showing the server's answer rather than a second implementation of it
    // that could disagree.
    //
    // Unset is M1 — no server, so no scopes above this process, and the
    // dialogs edit the client's own settings directly.
    std::function<void(std::uint64_t terminal, ckv::term::TerminalSubsession* target,
                       proto::PrinterScope scope, PrinterMode mode,
                       std::uint32_t ask_cache, std::uint32_t spool_limit)>
        set_printer_policy;
    // Asks for one job's text. The payload never travels unasked — a reader
    // who does not open a preview should not have paid to ship a megabyte —
    // so this is what opening one costs, and the answer arrives later through
    // the mirror.
    std::function<void(ckv::term::TerminalSubsession& terminal, std::uint64_t job)>
        fetch_print_job;
    // Forgets one job, or every one of a terminal's with 0.
    std::function<void(ckv::term::TerminalSubsession& terminal, std::uint64_t job)>
        discard_print_job;
    // What the session layer says one terminal's printer looks like now — the
    // mode in force, whether it is capturing or sinking, the live byte count
    // and the jobs held. A query on the client's own poll rather than a push,
    // for the reason `custom_title` is one: the button is assembled from this
    // and the reader's own answer together, and two readings a tick apart
    // could disagree.
    //
    // Unset is M1: no server, so the client reads the emulator's own scalars.
    std::function<PrinterButtonModel(const ckv::term::TerminalSubsession& terminal)>
        printer_status;
    // The jobs one terminal holds, as metadata, for the Print Output list.
    std::function<std::vector<proto::PrintJobInfo>(const ckv::term::TerminalSubsession& terminal)>
        printer_jobs;
    // One job's text, once it has arrived. Null until then — which is what the
    // preview shows as "fetching" rather than as an empty document.
    std::function<const std::string*(const ckv::term::TerminalSubsession& terminal,
                                     std::uint64_t job)>
        print_job_text;
    // The effective policy for one terminal, already worded — "Ask · 256 KB ·
    // 1 MB   (from: session)". The SERVER resolves precedence and says where
    // each answer came from; a client that re-derived it would be a second
    // implementation of the rule, free to disagree with the one in force.
    std::function<std::string(const ckv::term::TerminalSubsession& terminal)> printer_effective;
    // Writes a saved job where the reader asked. Injected for the reason the
    // clipboard writer is: the real one touches the filesystem, and a client
    // that touches the filesystem is one a test cannot drive. Answers whether
    // the write succeeded, and `printer_save_problem` says why when it did
    // not — a save that silently failed is the worst outcome of the four.
    std::function<bool(const std::string& path, std::string_view bytes)> write_print_file;
    std::function<std::string()> printer_save_problem;

    // Where this client's terminal windows now are (WP-29), reported once the
    // reader has finished moving them.
    //
    // Unset is M1: a client with no server has nobody to tell, and the whole
    // observation — the timer included — stays off rather than computing an
    // arrangement nothing consumes. An attached client passes a callback that
    // names each window's terminal on the wire and sends `SetLayout`.
    //
    // The WHOLE arrangement, every time, in z-order from the bottom. A layout
    // is session state owned by the server (the session model), and the server cannot
    // reconstruct one from per-window news: a raise renumbers windows nobody
    // touched, and a close removes a window that is then named nowhere.
    std::function<void(const std::vector<WindowPlacement>&)> report_layout;
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
    // How long the arrangement has to hold still before it is reported (WP-29).
    //
    // A drag produces a geometry change per frame and the server must not be
    // told about each one — WP-7 measured what an unthrottled per-frame channel
    // costs on the delta path, and the server's own producer coalesces to the
    // tick for the same reason. So this is a SETTLE interval, not a send rate:
    // the arrangement is sampled this often and reported only once two
    // consecutive samples agree, which makes a drag of any length exactly one
    // report rather than one report per this many nanoseconds.
    //
    // 150 ms because it has to be longer than the gap between two frames of a
    // reader's drag (mouse reports arrive far faster than that, and this
    // client's own loop wakes at least every 50 ms) and short enough that a
    // reader who moves a window and immediately detaches still has the move
    // recorded — a report lands between one and two intervals after the last
    // movement, so 150 to 300 ms.
    std::int64_t layout_settle_nanos = 150'000'000;
    // How long a window bar button must hold still before it may change
    // length again — wider after `switcher_grow_nanos`, narrower after
    // `switcher_shrink_nanos` (ckVision `WindowSwitcherBar::set_width_damping`).
    //
    // A terminal's caption is not a stable string: a shell rewrites it at
    // every prompt, and `make` writes its progress into one. Undamped, every
    // one of those re-sizes that button and slides every button beside it,
    // several times a second — the row becomes unreadable long before the
    // titles do, and a reader aiming at a button hits the one that took its
    // place.
    //
    // A second before widening and half a minute before narrowing, because
    // the two are not equally urgent: a button that is too NARROW is showing
    // an elided name and should catch up soon, while one that is too WIDE is
    // showing the whole name with slack around it — which costs the reader
    // nothing, and which the very next prompt is likely to need back.
    std::int64_t switcher_grow_nanos = 1'000'000'000;
    std::int64_t switcher_shrink_nanos = 30'000'000'000;

    // How long a toast stays before it takes itself away (WP-14). Five
    // seconds: long enough to read one line twice, short enough that a reader
    // who was not looking has not lost a row of their terminal for a minute.
    // A notification that must NOT go on a timer says so per-notification
    // (`persistent`), which is a different question from this one — see
    // `ClientApp::notify`. Held here so a test can turn expiry off and read
    // what was posted rather than racing a clock.
    std::int64_t toast_nanos = 5'000'000'000;

    // Tells the server whether this client wants per-terminal process stats
    // (WP-39's View toggles, any of them on). Unset means there is no server
    // to tell — the serverless client samples its own children locally
    // through the platform seam, on the application timer, and the footer
    // composer cannot tell the difference.
    std::function<void(bool)> watch_stats;
};

// The default child environment: TERM and friends per
// The terminal-emulation spec.
std::vector<std::pair<std::string, std::string>> default_environment();

class ClientApp {
public:
    explicit ClientApp(ckv::ui::Application& app, ClientOptions options = {});
    ~ClientApp();

    ClientApp(const ClientApp&) = delete;
    ClientApp& operator=(const ClientApp&) = delete;

    ckv::widgets::Desktop& desktop() noexcept { return *desktop_; }
    ckv::widgets::StatusLine& footer() noexcept { return *footer_; }
    // The taskbar row over the footer (WP-32). Not a second bottom dock —
    // `Desktop` holds one docked view per edge — but the upper half of the
    // `ui::Column` that IS the bottom dock, with the footer under it. On
    // screen while more than one terminal window is open, or while any
    // terminal is minimized; see `sync_window_switcher()`.
    ckv::widgets::WindowSwitcherBar& window_switcher() noexcept { return *switcher_; }

    // What the reader is told without being asked anything (WP-14): a session
    // taken over, a config file re-read, a detach that happened TO them
    // rather than one they asked for. Non-modal on purpose — every one of
    // these is news, and none of them is a question, so none may take the
    // keyboard away from the program the reader is typing into.
    //
    // `persistent` is for the one class that must not be missed: a reader
    // whose session was taken from them is looking at an empty desktop, and a
    // line that had already faded would leave them wondering where their work
    // went. Everything else expires on `ClientOptions::toast_nanos`.
    // What a reader is told when a detach happened TO them: taken over, the
    // session ended, the server stopped (WP-14). The wording lives here with
    // ckmux's other sentences rather than in the host loop, so a test can ask
    // what a reader would have been shown without standing up a server to
    // take a session away from it.
    //
    // `DetachReason::User` says nothing: a reader who asked to detach knows,
    // and is already being handed the picker.
    void report_detached(proto::DetachReason reason, const std::string& text);
    void notify(std::string text,
                ckv::widgets::NotificationSeverity severity =
                    ckv::widgets::NotificationSeverity::Info,
                bool persistent = false);
    // The surface itself, or nullptr while nothing has been posted — it is
    // created on the first notification and removed once the last one goes,
    // so an idle ckmux carries no notification chrome at all.
    ckv::widgets::NotificationCenter* notifications() noexcept { return toasts_; }

    // The help pages, so a caller can read what a reader would be shown
    // without presenting a viewer and scraping it — and so that a help key
    // named by some surface can be checked against the topics that exist,
    // which is the difference between F1 answering and F1 apologising.
    const ckv::widgets::MemoryHelpProvider& help() const noexcept { return help_; }

    // Whether ckmux's bottom chrome is collapsed to one row: the footer
    // hidden, the window bar on the last row of the terminal (WP-35).
    //
    // Both routes to it end here — the bar's own ▼ toggle and
    // `Window ▸ Status Bar` — so the glyph and the menu can never disagree
    // about a state there is only one of. Setting it is idempotent, and
    // states the whole answer rather than toggling: a caller that knows what
    // it wants must not have to know what is already true.
    void set_chrome_collapsed(bool collapsed);
    bool chrome_collapsed() const noexcept { return chrome_collapsed_; }

    // What this client is running under: the reader's file over the built-in
    // defaults, with anything the constructor had to resolve already resolved.
    // The host loop reads it because the requests it sends the server have to
    // name the same program this client would have launched — and because it
    // MOVED its options in here, so its own copy is not an answer any more.
    const Settings& settings() const noexcept { return options_.settings; }
    // One terminal's process-tree cost, from whichever measurer has it — the
    // server (via ServerSession::on_stats) or this client's own local sampler.
    // Remembered per subsession and written onto that terminal's window frame
    // footer, formatted per the View toggles (WP-39).
    void receive_terminal_stats(const ckv::term::TerminalSubsession& terminal,
                                const proto::TermStats& stats);
    // The SESSION's desktop — the world (WP-40/WP-43). Becomes the desktop's
    // extent, so a client whose screen is smaller than the session shows part
    // of it and pans; panning follows the focused window (a reader who
    // switches to a terminal off-screen means to see it), the offset is this
    // client's alone and never travels, and no window's rect changes because
    // somebody looked elsewhere — U7-a moves paint offsets, never bounds.
    void set_session_desktop(ckv::Size world);
    // `Window ▸ Fit Desktop to This Screen` (WP-40): tells the server this
    // reader's screen is the size the session's desktop should be. Every
    // window in the session is then re-laid against it and every child is
    // resized — for both readers, which is why it is asked for rather than
    // done whenever a terminal changes size.
    //
    // Public because the menu is no longer its only caller: `ckmux attach
    // --adopt-size` asks for the same act on arrival, and routing it through
    // here rather than sending its own `SetDesktopSize` keeps ONE answer to
    // "what does this reader's screen mean" — the whole screen, chrome rows
    // included — and gives the CLI reader the same acknowledgement the menu
    // reader gets.
    void fit_desktop_to_screen();
    // `Terminal ▸ Kill Terminal` (the session model). Asks first — the table says
    // "Confirmed in the UI" and this is why: SIGKILL takes a program's unsaved
    // work with no grace and no undo, so the one thing this must never be is
    // fast.
    void confirm_then_kill(ckv::widgets::Window* window);
    // The part of the world the reader can currently SEE, in world
    // coordinates: `content_area()` clipped to the view at its pan, mirroring
    // the library's own clamp arithmetic. Equal to `content_area()` itself
    // while no extent is set, which is what keeps every consumer written
    // before WP-43 byte-identical. The consumers that belong to the VIEW go
    // through this — a new window opens where the reader is looking, a toast
    // and the which-key popup hang where the reader's eyes are — while the
    // consumers that belong to the WORLD (zoom, tilings, layout restoration)
    // deliberately stay on `content_area()`, U7-a's own split.
    ckv::Rect view_content_area() const;
    // Whether any View readout is on — what a (re)connection tells the server
    // through `ClientOptions::watch_stats` / `ServerSession::set_stats_watched`.
    bool stats_watch_wanted() const noexcept {
        return options_.settings.show_cpu || options_.settings.show_memory_rss ||
               options_.settings.show_memory_real;
    }

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
    // What that session is called, as this client last heard it. Empty when
    // nothing is attached, or when no list has named it yet — a caller
    // composing a sentence about it has to handle both, because a session
    // this client never saw listed can still be taken away from it.
    const std::string& attached_session_name() const noexcept { return attached_session_name_; }
    // Puts this session's windows back where the reader left them (WP-30) —
    // the other half of `report_layout`, and what the whole layout cluster
    // exists for. Called with the arrangement the ATTACH SNAPSHOT carried, and
    // only that: a `LayoutDelta` is the server's record moving while the reader
    // watches, which is never a reattach and never something to lay down (see
    // `ServerSession::LayoutStatement`).
    //
    // **Three cases, and only the first that answers decides:**
    //
    //   * A window that was **maximized** is restored maximized, against the
    //     desktop as it is NOW rather than the one it was maximized on.
    //   * A window that was part of a **filled tiling** is restored at the same
    //     PROPORTION of the new desktop: a 50/50 split stays a 50/50 split at
    //     any size, where replaying its old cell rect would leave a gap or an
    //     overlap the moment the reader's terminal is not the size it was.
    //   * An ordinary **floating** window keeps its stored SIZE and is MOVED —
    //     up and left, only as far as it takes — to bring it as far inside the
    //     new desktop as it goes. Only then, only if it still does not fit, and
    //     only if `[general] resize-windows-to-fit` says so, is it resized down
    //     to what fits. The move always happens; the resize is a second step
    //     that is off by default, because a reader's window stays the size they
    //     made it unless they asked otherwise.
    //
    // **A window's place is restored once per attachment**, and after that the
    // reader owns it. Not every snapshot is a reattach: a mirror that falls
    // behind is healed by asking for one, which arrives down this same path
    // with every window already on screen and the reader part-way through
    // whatever they were doing. See `layout_settled_`.
    void apply_layout(const std::vector<WindowPlacement>& arrangement);
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
    // Whether the last list this client saw said somebody was already
    // watching that session — which, asked at the moment an attach succeeds,
    // is what makes the attach a TAKEOVER rather than an ordinary one
    // (WP-14). Read from the list that arrived BEFORE the attach: the fresh
    // one, requested straight afterwards, will say this client is watching it
    // and answer a different question.
    bool session_shows_attached(std::uint64_t id) const;
    // How many readers the server last said this client's session has, this
    // client included. One is the ordinary case and the reason the reader-mode
    // items and the footer count are hidden rather than greyed most of the time.
    int readers_here() const;
    // What this reader may do, and the notice when somebody else changed it
    // (WP-49). `told` distinguishes a mode this reader asked for — which needs
    // no announcement, they just did it — from one done TO them.
    void set_reader_mode(proto::AttachMode mode, bool told);
    // Hands the menu bar fresh items, for a checkmark that changed (WP-50).
    void refresh_menu_marks();
    proto::AttachMode reader_mode() const noexcept { return reader_mode_; }
    bool watching() const noexcept { return reader_mode_ == proto::AttachMode::Watch; }

    // A request the server refused, in front of the reader who made it. Every
    // request is answered (the protocol spec's `Error`), and an answer that reaches only
    // the client's front door is indistinguishable — to the person waiting —
    // from a server that has hung. Named by code and context as well as in
    // words, because those two are what a bug report is written from.
    void show_server_error(std::uint16_t code, const std::string& context,
                           const std::string& human);

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
    // is client-local by design (the session model: dialogs, the picker and copy mode
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
    // Wires the switcher bar to ckmux's own notion of a window (WP-32): which
    // windows it lists, what it calls them, what clicking one does, and what
    // right-clicking one offers. Called once, from `build_chrome()`, after the
    // bar and the footer have been composed into the docked Column.
    void install_window_switcher(ckv::widgets::WindowSwitcherBar& bar);
    // Puts the toast surface where it belongs and sizes it to what it holds,
    // or takes it away once nothing is left on it (WP-14). One place, called
    // after every change — including the expiries that happen on ckVision's
    // own timer, which this client would otherwise never hear about.
    void place_notifications();
    // The windows the bar lists: ckmux's TERMINAL windows, in the desktop's own
    // order. Dialogs, the session picker and the which-key popup are not places
    // a reader switches to.
    std::vector<ckv::widgets::Window*> switcher_windows() const;
    // What one row offers on a right press. Every entry acts on the window the
    // reader clicked, through `WindowSwitcherTarget::bind` — never through
    // command dispatch, which would act on `active_window()`, the one window
    // they demonstrably did not point at.
    std::vector<ckv::widgets::MenuItem> switcher_menu(
        const ckv::widgets::WindowSwitcherTarget& target);
    // Shows the bar exactly while there is more than one terminal window, and
    // tells the desktop its docked chrome changed height so the content area —
    // and every window clamped or zoomed into it — follows.
    void sync_window_switcher();
    void refresh_footer();
    void close_terminal(ckv::widgets::Window* window);
    void confirm_then_close(ckv::widgets::Window* window);
    // The move picker: where one terminal's program goes on running. Reached
    // from the close dialog's "Move instead…" and from ^B . — the same
    // question either way, so the same dialog.
    void show_move_terminal_dialog(ckv::term::TerminalSubsession* terminal);
    // The rename prompt for one terminal: a field holding whatever the caption
    // says now, and three ways out — name it, hand the name back, or leave it
    // alone. Holds the TERMINAL rather than the window for the reason the move
    // dialog does: a terminal can end while its dialog is open, and the lookup
    // failing IS the answer.
    void show_rename_terminal_dialog(ckv::term::TerminalSubsession* terminal);
    // --- The virtual printer's three surfaces (PRINT-4…6) -------------
    //
    // The Ask popup: a program is printing and nobody has said what to do
    // about it. Deliberately NOT modal — the program is still running, and a
    // modal box would stop a reader working in the terminal beside it while
    // they decide. Leaving it unanswered is a stable state, bounded by the ask
    // cache (the terminal-emulation spec).
    void show_printer_ask_dialog(ckv::term::TerminalSubsession* terminal);
    // Carries one answer to wherever it applies: the server when there is one,
    // this client's own settings when there is not, and the reader's file when
    // — and only when — the scope was global.
    void apply_printer_choice(ckv::term::TerminalSubsession* terminal, proto::PrinterScope scope,
                              PrinterMode mode);
    // Printer Settings: the three scopes, the mode, the two limits, and the
    // saving preferences. Shows the EFFECTIVE values and which scope each came
    // from, because a per-session override a reader set an hour ago and forgot
    // is exactly what an effective-value display must not hide.
    void show_printer_settings_dialog(ckv::term::TerminalSubsession* terminal);
    // Print Output: what this terminal has captured, and what a reader may do
    // with it — look, save, discard.
    void show_print_output_dialog(ckv::term::TerminalSubsession* terminal);
    // Saves one job, either to a name the reader chose or to the auto-name.
    void save_print_job(ckv::term::TerminalSubsession& terminal, std::uint64_t job,
                        const std::string& path);
    // What a reader is told when a save could not happen. A warning box rather
    // than a footer line: a save that failed quietly is the worst of the
    // outcomes, because the reader believes they have the document and finds
    // out when they need it.
    void show_printer_problem(const std::string& what);
    // What the frame button on one window should now say, assembled from the
    // mirror and this client's own memory of what its reader has answered.
    PrinterButtonModel printer_model_for(ckv::widgets::Window* window) const;
    // Re-reads every terminal's printer state and puts the button where it
    // belongs — adding the overlay when there is something to say and taking
    // it away when there is not. Runs on the same poll the captions do: the
    // byte counter has to move while a child prints, and the emulator raises
    // no event a client could subscribe to.
    void refresh_printer_buttons();
    // The window showing one terminal, or null when none does.
    ckv::widgets::Window* window_showing(const ckv::term::TerminalSubsession& terminal);
    // Completes a save that was waiting on its job text, if the text is here.
    void finish_pending_save();
    // Pins the caption, or — with an empty name — hands it back to whatever
    // the program says it is. One function for both because they are one
    // decision with two answers, and splitting them would be two places for
    // the "tell the server, then apply it here" order to be got wrong.
    void rename_terminal(ckv::term::TerminalSubsession* terminal, std::string name);
    // The window currently showing this terminal, or nullptr once it is gone.
    // Dialogs hold the terminal rather than the window, because a terminal
    // can end while its dialog is open — the lookup failing IS the answer.
    ckv::widgets::Window* window_showing(const ckv::term::TerminalSubsession* terminal) const;
    // And the reverse: the subsession behind one window, or nullptr for a
    // window that is not a terminal's. The result is non-const on purpose —
    // the map's keys are const only because a lookup never mutates, and the
    // caller gets back exactly the pointer open_terminal stored.
    ckv::term::TerminalSubsession* terminal_shown_by(ckv::widgets::Window* window) const;
    void focus_active_terminal();
    // The keyboard leaves a terminal that has left the screen — to whatever
    // the desktop activated in its place, or to nothing when every terminal
    // is now minimized. Called for every minimize the desktop reports,
    // whichever of the four routes into it the reader took.
    void keep_keyboard_off_hidden_terminals();
    // The window a digit chord means. The caption's number first, because that
    // is the number a reader can see; the desktop's own 1-9 convention when no
    // window carries it (the interface spec: "1-9 focus window").
    void focus_terminal_number(int number);
    ckv::widgets::Window* active_terminal() const;
    // Applies whatever titles the child programs have asked for since the
    // last frame. Polled rather than pushed: the emulator records the title
    // in its snapshot and raises no event, and a frame is exactly as often as
    // a caption can visibly change.
    void refresh_terminal_titles();
    // The whole arrangement as it stands right now: every window showing a
    // terminal, bottom of the stack first, with the desktop's own snapshot as
    // the source (ckVision `Desktop::snapshot()` walks its children in z-order
    // and carries each window's bounds and zoom state, so one call answers
    // where, how high and how big at a single instant rather than three
    // readings that could disagree). Dialogs are left out: a message box over
    // the desktop is not part of the reader's arrangement and would be a window
    // the server has no terminal for.
    std::vector<WindowPlacement> capture_layout() const;
    // The debounce, on `layout_settle_nanos`: sample, and report only once two
    // consecutive samples agree AND the settled arrangement differs from the
    // one last reported. Both halves matter — the first makes a drag one report
    // instead of one per frame, the second makes this edge-triggered, so a
    // desktop nobody is touching costs nothing on the wire however long the
    // timer runs.
    void report_layout_if_settled();
    // Where one stored placement goes on the desktop as it is now — the
    // geometry half of `apply_layout`, with no side effects, so the policy can
    // be reasoned about (and tested) as the function of two rectangles it is.
    // `area` is the current `content_area()`; zoom is the caller's business,
    // because maximizing is not a rect a window is set to but a state it enters.
    ckv::Rect restored_bounds(const WindowPlacement& placed, ckv::Rect area) const;
    void show_key_reference();
    // The complete listing, on the page rather than for the surface in front
    // of the reader — Help ▸ All Keybindings… (WP-14).
    void show_all_keys();
    // The name the New Session prompt offers, from the sessions last seen.
    std::string suggested_session_name() const;
    std::vector<SessionRow> last_sessions_;
    // Which window is showing which terminal, so that a terminal the server
    // has ended takes its window with it.
    std::map<const ckv::term::TerminalSubsession*, ckv::widgets::Window*> terminal_windows_;
    // The latest cost report per terminal, from the wire or from the local
    // sampler — a stream's most recent value, never persisted (WP-39). The
    // CPU baseline exists only for locally sampled terminals; a mirror's rate
    // was derived on the server.
    std::unordered_map<const ckv::term::TerminalSubsession*, proto::TermStats> latest_stats_;
    struct LocalCpuBaseline {
        std::uint64_t cpu_nanos = 0;
        std::int64_t at_nanos = 0;
        // A flag rather than a sentinel value, for WP-38's reason: a clock
        // may legitimately read zero.
        bool primed = false;
    };
    std::unordered_map<const ckv::term::TerminalSubsession*, LocalCpuBaseline> local_cpu_;
    // The local sampler's timer, 0 while none is armed (Application's ids
    // start at 1). Armed while any readout is on; cancelled when the last
    // goes off, so a client showing no stats spends no wakeups on them.
    ckv::ui::Application::TimerId stats_timer_ = 0;
    // Set while windows are being taken down for a session this client is
    // leaving, so the close path knows not to ask the server to end anything.
    bool forgetting_terminals_ = false;
    std::uint64_t attached_session_ = 0;
    std::string attached_session_name_;
    // What this reader may do in the session they are watching (WP-49/50), kept
    // in step with `ServerSession`'s copy by `run_client`. Held here because
    // three surfaces need it and none of them can see the session layer: the
    // footer says so persistently, the Session menu's items are gated on it,
    // and the picker preserves it across a switch the reader was never asked
    // about.
    proto::AttachMode reader_mode_ = proto::AttachMode::TakeOver;
    // Whether this reader has put the OTHERS on watch (WP-50). This client's
    // own belief about a state it imposed, not a fact the server states back —
    // there is no owner, so another reader may hand typing back at any moment
    // and this box would then be describing something that is no longer true.
    // Cleared whenever the company it describes goes away, which is the only
    // moment it is certainly wrong.
    bool others_read_only_ = false;
    // Whether the picker's last build offered the reader-mode group (WP-50).
    // It is offered only when somebody other than this client is watching
    // something, so the completion handler cannot assume a second radio is
    // there to read.
    bool mode_choice_offered_ = false;
    // The ids behind the picker's rows, in the order it lists them, so a
    // selection index means a session.
    std::vector<std::uint64_t> attach_choice_;
    // The move picker's rows, parallel to its radio options; the entry past
    // the last id is "a new session".
    std::vector<std::uint64_t> move_choice_;
    void show_about();
    // The ckVision terminal report over the OUTER host's capabilities, with
    // ckmux's decoded-SGR-reports probe added (options.mouse_reports_probe).
    void show_terminal_report();
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
    // its own title plus the COPY badge and the position (the interface spec).
    void refresh_copy_mode_caption();
    // Adopt a new value and store it. Terminals already open keep what they
    // started with; both return whether the store succeeded, so a dialog
    // changing two settings at once explains a read-only configuration file
    // once rather than once per setting.
    // --- The View readouts (WP-39) ---------------------------------------
    // One handler behind the three checkable items: flips the settings bool,
    // persists it through the same `save_setting` the Settings dialog's
    // checkboxes use, and re-applies everywhere at once.
    void toggle_stats_readout(bool Settings::* field, std::string_view key);
    // The toggles, applied: every terminal window's frame footer rewritten,
    // the View menu's checkmarks rebuilt, the server told whether anyone is
    // still watching, and the local sampler's timer armed or disarmed.
    void apply_stats_toggles();
    StatsToggles stats_toggles() const noexcept {
        return {options_.settings.show_cpu, options_.settings.show_memory_rss,
                options_.settings.show_memory_real};
    }
    void refresh_stats_footer(ckv::widgets::Window& window,
                              const ckv::term::TerminalSubsession& terminal);
    // The serverless half: one process-table pass over this client's own
    // children (the seam's `process_id()`, U5-b — a mirror answers -1 and is
    // skipped, because its numbers arrive over the wire instead). Runs on the
    // application timer at the same 1 Hz the server samples at.
    void sample_local_stats();
    // The whole menu set, built from current state — what `build_chrome`
    // installs and what a View toggle rebuilds, because a checkmark is state
    // and the bar's items are values.
    std::vector<ckv::widgets::MenuBarItem> build_menus();

    bool apply_login_shell(bool login_shell);
    bool apply_sixel_max_megapixels(int megapixels);
    bool apply_kill_grace_seconds(int seconds);
    // Read at the moment a layout is applied rather than at startup, so a
    // reader who ticks the box and reattaches gets the answer they just chose.
    bool apply_resize_windows_to_fit(bool resize);
    // These two also change what is on screen at once — the clock and the
    // colours are what the reader is looking at while they choose.
    bool apply_clock_mode(ClockMode mode);
    bool apply_theme(Theme theme);
    void report_settings_not_saved();
    void report_config_warnings();
    // The copy that did not leave ckmux, and what the helper said about it.
    void report_clipboard_problem(const std::vector<std::string>& refused);
    void set_theme(ckv::ui::Theme theme);
    void populate_help();
    void dismiss_prefix_overlay();

    ckv::ui::Application& app_;
    ClientOptions options_;
    // Built from the defaults with the file's `bind`/`unbind` lines applied,
    // once, at construction: a rebinding is a startup-time decision (the configuration spec
    // — client-side options are read at attach) and every surface reads the
    // result rather than re-deriving it.
    Keymap keymap_;
    ckv::ui::StandardRoles roles_{};
    ckv::widgets::Desktop* desktop_ = nullptr;
    ckv::widgets::StatusLine* footer_ = nullptr;
    // The bottom dock and its upper half (WP-32). The Column is held because
    // hiding the bar changes the Column's own preferred height, and the desktop
    // has to be told which docked view that was.
    ckv::ui::Column* chrome_stack_ = nullptr;
    ckv::widgets::WindowSwitcherBar* switcher_ = nullptr;
    // The toast surface (WP-14), a desktop popup so it stands above every
    // window rather than behind whichever one is maximized — the same layer
    // and the same lifetime rule as the prefix overlay: made when it is
    // needed, taken down when it is not.
    ckv::widgets::NotificationCenter* toasts_ = nullptr;
    // See set_chrome_collapsed. Held rather than read back off the footer's
    // visibility, because a footer can also be invisible for reasons that are
    // not this — and because the bar's toggle has to be put back in step with
    // it when the command is what moved it.
    bool chrome_collapsed_ = false;
    // This client's own liveness, for the callbacks the DESKTOP holds.
    //
    // The desktop belongs to the `Application` and outlives this object, so a
    // switcher provider, a bound menu action or a window-change observer that
    // captured `this` would otherwise be a call into freed storage at shutdown.
    // The same hazard the menu-bar clock avoids by copying its time provider
    // instead of reaching through the client — except that these callbacks
    // genuinely need the client, so they are guarded rather than avoided.
    // `subscribe_window_change`'s weak-lifetime overload drops its observer on
    // expiry, which is why nothing cancels one in the destructor.
    std::shared_ptr<void> alive_ = std::make_shared<char>();
    // The menu bar's trailing view, owned by the bar. Held because the
    // calendar hangs from it and has to tell it when it closes again.
    ckv::widgets::ClockView* clock_ = nullptr;
    // Per terminal window: the name ckmux gave it, and the last title its
    // child asked for. The first is what the window falls back to when the
    // child stops asking; the second is what stops us rewriting the caption
    // on every single frame.
    struct TerminalTitle {
        // The three, in the order they override each other. `fallback` is the
        // name ckmux gave the window and nothing ever takes away; `child` is
        // whatever the program in it last claimed with OSC 0/2, which is
        // ordinary and volatile; `custom` is the reader's, and outranks both.
        //
        // Three rather than one string that gets overwritten, because they
        // answer different questions and all three keep being asked. The child
        // goes on renaming itself underneath a custom title — that is the
        // whole point of the override being an override — so "use the default
        // title again" has a current answer to fall back to rather than the
        // stale one from whenever the reader pinned it.
        std::string fallback;
        // What the frame says about a program that has ended: `[exit 1]`, or
        // `[exited]` when nothing can supply the number (WP-13, the interface spec).
        // A fourth string rather than a rewrite of `child`, for the reason the
        // other three are separate: the child's last claimed title is still
        // wanted underneath the badge, and a respawn must be able to take the
        // badge off without having lost the name.
        std::string badge;
        std::string child;
        std::string custom;
        // The number in "Terminal 3", kept as a number so that `^B 3` can find
        // the window a reader is looking at rather than the third one in the
        // desktop's insertion order. The two agree until a window in the
        // middle closes, and then only the caption is visible.
        int number = 0;
        // What this window was last seen to be saying (WP-19), so the border
        // override, the host bell and the footer are each touched once per
        // CHANGE rather than rewritten on every poll — this runs several times
        // a second for every window.
        //
        // LAST in the struct deliberately: the one construction site
        // initialises positionally, and a field added in the middle silently
        // shifts `number` into `custom` — which is exactly what it did when
        // the `[exit]` badge went in above.
        bool lit = false;
        bool noted_activity = false;
        // How many title polls this terminal's bell has stood for. The poll is
        // the client's only regular tick, so the bell's whole behaviour is
        // counted in it rather than read off a clock: at 100 ms a phase is 5
        // ticks and five seconds is 50, which makes "blinks once a second" and
        // "stops after five seconds" the SAME mechanism at two multiples — and
        // makes both testable without a real clock anywhere near them.
        int bell_ticks = 0;
        // What the title currently shows for the bell: the glyph, a single
        // space during a blink's off phase, or nothing at all once the bell is
        // over. Stored rather than recomputed in `effective_title`, which is
        // static and has no business knowing about focus or ticks.
        std::string bell_cell;
        // What this reader has already answered by looking. The server's flag
        // is sticky — set when the child rings, cleared only on respawn — so
        // it says "this terminal has rung at some point", not "since you last
        // looked". The second sentence is the one a footer flag means, and it
        // is a statement about a READER rather than about a terminal: two
        // clients on one session are each in a different window, so the same
        // terminal is simultaneously one somebody is watching and one nobody
        // is. So the answer is remembered here, per client, and the terminal
        // keeps only the fact.
        // The serials this reader has answered by looking. A number rather
        // than a flag, because "I saw this bell" and "I saw every bell so far"
        // are the same statement only until it rings again — the case a flag
        // cannot express and the reason the wire now counts.
        std::uint32_t bell_answered = 0;
        std::uint32_t activity_answered = 0;
    };
    std::unordered_map<ckv::widgets::Window*, TerminalTitle> titles_;
    // The button on each window that has something to say about printing, and
    // whether this reader has answered the ask for that terminal.
    //
    // `answered` is the CLIENT's, not the server's, and that is the whole
    // point of it: it records what this reader has been shown and decided, and
    // a second client attaching to the same session has been shown nothing.
    // Putting it on the server would make one reader's "yes" silently answer
    // for another's terminal.
    struct PrinterSurface {
        PrinterButton* button = nullptr;
        bool answered = false;
    };
    std::unordered_map<ckv::widgets::Window*, PrinterSurface> printers_;
    // The job a preview is waiting for, so the text can be shown the moment it
    // finishes arriving rather than by polling for it.
    std::uint64_t awaiting_print_job_ = 0;
    std::string pending_save_path_;
    // The window whose terminal the pending save belongs to, used to find that
    // terminal again when the text arrives and to tell whether the reader has
    // closed it meanwhile. A window pointer rather than a subsession pointer
    // because its liveness is checkable against `desktop_->windows()`.
    ckv::widgets::Window* pending_save_window_ = nullptr;
    // What one terminal's window is called, given everything currently known
    // about it: the reader's name if they set one, else the program's if it
    // claimed one, else ckmux's own. The single answer, so the title poll, the
    // rename and copy mode's badge cannot each have an opinion. Declared here
    // rather than with the other methods because it reads the struct above and
    // a nested type has to exist before a signature can name it.
    static std::string effective_title(const TerminalTitle& title);
    ckv::widgets::MemoryHelpProvider help_;
    PrefixOverlay* overlay_ = nullptr;
    CopyModeView* copy_mode_ = nullptr;
    // The window whose history copy mode is showing, so its caption can be
    // put back and the surface can follow it if it moves — and, beside it, the
    // proof that this is still that window. The title poll writes a caption
    // through this pointer several times a second; a terminal the server ends
    // while its reader is in copy mode takes the window with it, and the write
    // that follows is into freed memory (ckVision View::lifetime_token).
    ckv::widgets::Window* copy_mode_window_ = nullptr;
    std::weak_ptr<void> copy_mode_window_alive_;
    // ckmux's own clipboard: what `^B ]` pastes. Separate from the system
    // clipboard because a reader copying inside ckmux over SSH may have no
    // system clipboard that ckmux can reach at all.
    std::string internal_clipboard_;
    ckv::ui::Application::TimerId which_key_timer_ = 0;
    ckv::ui::Application::TimerId title_timer_ = 0;
    // WP-29's two memories. `layout_seen_` is the previous sample and is what
    // "has it stopped moving?" is answered against; `layout_reported_` is what
    // was last handed to `report_layout` and is what "is this news?" is
    // answered against. Two rather than one because a settled arrangement and
    // an unreported one are different questions: a reader who drags a window
    // back to where it started leaves the first satisfied and the second not.
    std::vector<WindowPlacement> layout_seen_;
    std::vector<WindowPlacement> layout_reported_;
    ckv::ui::Application::TimerId layout_timer_ = 0;
    // And WP-30's one memory: the terminals whose windows have been placed from
    // a snapshot already, so that a place is restored once per attachment and
    // the reader owns it afterwards.
    //
    // What it guards is the HEAL. A mirror that loses track asks for every
    // terminal whole again, and that answer is an `Attached` — the same message
    // a reattach arrives on, down the same path, except that every window is
    // already on screen where the reader put it and the reader is in the middle
    // of using them. Without this, a dropped frame would pick the whole desktop
    // up and put it back, mid-session, for no reason a reader could see.
    //
    // Keyed by subsession rather than by window because that is what a
    // placement names, and cleared with the windows in `forget_terminals()`:
    // the next session's arrangement is a different desktop's.
    std::set<const ckv::term::TerminalSubsession*> layout_settled_;
    // The window whose confirmation the reader has already answered, so the
    // second close() pass does not ask again — as an address AND as an
    // identity. An address alone is a claim about a window that may have been
    // destroyed since the reader answered, and the next window allocated at
    // that address would inherit the answer and close without asking. The
    // token is what tells those two apart (ckVision View::lifetime_token).
    ckv::widgets::Window* confirmed_close_ = nullptr;
    std::weak_ptr<void> confirmed_close_alive_;
    std::size_t next_terminal_number_ = 1;
    // One dialog whose completion handler must outlive the command that opened
    // it (ckVision's non-blocking dialog contract), and the way to ask whether
    // it is finished with. The question is type-erased alongside the pointer
    // because these are several different presentation types and only the code
    // that made one knows how to ask it.
    struct PendingDialog {
        std::shared_ptr<void> presentation;
        std::function<bool()> completed;
    };
    std::vector<PendingDialog> pending_dialogs_;
    // Retains one, dropping the ones already answered. Every dialog a reader
    // opened used to be kept for the life of the client: a session's worth of
    // Settings boxes and About boxes, each holding its own captured state,
    // never freed because nothing ever asked whether they were over.
    //
    // It takes the `shared_ptr` rather than the presentation itself, because a
    // completion handler that captured it holds the same object — moving out
    // from under that handler would leave it watching an emptied one.
    template <class Presentation>
    void retain_dialog(std::shared_ptr<Presentation> presentation) {
        prune_pending_dialogs();
        std::shared_ptr<Presentation> held = std::move(presentation);
        pending_dialogs_.push_back(PendingDialog{held, [held] { return held->completed(); }});
    }
    void prune_pending_dialogs();
};

}  // namespace ckm::client
