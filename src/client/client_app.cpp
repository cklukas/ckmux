// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/client_app.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <utility>

#include "common/config.hpp"
#include "common/version.hpp"
#include "common/shell.hpp"
#include "platform/paths.hpp"
#include "platform/process_stats.hpp"

#include "cvision/core/text.hpp"
#include "cvision/widgets/application_shell.hpp"
#include "cvision/widgets/dialog.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/message_box.hpp"
#include "cvision/widgets/terminal_report_dialog.hpp"
#include "cvision/widgets/terminal_scrollbar.hpp"
#include "cvision/widgets/terminal_view.hpp"

namespace ckm::client {
namespace {

namespace w = ckv::widgets;
namespace u = ckv::ui;

// The version string children see, and the About box shows — stamped by
// CMake from project(ckmux VERSION ...), the one place the version is
// stated, so this can never disagree with the package or `--version`.
constexpr const char* kVersion = CKMUX_VERSION_STRING;

// How much of a child's own title a dialog will quote. The wire lets a
// caption be four kilobytes; a dialog note is ONE ROW, clipped rather than
// wrapped, so an unbounded quotation is a wall of the child's text with the
// sentence around it lost off the end. The one place ckmux prints a child's
// string somewhere other than a window frame therefore elides it, exactly as
// the frame itself is length-bounded ([02-architecture.md](02-architecture.md)
// security posture). Fifty-six columns plus its quotes and indent is 60, which
// fits the 80-column terminal the interface spec sizes every dialog against.
constexpr int kDialogTitleColumns = 56;

// ckmux's own printer mode, in the wire's vocabulary. The two enums are
// separate on purpose (proto::Rect's reasoning), so this is a stated mapping
// rather than a cast that would follow a renumbering silently.
proto::PrinterMode wire_printer_mode_of(PrinterMode mode) {
    switch (mode) {
        case PrinterMode::Ask: return proto::PrinterMode::Ask;
        case PrinterMode::Capture: return proto::PrinterMode::Capture;
        case PrinterMode::Off: return proto::PrinterMode::Off;
    }
    return proto::PrinterMode::Ask;
}

w::TerminalView* terminal_view_of(w::Window* window) {
    if (window == nullptr) return nullptr;
    return dynamic_cast<w::TerminalView*>(window->content());
}

// Where a tiling's boundary falls on an extent of `extent` cells (WP-30).
//
// An EDGE, deliberately, rather than a width: both windows either side of a
// boundary ask for the same fraction and get the same cell back, so the tiles
// still meet exactly and still fill the desktop. Rounding each window's WIDTH
// on its own does not — two halves of 79 columns each round to 40, and the
// arrangement that was a filled tiling comes back one column too wide.
int tile_edge(double fraction, int extent) {
    return static_cast<int>(std::llround(fraction * static_cast<double>(extent)));
}

// The choices the settings dialog offers, each label beside the value it
// stands for and in the order a reader sees them. One table rather than a list
// of labels and a switch over indices somewhere else: those two can be
// reordered independently, and then a dialog quietly stores the wrong answer.
template <typename Value>
struct Choice {
    std::string_view label;
    Value value;
};

constexpr std::array<Choice<ClockMode>, 3> kClockChoices{{{"With seconds", ClockMode::Seconds},
                                                          {"Without seconds", ClockMode::Minutes},
                                                          {"No clock", ClockMode::Off}}};
constexpr std::array<Choice<Theme>, 3> kThemeChoices{
    {{"Dark", Theme::Dark}, {"Light", Theme::Light}, {"Monochrome", Theme::Mono}}};

template <typename Value, std::size_t N>
std::vector<std::string> choice_labels(const std::array<Choice<Value>, N>& choices) {
    std::vector<std::string> labels;
    labels.reserve(N);
    for (const Choice<Value>& choice : choices) labels.emplace_back(choice.label);
    return labels;
}

template <typename Value, std::size_t N>
int choice_index(const std::array<Choice<Value>, N>& choices, Value value) {
    for (std::size_t i = 0; i < N; ++i)
        if (choices[i].value == value) return static_cast<int>(i);
    return -1;
}

// What the reader picked, or what they had. A dialog that came back with an
// index outside the list must not turn that into a setting.
template <typename Value, std::size_t N>
Value choice_value(const std::array<Choice<Value>, N>& choices, int index, Value fallback) {
    if (index < 0 || static_cast<std::size_t>(index) >= N) return fallback;
    return choices[static_cast<std::size_t>(index)].value;
}

// Whether closing `window` would take a program with it. A window whose child
// has already exited closes without asking: there is nothing to lose.
//
// Ready counts as live. A session becomes Running only once the child has
// produced output, so a program that has printed nothing yet — a shell
// waiting at a prompt it has not drawn, anything reading stdin — is still
// Ready, and is exactly the kind of thing a reader would not want closed
// silently.
// Whether this window's program has ended. Exited and Failed both count: to a
// reader a command that could not start and one that ran and stopped are the
// same situation — a window with nothing behind it — and both are offered the
// same two keys.
bool child_has_ended(const w::TerminalView* view) {
    if (view == nullptr) return false;
    using State = ckv::core::TerminalSubsessionState;
    const State state = view->session().state();
    return state == State::Exited || state == State::Failed;
}

bool child_is_live(w::Window* window) {
    w::TerminalView* const view = terminal_view_of(window);
    if (view == nullptr) return false;
    using State = ckv::core::TerminalSubsessionState;
    const State state = view->session().state();
    return state == State::Ready || state == State::Running;
}

// The file every configuration warning came from, when they all came from one
// and it looks like a path. Empty means "do not lift it out": one warning about
// the file itself and one about a line in another would both lose their
// subject, and a dialog that names the wrong file is worse than a wordy one.
std::string common_config_file(const std::vector<std::string>& warnings) {
    if (warnings.empty()) return {};
    const std::size_t colon = warnings.front().find(':');
    if (colon == std::string::npos) return {};
    const std::string prefix = warnings.front().substr(0, colon);
    if (prefix.empty() || prefix.find('/') == std::string::npos) return {};
    for (const std::string& warning : warnings)
        if (warning.size() <= colon || warning.compare(0, colon + 1, prefix + ":") != 0) return {};
    return prefix;
}

// One warning with that file struck off the front: "4: unknown key 'shel'".
std::string without_prefix(const std::string& warning, const std::string& prefix) {
    if (prefix.empty() || warning.size() <= prefix.size() + 1) return warning;
    std::string rest = warning.substr(prefix.size() + 1);
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
    return rest;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> default_environment() {
    // Only what ckmux itself has an opinion about. HOME, USER, LANG, PATH and
    // everything else the reader's shell was configured with arrive through
    // the launch spec's inherit-and-override policy: a terminal that hands
    // its child a machine other than the one its user set up is not a
    // terminal, and the failures look like the program is broken rather than
    // like the environment is missing.
    //
    // The terminal-emulation spec: xterm-256color first, with honest
    // capability replies behind it; a ckmux-specific terminfo entry replaces
    // it at M5. TERM_PROGRAM lets a child tell what it is running under.
    return {{"TERM", "xterm-256color"},
            {"COLORTERM", "truecolor"},
            {"TERM_PROGRAM", "ckmux"},
            {"TERM_PROGRAM_VERSION", kVersion}};
}

ClientApp::ClientApp(u::Application& app, ClientOptions options)
    : app_(app), options_(std::move(options)) {
    if (options_.environment.empty()) options_.environment = default_environment();
    // Resolved once, here, rather than per terminal: every window in one
    // session should run the same shell, and a reader who changes $SHELL
    // mid-session has not asked for their next window to differ from the
    // last one.
    if (options_.settings.shell.empty()) options_.settings.shell = ckm::resolve_shell();
    // The reader's rebinding, applied once and before anything reads the
    // table: the commands are registered from it, the menus quote it, and the
    // footer advertises it. Anything the file asked for that ckmux cannot do
    // joins the warnings the reader is shown at startup, in the same list as
    // a misspelled key — a binding that quietly does nothing is the failure
    // this project pins tests against.
    //
    // Send-prefix defaults to the prefix key itself — ^B ^B types a ^B — and
    // the prefix is settled by now, so the default is seeded before the
    // reader's directives get their say over it.
    keymap_.set_default_chord(Action::SendPrefix, ckm::chord_spelling(options_.settings.prefix));
    keymap_.apply(options_.settings.binds, options_.config_warnings);
    roles_ = u::intern_standard_roles(app_.roles());
    // [general] theme, applied before anything is drawn — and before
    // build_chrome(), which hands the shell whatever is on the application
    // rather than naming a theme of its own. Settings ▸ General… changes it
    // and stores it.
    switch (options_.settings.theme) {
        case Theme::Dark: set_theme(u::make_dark_theme(app_.roles(), roles_)); break;
        case Theme::Light: set_theme(u::make_light_theme(app_.roles(), roles_)); break;
        case Theme::Mono: set_theme(u::make_mono_theme(app_.roles(), roles_)); break;
    }
    register_commands();
    build_chrome();
    populate_help();
    app_.set_help_provider([this](const std::string& key) {
        if (desktop_ == nullptr) return;
        retain_dialog(std::make_shared<w::HelpViewerPresentation>(
            w::present_help_viewer(help_, key, app_, *desktop_, roles_)));
    });
    footer_->set_hint_provider([](const std::string& key) -> std::string {
        if (key == "ckmux.prefix") return "choose a key, or Esc to cancel";
        if (key == "ckmux.terminal") return "keys go to the program in this window";
        return {};
    });
    if (options_.open_terminal_at_startup) new_terminal();
    refresh_footer();
    report_config_warnings();
    title_timer_ = app_.start_timer(options_.title_poll_nanos, /*repeating=*/true,
                                    [this] { refresh_terminal_titles(); });
    // Where the reader's windows are, watched only when somebody is listening
    // (WP-29). With no `report_layout` there is no server to tell, and the
    // timer does not exist at all rather than sampling an arrangement into a
    // callback that is not there — which is exactly the M1 client, and is why
    // nothing about a ckmux with no server changed when this landed.
    if (options_.report_layout && options_.layout_settle_nanos > 0)
        layout_timer_ = app_.start_timer(options_.layout_settle_nanos, /*repeating=*/true,
                                         [this] { report_layout_if_settled(); });
    // The View readouts the config file already had on: applied now so a
    // restarted client comes up with the checkmarks ticked, the subscription
    // stated, and the sampler armed — the preference survived because the
    // reader flipped it, and startup is where that has to become visible.
    apply_stats_toggles();
}

ClientApp::~ClientApp() {
    // First, before a single member is torn down: this expires every callback
    // the DESKTOP is holding on this client's behalf — the switcher's four
    // providers, the menu actions bound to a window, and the window-change
    // observer. The desktop outlives this object, so the alternative is a
    // provider reading members that are already gone. Dropped here rather than
    // left to the member's own destruction, whose place in the order would then
    // be load-bearing and silently so.
    alive_.reset();
    if (which_key_timer_ != 0) app_.cancel_timer(which_key_timer_);
    if (title_timer_ != 0) app_.cancel_timer(title_timer_);
    if (layout_timer_ != 0) app_.cancel_timer(layout_timer_);
    if (stats_timer_ != 0) app_.cancel_timer(stats_timer_);
}

void ClientApp::register_commands() {
    u::CommandRegistry& registry = app_.commands();
    // Only ckmux's own commands are declared here. ckVision's standard set is
    // declared by every registry's constructor with its own titles and default
    // chords, and its Desktop installs the window-management handlers —
    // referencing those by their keys is the sanctioned path, and re-declaring
    // one would replace metadata the library owns.
    //
    // "Installs the handlers" is worth checking rather than assuming, which is
    // what test_command_reachability.cpp does: an unhandled command still
    // reports itself available, so a menu item bound to one looks live and
    // does nothing. Window List shipped that way through M1, because Desktop
    // covered every window command except that one (fixed upstream — 07's
    // L-32).
    //
    // Nobody picks a number: `declare()` assigns the id that stands for each
    // key, and `keymap_.resolve()` at the end of this function is where the
    // table learns them (ckVision D-013).
    const auto declare = [&](std::string_view key, std::string_view title, std::string category,
                             std::function<void()> handler) {
        return registry.declare(u::CommandDescriptor{.key = std::string(key),
                                                     .title = std::string(title),
                                                     .category = std::move(category),
                                                     .context = {},
                                                     .chord = {},
                                                     .visibility = u::CommandVisibility::Palette,
                                                     .handler = std::move(handler)});
    };

    for (const KeyBinding& binding : keymap_.bindings()) {
        // A row whose key belongs to the library is the library's to declare.
        if (!binding.key.starts_with("ckmux.")) continue;
        if (binding.key == commands::kNewTerminal) {
            const u::CommandId new_term =
                declare(binding.key, binding.title, "Terminal", [this] { new_terminal(); });
            // A terminal lives in a session. A client with none — one that
            // started without attaching — cannot open one, and says so plainly
            // rather than sending a request the server will refuse.
            //
            // A client with no server at all is a different thing: M1's local
            // ckmux owns its terminals directly and has no sessions to be
            // outside of, so nothing is gated there.
            registry.set_enabled_predicate(new_term, [this] {
                return !options_.detach_from_server || attached_session_ != 0;
            });
        } else if (binding.key == commands::kCloseTerminal) {
            declare(binding.key, binding.title, "Terminal", [this] { close_terminal(active_terminal()); });
        } else if (binding.key == commands::kKillTerminal) {
            const u::CommandId kill =
                declare(binding.key, binding.title, "Terminal",
                        [this] { confirm_then_kill(active_terminal()); });
            // Two conditions, and both are real. Without a server there is no
            // process group to signal past ckVision's own escalation, so the
            // operation does not exist; and with no terminal there is nothing
            // to aim it at — a state a reader reaches by closing the last
            // window, so it is asked rather than assumed.
            registry.set_enabled_predicate(kill, [this] {
                return static_cast<bool>(options_.kill_terminal_in_session) &&
                       active_terminal() != nullptr;
            });
        } else if (binding.key == commands::kMoveTerminal) {
            // Somewhere else for the program to keep running requires there
            // to BE a somewhere else: a server with sessions, not M1's one
            // process. Gated like every session command, because that is
            // what it is — the terminal is only the passenger.
            const u::CommandId move = declare(binding.key, binding.title, "Terminal", [this] {
                show_move_terminal_dialog(terminal_shown_by(active_terminal()));
            });
            registry.set_enabled_predicate(move, [this] {
                return options_.move_terminal && attached_session_ != 0;
            });
        } else if (binding.key == commands::kRenameTerminal) {
            // Aimed at the ACTIVE terminal, like Close Terminal beside it. The
            // window bar's context menu aims at the row the reader pointed at
            // instead, which is why that one is not wired as this command
            // (`switcher_menu`, and ckVision's WindowSwitcherTarget for why).
            const u::CommandId rename = declare(binding.key, binding.title, "Terminal", [this] {
                show_rename_terminal_dialog(terminal_shown_by(active_terminal()));
            });
            // Nothing to name is a menu item that is plainly not available,
            // not a dialog that opens onto no terminal. An empty desktop is
            // exactly that state and a reader can reach it (close the last
            // window), so it is asked rather than assumed.
            registry.set_enabled_predicate(rename,
                                           [this] { return active_terminal() != nullptr; });
        } else if (binding.key == commands::kShowCpuUsage) {
            declare(binding.key, binding.title, "View",
                    [this] { toggle_stats_readout(&Settings::show_cpu, "show-cpu"); });
        } else if (binding.key == commands::kShowMemoryRss) {
            declare(binding.key, binding.title, "View", [this] {
                toggle_stats_readout(&Settings::show_memory_rss, "show-memory-rss");
            });
        } else if (binding.key == commands::kShowMemoryReal) {
            declare(binding.key, binding.title, "View", [this] {
                toggle_stats_readout(&Settings::show_memory_real, "show-memory-real");
            });
        } else if (binding.key == commands::kFitDesktop) {
            // Available only where there is a session to reshape: a client
            // with no server has one desktop and it is already this screen.
            const u::CommandId fit =
                declare(binding.key, binding.title, "Session", [this] { fit_desktop_to_screen(); });
            registry.set_enabled_predicate(
                fit, [this] { return static_cast<bool>(options_.fit_session_desktop); });
        } else if (binding.key == commands::kToggleStatusBar) {
            // The keyboard's and the menu's way to the same state the bar's ▼
            // reports (WP-35). It has to exist separately from that toggle
            // because the bar itself is not always on screen — one terminal,
            // none minimized, and the only route back to the footer would be
            // a row that is not there.
            declare(binding.key, binding.title, "Window",
                    [this] { set_chrome_collapsed(!chrome_collapsed_); });
        } else if (binding.key == commands::kPrintOutput) {
            const u::CommandId print = declare(binding.key, binding.title, "Terminal", [this] {
                show_print_output_dialog(terminal_shown_by(active_terminal()));
            });
            registry.set_enabled_predicate(print,
                                           [this] { return active_terminal() != nullptr; });
        } else if (binding.key == commands::kPrinterSettings) {
            const u::CommandId settings = declare(binding.key, binding.title, "Terminal", [this] {
                show_printer_settings_dialog(terminal_shown_by(active_terminal()));
            });
            registry.set_enabled_predicate(settings,
                                           [this] { return active_terminal() != nullptr; });
        } else if (binding.key == commands::kMoveResize) {
            declare(binding.key, binding.title, "Window", [this] {
                if (w::Window* const window = desktop_->active_window()) window->enter_move_mode();
            });
        } else if (binding.key == commands::kKeyReference) {
            declare(binding.key, binding.title, "Help", [this] { show_key_reference(); });
        } else if (binding.key == commands::kAbout) {
            declare(binding.key, binding.title, "Help", [this] { show_about(); });
        } else if (binding.key == commands::kSettings) {
            declare(binding.key, binding.title, "Settings", [this] { show_settings(); });
        } else if (binding.key == commands::kCopyMode) {
            declare(binding.key, binding.title, "Terminal", [this] { enter_copy_mode(); });
        } else if (binding.key == commands::kPaste) {
            const u::CommandId paste = declare(binding.key, binding.title, "Terminal",
                                               [this] { paste_into_terminal(); });
            // Nothing to paste is not a failure to explain in a dialog; it is
            // a menu item that is plainly not available yet, which is what an
            // empty clipboard looks like everywhere else.
            registry.set_enabled_predicate(paste, [this] { return !internal_clipboard_.empty(); });
        } else if (binding.key == commands::kDetach) {
            // Detaching leaves everything running and closes this client. It is
            // available exactly when there is a server to detach from: in the
            // M1 world there is nothing to leave, and a menu item that pretends
            // otherwise is worse than one that is plainly not available.
            const u::CommandId detach = declare(binding.key, binding.title, "Session", [this] {
                if (options_.detach_from_server) options_.detach_from_server();
            });
            registry.set_enabled_predicate(
                detach, [this] { return static_cast<bool>(options_.detach_from_server); });
        } else if (binding.key == commands::kNewSession) {
            // Straight to the name prompt: creating a session is one question
            // (what is it called?), and the picker is not part of asking it.
            const u::CommandId new_session = declare(binding.key, binding.title, "Session", [this] {
                show_new_session_dialog(suggested_session_name());
            });
            registry.set_enabled_predicate(new_session, [this] {
                return static_cast<bool>(options_.create_session);
            });
        } else if (binding.key == commands::kRenameSession) {
            const u::CommandId rename = declare(binding.key, binding.title, "Session", [this] {
                show_rename_session_dialog(attached_session_name_);
            });
            registry.set_enabled_predicate(rename, [this] {
                return options_.rename_session && attached_session_ != 0;
            });
        } else if (binding.key == commands::kKillSession) {
            const u::CommandId kill =
                declare(binding.key, binding.title, "Session", [this] { show_kill_session_dialog(); });
            registry.set_enabled_predicate(kill, [this] {
                return options_.kill_session && attached_session_ != 0;
            });
        } else if (binding.key == commands::kSessions) {
            // What is running, and which of it this client is watching. Not the
            // picker — switching between sessions is WP-9 — but a reader asking
            // "what is on this server?" gets a true answer instead of a greyed
            // menu item.
            const u::CommandId sessions = declare(binding.key, binding.title, "Session", [this] {
                if (options_.list_sessions) options_.list_sessions();
            });
            registry.set_enabled_predicate(
                sessions, [this] { return static_cast<bool>(options_.list_sessions); });
        } else if (binding.key == commands::kSendPrefix) {
            declare(binding.key, binding.title, "Terminal", [this] {
                // The view encodes it, because the view is what encodes every
                // other key this terminal receives — same modifiers, same
                // keyboard protocol, same everything (ckVision `send_key`,
                // which delivers a synthesized key without claiming the parent
                // escape it would otherwise intercept).
                //
                // ckmux used to spell the bytes itself, and could only spell
                // Ctrl+letter: a reader whose `prefix = F5` or `prefix = A-a`
                // was offered "send the prefix" in the menu, in the footer and
                // in the which-key popup, pressed it, and nothing whatever
                // happened. A chord ckmux advertises has to arrive.
                if (w::TerminalView* const view = terminal_view_of(active_terminal()))
                    (void)view->send_key(ckv::KeyEvent{options_.settings.prefix});
            });
        } else if (binding.key == commands::kFocusByNumber) {
            // Display-only: the digits carry a value, so they are dispatched
            // directly rather than through one handler.
            registry.set_enabled_predicate(declare(binding.key, binding.title, "Terminal", [] {}),
                                           [] { return false; });
        }
    }

    // No keymap row, so no iteration of the loop above declares it: the
    // complete listing is reached from Help ▸ All Keybindings… and from F1 on
    // a surface with nothing more specific to say (WP-14).
    // "A&ll", not "&All": this menu already has "&About ckmux…" on A, and two
    // items sharing a mnemonic means the second one cannot be typed at all.
    declare(commands::kAllKeys, "A&ll Keybindings…", "Help", [this] { show_all_keys(); });

    // The three reader-mode acts (WP-50), declared here rather than off the
    // keymap table because they have no chords — see `commands.hpp`.
    //
    // Every one of them is gated on there being somebody to act on or
    // something to stop doing, and greying is right where hiding would be
    // wrong: a reader who has heard of "Take Session Over" should find it in
    // the menu, greyed, and learn from that that nobody else is in here.
    {
        const u::CommandId take_over =
            declare(commands::kTakeSessionOver, "&Take Session Over", "Session", [this] {
                if (!options_.set_reader_mode) return;
                options_.set_reader_mode(proto::ReaderScope::Others, proto::AttachMode::TakeOver);
                const int others = readers_here() - 1;
                notify(others == 1 ? "The other reader was dropped to their picker"
                                   : "The other " + std::to_string(others) +
                                         " readers were dropped to their pickers");
            });
        registry.set_enabled_predicate(take_over, [this] {
            return options_.set_reader_mode && attached_session_ != 0 && readers_here() > 1 &&
                   !watching();
        });

        const u::CommandId others_read_only =
            declare(commands::kOthersReadOnly, "Others &Read-Only", "Session", [this] {
                if (!options_.set_reader_mode) return;
                others_read_only_ = !others_read_only_;
                options_.set_reader_mode(proto::ReaderScope::Others,
                                         others_read_only_ ? proto::AttachMode::Watch
                                                           : proto::AttachMode::Join);
                // A checkmark is a value in the bar's items, so the bar gets
                // fresh ones — the same way the View toggles refresh theirs.
                refresh_menu_marks();
            });
        registry.set_enabled_predicate(others_read_only, [this] {
            return options_.set_reader_mode && attached_session_ != 0 && readers_here() > 1 &&
                   !watching();
        });

        const u::CommandId watch_only =
            declare(commands::kWatchOnly, "&Watch Only", "Session", [this] {
                if (!options_.set_reader_mode) return;
                const bool now_watching = !watching();
                options_.set_reader_mode(proto::ReaderScope::Me, now_watching
                                                                     ? proto::AttachMode::Watch
                                                                     : proto::AttachMode::Join);
                // Believed at once rather than awaited: the server sends no
                // `ReaderMode` back for a mode this reader asked for, so
                // nothing else will ever tell this client what it just did.
                set_reader_mode(now_watching ? proto::AttachMode::Watch : proto::AttachMode::Join,
                                /*told=*/false);
                refresh_menu_marks();
            });
        registry.set_enabled_predicate(
            watch_only, [this] { return options_.set_reader_mode && attached_session_ != 0; });
    }

    // Quitting an attached client DETACHES it.
    //
    // ckVision's standard quit sweeps the windows first and lets any of them
    // veto — right for an application with documents, wrong for a multiplexer:
    // a reader who quits ckmux while `htop` is running was asked "Terminal is
    // still running a program. Close it anyway?", which is a question about the
    // wrong thing entirely. Their program is not being closed; their view of it
    // is. `set_handler` on a standard command is the sanctioned way to say so
    // (ckVision's own command.hpp gives this as the example).
    // Replaced only when there IS a server. With none, ckVision's own handler is
    // exactly right — the terminals really are ending, so sweeping the windows
    // and asking about the live ones is what a reader wants.
    if (options_.detach_from_server) {
        registry.set_handler(registry.standard().quit,
                             [this] { options_.detach_from_server(); });
    }

    // The terminal report, claimed for the same sanctioned reason: Desktop's
    // default handler shows the report without the decoded-SGR-reports line,
    // because only the host knows its terminal can count them. ckmux's outer
    // terminal is a POSIX host that can, so the claim adds the one line that
    // separates "the terminal never sent the click" from "it arrived and
    // landed somewhere unexpected" — the evidence WP-16's advertisement
    // decision reads.
    registry.set_handler(registry.standard().terminal_report,
                         [this] { show_terminal_report(); });

    // The prefix key itself, as a command with a chord.
    //
    // A terminal view consumes every key except the prefix, so this binding can
    // only ever fire when no terminal has focus — which is exactly the state a
    // client with no session is in. Without it the footer advertised "^B m
    // menu" to a reader who had just chosen to start without a session, and not
    // one of those keys did anything: the prefix lived on the terminal view,
    // and there was no terminal.
    registry.bind_key(options_.settings.prefix,
                      registry.declare(u::CommandDescriptor{
                          .key = std::string(commands::kArmPrefix),
                          .title = "Prefix",
                          .category = "Terminal",
                          .context = {},
                          .chord = {},
                          .visibility = u::CommandVisibility::Hidden,
                          .handler = [this] { arm_prefix(); }}));

    // Everything is declared, so the table can learn what its keys resolve to.
    // Every surface reads ids from here afterwards, which is why this is the
    // last line rather than an incremental step inside the loop above.
    keymap_.resolve(registry);
}

std::vector<w::MenuBarItem> ClientApp::build_menus() {
    // Every menu entry shows the key that reaches it from a terminal — the
    // prefix chord, not whatever single chord the registry holds. The two
    // differ for ckVision's standard commands, and the prefix one is the one
    // that works where a ckmux reader spends their time.
    const auto presentation = [this](const KeyBinding& binding) {
        return w::CommandPresentation{binding.command, std::string(binding.title),
                                      binding_label(options_.settings.prefix, binding)};
    };
    // A menu entry names its command the way everything durable does — by key
    // — and the id comes from the table, which resolved it once at startup.
    // A key with no row (nothing in ckmux reaches it by a prefix chord) still
    // makes a menu item; it simply carries no chord hint of ckmux's own.
    const auto item = [&](std::string_view key) {
        for (const KeyBinding& binding : keymap_.bindings())
            if (binding.key == key) return w::MenuItem::command(presentation(binding));
        const std::optional<u::CommandId> id = app_.commands().id_for(key);
        return w::MenuItem::command(w::CommandPresentation{id.value_or(u::kInvalidCommand)});
    };
    const w::MenuItem separator = w::MenuItem::separator();

    const auto checkable = [&](std::string_view key, bool on) {
        return item(key).with_mark(on ? w::MenuMark::Checked : w::MenuMark::Unchecked);
    };

    // No New Terminal here: a terminal is the Terminal menu's to make, and the
    // same item under two titles is a reader wondering which one they used.
    // This menu holds what happens to SESSIONS.
    w::MenuBarItem session_menu{
        "&Session",
        {item(commands::kNewSession), item(commands::kSessions),
         item(commands::kRenameSession), item(commands::kKillSession), separator,
         item(commands::kTakeSessionOver), checkable(commands::kOthersReadOnly, others_read_only_),
         checkable(commands::kWatchOnly, watching()), separator,
         item(commands::kDetach), item(u::std_command_keys::kQuit)}};
    // Rule 2 of the interface spec: everything is in the menu, with the chord that
    // reaches it beside it. Copy Mode, Paste and Send Prefix live here — the
    // three verbs a reader aims at the terminal itself rather than at the
    // desktop around it.
    w::MenuBarItem terminal_menu{
        "&Terminal",
        {item(commands::kNewTerminal), item(commands::kCloseTerminal),
         item(commands::kKillTerminal), item(commands::kRenameTerminal),
         item(commands::kMoveTerminal), separator,
         item(commands::kPrintOutput), item(commands::kPrinterSettings), separator,
         item(u::std_command_keys::kNextWindow), item(u::std_command_keys::kPreviousWindow),
         separator, item(commands::kCopyMode), item(commands::kPaste), separator,
         item(commands::kSendPrefix)}};
    // The View menu (WP-39): what this client SHOWS, distinct from what a
    // setting configures — the theme stayed a stored Settings value for
    // exactly that reason, and these three are the other thing: live toggles
    // a reader flips while looking at the answer. Checkable, persisted as
    // `[general]` keys the moment they change, and applied to every open
    // terminal window at once (`apply_stats_toggles`). The checkmark is a
    // value in the item, which is why a toggle rebuilds the menus.
    w::MenuBarItem view_menu{
        "&View",
        {checkable(commands::kShowCpuUsage, options_.settings.show_cpu),
         checkable(commands::kShowMemoryRss, options_.settings.show_memory_rss),
         checkable(commands::kShowMemoryReal, options_.settings.show_memory_real)}};
    // The window list is a list of WINDOWS — it lives with the other commands
    // that arrange them, not under Terminal, where it sat because M1 had no
    // Window menu yet to put it in.
    //
    // The three tilings are named for the arrangement each produces, and the
    // library's unqualified `Tile` is deliberately not among them: it produces
    // exactly what Tile Vertically produces (U4-b), so a menu carrying both
    // would offer one behaviour twice under two names — and a reader choosing
    // between the two entries would be choosing between nothing.
    //
    // Minimize sits beside Zoom, which is where a reader looks for it and
    // what the window's own frame says by drawing the two controls together
    // (WP-34). Status Bar is last and on its own: it is the only item here
    // that arranges ckmux's CHROME rather than the reader's windows.
    w::MenuBarItem window_menu{
        "&Window",
        {item(u::std_command_keys::kWindowList), separator, item(commands::kMoveResize),
         item(u::std_command_keys::kZoom), item(u::std_command_keys::kMinimize), separator,
         item(u::std_command_keys::kTileHorizontally), item(u::std_command_keys::kTileVertically),
         item(u::std_command_keys::kTileGrid), item(u::std_command_keys::kCascade), separator,
         item(commands::kToggleStatusBar), item(commands::kFitDesktop)}};
    // The interface spec: settings dialogs collect under a top-level
    // Settings menu rather than hiding one per feature area — and the colour
    // theme is a setting, which is why there is no View menu holding three
    // themes of its own. It was one: three menu items that changed the theme
    // for the session and left the file saying something else, so a reader who
    // liked Light had to find it again every morning.
    // "Sett&ings", not "&Settings": the bar already answers Alt+S with Session,
    // and a second S here is a menu a reader cannot reach by its own letter —
    // silently, because the first one simply opens instead.
    w::MenuBarItem settings_menu{"Sett&ings", {item(commands::kSettings)}};
    // The terminal report sits where cksetup keeps it: in Help, beside the
    // other "what is this thing doing" answers. It reports the OUTER host —
    // what ckmux itself detected, Sixel included — which is the report a
    // reader wants when a picture drawn inside a ckmux terminal fails to
    // appear.
    w::MenuBarItem help_menu{"&Help",
                             {item(commands::kKeyReference), item(commands::kAllKeys),
                              item(u::std_command_keys::kTerminalReport), separator,
                              item(commands::kAbout)}};

    std::vector<w::MenuBarItem> menus;
    menus.push_back(std::move(session_menu));
    menus.push_back(std::move(terminal_menu));
    menus.push_back(std::move(view_menu));
    menus.push_back(std::move(window_menu));
    menus.push_back(std::move(settings_menu));
    menus.push_back(std::move(help_menu));
    return menus;
}

void ClientApp::build_chrome() {
    // The theme the constructor already put on the application, handed
    // straight back: the shell assigns whatever it is given, so naming one
    // here would quietly overrule `[general] theme` — which is how a reader
    // who chose Light got Dark every morning.
    w::ApplicationShell shell(app_, {.theme = app_.theme(),
                                     .menus = build_menus(),
                                     .status_items = {},
                                     // No status line from the shell, which
                                     // would dock one on its own: ckmux's
                                     // footer is not the bottom dock any more.
                                     // The window switcher bar sits on top of
                                     // it (WP-32) and the two are composed
                                     // below — see there for why that is a
                                     // Column rather than a second dock.
                                     .always_dock_status_line = false});
    desktop_ = &shell.desktop();
    // ckmux lists its own windows, so ckVision must not list them a second
    // time. Its default (ckVision D-064) parks a minimized window on the
    // desktop's bottom edge as a one-row stub — the right answer for an
    // application that has built no listing, and one row of duplication for
    // this one, which puts every window on the switcher bar directly below.
    // `HostListed` is D-056's plain hiding, which is what this client has
    // always been written against.
    desktop_->set_minimized_window_placement(w::MinimizedWindowPlacement::HostListed);
    // Panning follows the focused window (WP-43): a reader who switches to a
    // terminal off the visible region means to see it, and `pan_to_show`
    // moves the least it can — nothing at all for a window already in view,
    // which is every activation on a desktop whose extent equals its bounds,
    // so a client attached to a session no larger than its screen behaves
    // exactly as before. The pan moves paint offsets, never `bounds()`
    // (U7-a), so no layout report can follow from it.
    desktop_->subscribe_window_change(
        [this](w::Desktop::WindowChange change, w::Window& window) {
            if (change != w::Desktop::WindowChange::Activated) return;
            desktop_->pan_to_show(window.bounds());
        },
        alive_);
    // A terminal opened while the reader is working maximized opens maximized
    // too (U4-c), rather than cascading a small window on top of the full-
    // screen one they were reading — which is the arrangement they had just
    // said they did not want. Unconditional, and deliberately not a setting:
    // it is the state the reader themselves put the desktop in that decides,
    // so the desktop already carries the answer, and `^B z` on the new window
    // undoes it for that window alone. A `[general]` key would be a second
    // place to say the same thing, and Settings ▸ General… is at its 80×24
    // budget already.
    //
    // Dialogs are outside the policy upstream — present_modal/present_modeless
    // bypass it — so ckmux's confirmations and pickers are unaffected and no
    // guard of ckmux's own is needed here.
    desktop_->set_maximize_follows_active(true);

    // And the keyboard never stays in a window that has been put away (U4-j).
    // There are four routes to that transition and ckmux owns two of them —
    // the switcher's minimize action and its row menu, which both call
    // `focus_active_terminal()` afterwards. The `↓` on the window's own top
    // border and the standard Minimize command reach `Window::set_minimized`
    // through ckVision without ckmux hearing a word, and `focus_active_
    // terminal()` is in any case silent about the case that has no active
    // terminal to name: every terminal minimized, and the focus left sitting
    // on one that is no longer on the screen.
    //
    // Ridden off the desktop's own notification instead, which every route
    // ends in, so all four end the same way.
    desktop_->subscribe_window_change(
        [this](w::Desktop::WindowChange change, w::Window&) {
            if (change == w::Desktop::WindowChange::Minimized)
                keep_keyboard_off_hidden_terminals();
        },
        alive_);

    // The bottom chrome, in one piece: the window switcher bar over the footer
    // (WP-32 / U4-a).
    //
    // `Desktop::dock_bottom` holds exactly ONE view per edge — its own header
    // says docking a second "replaces the first as far as auto-positioning goes,
    // but does NOT remove it" — so the bar is not a second dock. The two rows
    // are composed into a `ui::Column` and the Column is what is docked. That
    // is the right answer rather than the convenient one: a Column's vertical
    // hint SUMS its children, so the reserved height comes out correct by
    // itself, and `content_area()` — hence the bounds a maximized window is
    // zoomed into, and the rect every window is clamped within — already
    // excludes both rows. Nothing here makes the bar uncoverable; the
    // arithmetic that was already there does.
    auto switcher = std::make_unique<w::WindowSwitcherBar>(*desktop_);
    switcher_ = switcher.get();
    auto status_line = std::make_unique<w::StatusLine>();
    footer_ = status_line.get();
    auto stack = std::make_unique<u::Column>();
    stack->add_item(std::move(switcher));
    stack->add_item(std::move(status_line));
    chrome_stack_ = desktop_->dock_bottom(std::move(stack));
    install_window_switcher(*switcher_);
    install_clock();
}

void ClientApp::install_window_switcher(w::WindowSwitcherBar& bar) {
    // Every provider below is held by a view the DESKTOP owns, and the desktop
    // outlives this client, so each is guarded by this client's own liveness
    // rather than trusting a bare `this` (see `alive_`).
    const std::weak_ptr<void> alive = alive_;

    // ckmux's terminal windows, and only those.
    bar.set_window_source([this, alive] {
        return alive.expired() ? std::vector<w::Window*>{} : switcher_windows();
    });
    // The caption the window already carries — which is ckmux's own numbering
    // until a child claims a title with OSC 0/2 and the child's title
    // afterwards (`open_terminal` and `refresh_terminal_titles` between them),
    // and the COPY badge while a reader is reading that window's history.
    // Read back off the window rather than re-derived, so the row and the
    // window's own title bar cannot come to disagree.
    bar.set_label_provider([](w::Window& window) { return window.title(); });
    // Click = activate and raise, which is what `Desktop::activate` is, plus
    // the focus: a reader who picks a terminal off the bar means to type into
    // it, and leaving the keyboard where it was would raise a window that then
    // ignored them.
    bar.set_activate_action([this, alive](w::Window& window) {
        if (alive.expired() || desktop_ == nullptr) return;
        desktop_->activate(&window);
        focus_active_terminal();
        refresh_footer();
    });
    // And the other half of a taskbar button (U4-j): clicking the row of the
    // terminal the reader is IN puts it away. The library decides WHICH of
    // the three transitions a click is; what ckmux adds is the same pair of
    // side effects its activate action carries, because the keyboard and the
    // footer follow the window either way — a minimize that left the focus
    // on a hidden terminal would send the reader's next keystroke nowhere.
    bar.set_minimize_action([this, alive](w::Window& window) {
        if (alive.expired() || desktop_ == nullptr) return;
        window.set_minimized(true);
        focus_active_terminal();
        refresh_footer();
    });
    bar.set_context_menu_provider([this, alive](const w::WindowSwitcherTarget& target) {
        return alive.expired() ? std::vector<w::MenuItem>{} : switcher_menu(target);
    });
    // The ▼ at the far left of the row, and what ckmux means by it (WP-35).
    // The bar hides nothing itself — it reports the toggle and answers
    // `collapsed()` — so this is the whole of the behaviour, and it is one
    // call into the state both routes share.
    bar.set_collapsible(true);
    bar.on_collapse_changed = [this, alive](bool collapsed) {
        if (alive.expired()) return;
        set_chrome_collapsed(collapsed);
    };
    bar.set_help_context_key("ckmux.switcher");

    // And the buttons stop twitching. A terminal's caption follows the program
    // in it (the interface spec), which for a shell means a rewrite at every prompt and
    // for a build tool a rewrite per target; undamped, each of those re-sizes
    // that button and slides every button after it. The two intervals are the
    // reader-facing rule — at most one widening a second, at most one
    // narrowing every half minute — and they live in ClientOptions so a test
    // can turn them off and read natural widths.
    bar.set_width_damping(options_.switcher_grow_nanos, options_.switcher_shrink_nanos);

    // And minimizing SHOWS where the window went (U4-k): the frame shrinks and
    // flies to the row it will live in, and back out of it on restore.
    //
    // ckVision cannot know this by itself — a Desktop does not know what lists
    // its windows — so the destination is a provider, and this is ckmux's
    // answer: the entry that stands for that window on the bar. `nullopt` for
    // a window the bar is not currently showing an entry for, which is the
    // ordinary case rather than an error: the row is hidden with one terminal,
    // and a window on another page of a paged strip has no columns at all.
    // No answer, no flight.
    if (desktop_ != nullptr) {
        w::WindowSwitcherBar* const row = &bar;
        desktop_->set_minimize_target_provider(
            [this, alive, row](w::Window& window) -> std::optional<ckv::Rect> {
                if (alive.expired() || desktop_ == nullptr || !row->visible())
                    return std::nullopt;
                for (const w::WindowSwitcherBar::DrawnEntry& drawn : row->drawn_entries()) {
                    if (drawn.index >= row->entries().size()) continue;
                    if (row->entries()[drawn.index].window != &window) continue;
                    // Into the frame `Window::bounds()` is measured in. The
                    // bar answers in its own local columns and sits inside a
                    // docked Column, so neither its own origin nor the
                    // desktop's can be assumed to be zero.
                    const ckv::Rect bar_area = row->absolute_bounds();
                    const ckv::Rect desktop_area = desktop_->absolute_bounds();
                    return ckv::Rect{bar_area.x + drawn.x - desktop_area.x,
                                     bar_area.y - desktop_area.y, drawn.width, 1};
                }
                return std::nullopt;
            });
    }

    // The bar keeps its own list current through the desktop's window-change
    // notification — its `Desktop` constructor subscribed for us, which is why
    // nothing here polls. This second subscription answers ckmux's own
    // question, whether the row is there AT ALL, and rides the same signal for
    // the same reason: nothing invalidates a view that lists windows it does
    // not contain. The weak-lifetime overload drops it when this client goes.
    if (desktop_ != nullptr)
        desktop_->subscribe_window_change(
            [this](w::Desktop::WindowChange, w::Window&) { sync_window_switcher(); }, alive_);
    sync_window_switcher();
}

std::vector<ckv::widgets::Window*> ClientApp::switcher_windows() const {
    std::vector<w::Window*> listed;
    if (desktop_ == nullptr) return listed;
    listed.reserve(terminal_windows_.size());
    // Asked of the DESKTOP rather than of `terminal_windows_`, and in the
    // desktop's own insertion order: that is the order `^B n` cycles and the
    // order the captions are numbered in, so the bar reads left to right the
    // way the keyboard walks. It also keeps a window that has already left the
    // desktop out of the list while the map still names it — `forget_terminals`
    // removes the windows one at a time and clears the map afterwards, and each
    // removal notifies while the rest are still in there.
    for (w::Window* window : desktop_->windows())
        if (terminal_shown_by(window) != nullptr) listed.push_back(window);
    return listed;
}

std::vector<ckv::widgets::MenuItem> ClientApp::switcher_menu(
    const w::WindowSwitcherTarget& target) {
    std::vector<w::MenuItem> items;
    w::Window* const window = target.window();
    if (window == nullptr || desktop_ == nullptr) return items;
    const std::weak_ptr<void> alive = alive_;

    // EVERY row is bound to the window whose row was clicked, through
    // `WindowSwitcherTarget::bind` — never wired as `MenuItem::command`. Every
    // standard window command reads `active_window()`, which when a reader
    // right-clicks a BACKGROUND row is by definition the window they did not
    // point at: "Close" there would close the foreground terminal, silently
    // and destructively. That is the whole reason the target exists (U4-a), and
    // `bind` also makes a window that ends while the menu stands open a no-op
    // rather than a reference to freed storage.
    // Minimize first, and Maximize under it: that is the order the two
    // controls sit in on the window's own frame, and a menu that reversed
    // them would be describing a different window (WP-34).
    //
    // "&Show" rather than a second "&Restore": a window minimized while
    // maximized has a Restore on the row below — the one that un-maximizes
    // it — and two items reading Restore in one menu is a reader choosing
    // between them by guessing. A terminal that draws no `_` control gets no
    // row at all rather than a disabled one, because the reason is not a
    // state that will pass.
    if (window->minimizable())
        items.push_back(
            w::MenuItem::action(window->minimized() ? "&Show" : "Mi&nimize",
                                target.bind([this, alive](w::Window& clicked) {
                                    if (alive.expired() || desktop_ == nullptr) return;
                                    if (clicked.minimized()) {
                                        // Named from a list is asked for: it
                                        // comes back in front, the way a click
                                        // on its row does.
                                        desktop_->activate(&clicked);
                                    } else {
                                        clicked.set_minimized(true);
                                    }
                                    focus_active_terminal();
                                    refresh_footer();
                                }))
                .with_help("ckmux.switcher"));
    items.push_back(w::MenuItem::action(window->zoomed() ? "&Restore" : "Ma&ximize",
                                        target.bind([this, alive](w::Window& clicked) {
                                            if (alive.expired() || desktop_ == nullptr) return;
                                            clicked.toggle_zoom(desktop_->content_area());
                                        }))
                        .with_help("ckmux.switcher"));
    items.push_back(w::MenuItem::action("&Move / Resize",
                                        target.bind([this, alive](w::Window& clicked) {
                                            if (alive.expired() || desktop_ == nullptr) return;
                                            // Raised first, unlike the two
                                            // above: keyboard move mode is a
                                            // reader steering a window with the
                                            // arrow keys, and one still behind
                                            // another would slide about out of
                                            // sight.
                                            desktop_->activate(&clicked);
                                            focus_active_terminal();
                                            clicked.enter_move_mode();
                                        }))
                        .with_help("ckmux.switcher"));
    items.push_back(w::MenuItem::separator());

    // Naming the window is the reader's, and it is available wherever a
    // terminal is: unlike Move to session… below, it needs nothing outside
    // this desktop. `n` rather than `m`, which Move / Resize already holds in
    // this menu.
    items.push_back(w::MenuItem::action("Re&name…", target.bind([this, alive](w::Window& clicked) {
                                            if (alive.expired()) return;
                                            show_rename_terminal_dialog(
                                                terminal_shown_by(&clicked));
                                        }))
                        .with_help("ckmux.switcher"));
    items.push_back(w::MenuItem::separator());

    // Same availability as `Terminal ▸ Move terminal…`, and stated the same
    // way: somewhere else for the program to keep running requires there to BE
    // a somewhere else, which M1's single process is not.
    const bool can_move = static_cast<bool>(options_.move_terminal) && attached_session_ != 0;
    w::MenuItem move_out = w::MenuItem::action("Move to &session…",
                                               target.bind([this, alive](w::Window& clicked) {
                                                   if (alive.expired()) return;
                                                   show_move_terminal_dialog(
                                                       terminal_shown_by(&clicked));
                                               }));
    if (!can_move)
        move_out = move_out.with_enabled(false).with_disabled_reason(
            "this terminal is not in a server session");
    items.push_back(move_out.with_help("ckmux.switcher"));
    items.push_back(w::MenuItem::separator());

    // `close()`, not `remove_window()`: the window's own `close_request` is
    // what puts the confirmation in front of the reader, and being asked about
    // the terminal they pointed at is the entire point of this menu.
    items.push_back(
        w::MenuItem::action("&Close", target.bind([](w::Window& clicked) { clicked.close(); }))
            .with_help("ckmux.switcher"));
    return items;
}

void ClientApp::sync_window_switcher() {
    if (switcher_ == nullptr || chrome_stack_ == nullptr || desktop_ == nullptr) return;
    // "When more than one window is open", read literally. With one terminal
    // there is nothing to switch BETWEEN, and the row would spend a line of the
    // reader's desktop repeating what that window's own title bar already says.
    // On an 80×24 terminal that line is one of the 22 a menu bar and a footer
    // leave — the exact budget `Settings ▸ General…` is drawn to fit
    // (the interface spec), which it therefore still gets.
    //
    // …with a second clause, which is the reader's own report (WP-34): OR any
    // terminal is minimized. One window, hidden, and no bar is a reader
    // looking at an empty desktop with their program still running somewhere
    // they cannot name — the bar is the only route back to a minimized
    // window, so the one case where hiding it strands them is exactly the
    // case where it must appear. Asked of the WINDOWS rather than tracked
    // alongside them, for the reason every other question here is: the
    // desktop already knows, and a second record could disagree with it.
    const std::vector<w::Window*> listed = switcher_windows();
    const bool any_minimized = std::any_of(listed.begin(), listed.end(), [](const w::Window* w) {
        return w != nullptr && w->minimized();
    });
    const bool wanted = listed.size() > 1U || any_minimized;
    if (switcher_->visible() == wanted) return;
    switcher_->set_visible(wanted);
    // And the desktop has to be TOLD. `content_area()` reads the docked view's
    // hint live, so it answers correctly the moment the row goes — but nothing
    // re-runs the pass that re-places the dock, re-clamps every window and
    // re-fills the zoomed ones, because `View::set_visible` invalidates without
    // raising `size_hint_changed()` even though a container's aggregate hint
    // skips invisible children. This is exactly the docked-view-changed-height
    // path ckVision already owns and already exposes
    // (`Desktop::on_child_size_hint_changed` → `on_resized`), so this is the
    // library's own seam called by hand rather than geometry recomputed here —
    // the one line missing upstream is `View::set_visible` raising the change,
    // which is a ckVision fix to make, not a rule for ckmux to keep.
    desktop_->on_child_size_hint_changed(*chrome_stack_);
}

void ClientApp::report_detached(proto::DetachReason reason, const std::string& text) {
    // Persistent, all three of them. A reader who stepped away and came back
    // to an empty ckmux is exactly the person this is for, and a line that had
    // already faded would leave them the empty desktop with no account of it.
    const std::string& name = attached_session_name_;
    const std::string what = text.empty() ? std::string("detached") : text;
    std::string line;
    switch (reason) {
        case proto::DetachReason::Takeover:
            line = name.empty() ? "This session was " + what : "'" + name + "' was " + what;
            break;
        case proto::DetachReason::SessionKilled:
            line = name.empty() ? "This session ended: " + what : "'" + name + "' ended: " + what;
            break;
        case proto::DetachReason::ServerShutdown:
            line = "The ckmux server stopped: " + what;
            break;
        case proto::DetachReason::User:
            return;
    }
    notify(std::move(line), w::NotificationSeverity::Warning, /*persistent=*/true);
}

int ClientApp::readers_here() const {
    if (attached_session_ == 0) return 0;
    for (const SessionRow& row : last_sessions_)
        if (row.id == attached_session_) return row.readers;
    // Not in the list yet — an attach whose `SessionList` has not arrived. One
    // rather than zero: this client is attached, so it is a reader, and
    // answering zero would hide the footer count and grey the menu items for
    // the moment between attaching and being told about it.
    return 1;
}

void ClientApp::set_reader_mode(proto::AttachMode mode, bool told) {
    const bool was_watching = watching();
    reader_mode_ = mode;
    // The footer carries "read-only" while it is true, because a toast expires
    // and the fact does not.
    refresh_footer();
    if (!told) return;
    // Somebody ELSE did this, so it needs saying. A reader who ticked the box
    // themselves is not told, which is why `told` is a parameter rather than a
    // comparison: the two calls are indistinguishable from the state alone.
    if (mode == proto::AttachMode::Watch)
        notify("Another reader made this session read-only for you",
               w::NotificationSeverity::Warning);
    else if (was_watching)
        notify("You can type in this session again", w::NotificationSeverity::Info);
}

bool ClientApp::session_shows_attached(std::uint64_t id) const {
    for (const SessionRow& row : last_sessions_)
        if (row.id == id) return row.readers > 0;
    return false;
}

void ClientApp::notify(std::string text, w::NotificationSeverity severity, bool persistent) {
    if (desktop_ == nullptr || text.empty()) return;
    if (toasts_ == nullptr) {
        // Made on demand, exactly as the prefix overlay is, and for the same
        // reason: a surface that is only ever needed occasionally should not
        // be a permanent child that every layout pass has to think about.
        auto centre = std::make_unique<w::NotificationCenter>();
        centre->set_auto_dismiss(options_.toast_nanos);
        // Not a focus stop. ckVision's default makes one, which is right for
        // a centre a reader Tabs to in a form; here it would put the reader's
        // Tab in a message about something that already happened instead of
        // in the program they are typing into.
        centre->set_focus_policy(u::FocusPolicy::None);
        toasts_ = desktop_->add_popup(std::move(centre));
        // Re-placed on every change, including the ones this client did not
        // make: a toast expires on ckVision's timer, and nothing else would
        // tell us the surface had shrunk or emptied.
        const std::weak_ptr<void> alive = alive_;
        toasts_->on_changed = [this, alive] {
            if (alive.expired()) return;
            // POSTED, not called. This runs from inside the centre — a click
            // that dismissed a line, or an expiry firing on its timer — and
            // the last notification going means this surface is taken down.
            // Destroying a view from inside its own handler frees the object
            // whose stack we are standing on; the prefix overlay unwinds
            // first for exactly this reason.
            app_.post([this, alive] {
                if (alive.expired()) return;
                place_notifications();
            });
        };
    }
    toasts_->add(w::Notification{severity, std::move(text), persistent});
    place_notifications();
}

void ClientApp::place_notifications() {
    if (toasts_ == nullptr || desktop_ == nullptr) return;
    // Nothing left to say: the surface goes rather than sitting there as an
    // empty popup that still takes clicks over the reader's windows.
    if (toasts_->notifications().empty()) {
        w::NotificationCenter* const going = toasts_;
        toasts_ = nullptr;
        desktop_->remove_popup(going).reset();
        return;
    }
    // Top-right of the VISIBLE content — inside the chrome rather than over
    // it, so a toast never covers the window bar a reader may be reaching
    // for, and clear of the menu bar above. The view's rect, not the
    // world's (WP-43): a toast is chrome for the reader's eyes, and a world
    // top-right may be a screenful away from them.
    const ckv::Rect area = view_content_area();
    int width = 0;
    for (const w::Notification& notification : toasts_->notifications())
        width = std::max(width, ckv::text::text_width(notification.text));
    // Two cells for the severity marker the centre draws, and one of margin
    // either side so the text does not run into the desktop edge.
    width = std::clamp(width + 4, 12, std::max(12, area.width - 2));
    const int height = std::min(static_cast<int>(toasts_->notifications().size()), area.height);
    toasts_->set_bounds(ckv::Rect{area.x + area.width - width, area.y, width, height});
}

void ClientApp::set_chrome_collapsed(bool collapsed) {
    if (switcher_ == nullptr || footer_ == nullptr || chrome_stack_ == nullptr ||
        desktop_ == nullptr)
        return;
    // Asked BEFORE anything moves, because the answer is what decides
    // whether the desktop has to be told at all.
    const bool moved = footer_->visible() == collapsed;
    chrome_collapsed_ = collapsed;
    // Idempotent on purpose — see the header. Both of these are guarded
    // upstream (`View::set_visible` and `PagedStrip::set_collapsed` each
    // return early on no change), which is also what keeps the two routes
    // from chasing each other: the bar's toggle calls this, this calls the
    // bar's toggle, and the second call is the one that stops — the strip
    // assigns its flag before it reports it, so the re-entrant call finds
    // nothing left to change and notifies nobody.
    footer_->set_visible(!collapsed);
    switcher_->set_collapsed(collapsed);
    if (!moved) return;
    // The same missing library seam `sync_window_switcher` documents: a
    // docked Column's aggregate height skips an invisible child, so
    // `content_area()` already answers correctly — but nothing re-runs the
    // pass that re-places the dock and re-fills the zoomed windows, because
    // `View::set_visible` invalidates without raising `size_hint_changed()`.
    // So the desktop is told by hand, and a maximized terminal grows into the
    // freed row (and gives it back) as part of that one pass.
    desktop_->on_child_size_hint_changed(*chrome_stack_);
}

ckv::widgets::MenuBar* ClientApp::menu_bar() {
    if (desktop_ == nullptr) return nullptr;
    return dynamic_cast<w::MenuBar*>(desktop_->top_dock());
}

void ClientApp::install_clock() {
    // The clock goes at the right end of the menu bar (the interface spec): it is where
    // a reader looks for one, and it is the one permanent row with space to
    // spare — the footer's right side already carries session state. The bar
    // owns the placement and re-decides it on every resize, so the clock stays
    // at the right end rather than where the right end used to be.
    //
    // Re-run whenever the setting changes, so this is the whole of "what the
    // bar's right end should be", not an addition to it: `off` and a client
    // with no way to read the time both leave the bar as it was before there
    // was a clock, rather than leaving a stopped one behind.
    w::MenuBar* const bar = menu_bar();
    if (bar == nullptr) return;
    const ClockMode mode = options_.local_now ? options_.settings.clock : ClockMode::Off;
    if (mode == ClockMode::Off) {
        clock_ = bar->set_trailing_view(std::unique_ptr<w::ClockView>{});
        return;
    }
    if (clock_ == nullptr) {
        auto clock = std::make_unique<w::ClockView>();
        // No blinking separator, unlike ckVision's terminal example. A blinking
        // one repaints twice a second whatever else is happening, and ckmux's
        // frames may be travelling down an SSH link to a reader who is not
        // looking at the clock.
        //
        // The provider is copied rather than reached through `this`: the bar
        // outlives this client (the desktop belongs to the Application), and a
        // clock that ticks through a dangling client is a crash at shutdown.
        clock->set_time_provider([now = options_.local_now] { return now().time; });
        clock_ = bar->set_trailing_view(std::move(clock));
        // Clicking it drops a calendar out of it. The two widgets know nothing
        // about each other; this is the whole of the wiring, and the keyboard
        // reaches it for free — a trailing view is a title on the bar, so the
        // menu key and → walk onto it and Enter acts.
        clock_->on_click = [this] { open_calendar(); };
    }
    // Seconds are a repaint a second and a cell or two of width; the bar
    // re-measures its trailing view, so switching either way needs nothing
    // else said about the layout.
    clock_->set_show_seconds(mode == ClockMode::Seconds);
}

void ClientApp::open_calendar() {
    if (desktop_ == nullptr || clock_ == nullptr || !options_.local_now) return;
    // An open calendar holds the mouse, so a second click on the clock
    // dismisses it before the clock ever hears about it — the way clicking an
    // open menu title closes its menu. Reopening takes the next click.
    w::CalendarDropdown* const dropdown = w::show_calendar_dropdown(*clock_, app_, *desktop_);
    clock_->set_open(true);
    // Read through the member rather than captured: the clock this calendar
    // hangs from can be replaced or removed while it is open (Settings ▸
    // General…), and a closing calendar must not put a highlight back on a
    // clock that is gone.
    dropdown->on_closed = [this] {
        if (clock_ != nullptr) clock_->set_open(false);
    };
    // Today is asked for on every repaint rather than fixed when the calendar
    // opens: a ckmux session outlives midnight, and a calendar still marking
    // yesterday is confidently wrong about the one fact it exists to state.
    dropdown->calendar().set_today_provider(
        [now = options_.local_now] { return std::optional<w::DateValue>(now().date); });
    const w::DateValue today = options_.local_now().date;
    dropdown->show_month(today);  // the month picker and the year field follow
    dropdown->calendar().set_selected(today);
}

std::string ClientApp::suggested_session_name() const {
    // One past the largest "session-N" a reader already has, so the prompt
    // offers a name that does not collide with one they are looking at. The
    // server suggests the same way; this is what the FIELD starts at, and the
    // reader is free to type over it.
    long highest = 0;
    for (const SessionRow& row : last_sessions_) {
        if (row.name.rfind("session-", 0) != 0) continue;
        const std::string tail = row.name.substr(8);
        if (tail.empty() || tail.find_first_not_of("0123456789") != std::string::npos) continue;
        highest = std::max(highest, std::strtol(tail.c_str(), nullptr, 10));
    }
    return "session-" + std::to_string(highest + 1);
}

void ClientApp::show_new_session_dialog(std::string suggested_name) {
    w::DialogDescriptor descriptor;
    descriptor.title = "New session";
    w::FieldDescriptor name;
    name.label = "Name";
    name.kind = w::FieldKind::Text;
    name.initial_text = suggested_name.empty() ? suggested_session_name() : std::move(suggested_name);
    descriptor.fields.push_back(std::move(name));
    descriptor.buttons.push_back(w::ButtonDescriptor{"&Create session", w::ButtonRole::Accept, {}});
    descriptor.buttons.push_back(w::ButtonDescriptor{"Cancel", w::ButtonRole::Dismiss, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler([this, presentation](const w::DialogResult& result) {
        if (!result.accepted || result.values.empty()) return;
        // An empty field means "you name it": the server's own rule, rather
        // than a dialog inventing one a second way.
        if (options_.create_session) options_.create_session(result.values.front());
    });
}

void ClientApp::show_rename_session_dialog(std::string current_name) {
    w::DialogDescriptor descriptor;
    descriptor.title = "Rename session";
    w::FieldDescriptor name;
    name.label = "Name";
    name.kind = w::FieldKind::Text;
    name.initial_text = std::move(current_name);
    descriptor.fields.push_back(std::move(name));
    descriptor.buttons.push_back(w::ButtonDescriptor{"&Rename", w::ButtonRole::Accept, {}});
    descriptor.buttons.push_back(w::ButtonDescriptor{"Cancel", w::ButtonRole::Dismiss, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler([this, presentation](const w::DialogResult& result) {
        if (!result.accepted || result.values.empty() || result.values.front().empty()) return;
        if (options_.rename_session) options_.rename_session(result.values.front());
    });
}

void ClientApp::show_kill_session_dialog() {
    const std::string& name = attached_session_name_;
    int terminals = 0;
    for (const SessionRow& row : last_sessions_)
        if (row.id == attached_session_) terminals = row.terminals;

    w::DialogDescriptor descriptor;
    descriptor.title = "End session";
    w::FieldDescriptor what;
    what.kind = w::FieldKind::Note;
    what.label = "Every program in " + (name.empty() ? std::string("this session") : "\"" + name + "\"") +
                 (terminals == 1 ? " (1 terminal)" : " (" + std::to_string(terminals) + " terminals)") +
                 " will be asked to quit.";
    descriptor.fields.push_back(std::move(what));

    const std::size_t force_field = descriptor.fields.size();
    w::FieldDescriptor force;
    force.kind = w::FieldKind::Check;
    force.label = "&Kill anything still running after " +
                  std::to_string(options_.settings.kill_grace_seconds) + " seconds";
    // Ticked by default: a reader ending a session means it to end. Unticking
    // it is the deliberate choice, and the note says what that choice costs.
    force.initial_checked = true;
    descriptor.fields.push_back(std::move(force));
    w::FieldDescriptor note;
    note.kind = w::FieldKind::Note;
    note.label = "  Unticked, a program that declines to quit keeps running — and so does the "
                 "session.";
    descriptor.fields.push_back(std::move(note));
    w::FieldDescriptor where;
    where.kind = w::FieldKind::Note;
    where.label = "  The wait is Settings ▸ \"Give programs this long to quit\".";
    descriptor.fields.push_back(std::move(where));

    descriptor.buttons.push_back(w::ButtonDescriptor{"&End session", w::ButtonRole::Accept, {}});
    descriptor.buttons.push_back(w::ButtonDescriptor{"Cancel", w::ButtonRole::Dismiss, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler([this, presentation, force_field](const w::DialogResult& result) {
        if (!result.accepted || result.checked.size() <= force_field) return;
        if (options_.kill_session)
            options_.kill_session(result.checked[force_field], options_.settings.kill_grace_seconds);
    });
}

std::string session_row_label(const SessionRow& row, std::uint64_t watched) {
    std::string line = row.name.empty() ? "session " + std::to_string(row.id) : row.name;
    line += row.terminals == 1 ? " — 1 terminal"
                               : " — " + std::to_string(row.terminals) + " terminals";
    // Three states, and a reader picking a row needs to tell them apart: the
    // one they are already watching, one somebody else is watching — where
    // attaching takes it from them — and one nobody holds.
    //
    // The NUMBER, not "in use" (WP-48). The wire has carried a count since
    // WP-44 and this line could only say that a session was busy, which is the
    // one thing a reader already assumes; what they cannot guess is whether
    // joining puts them in a room with one person or three.
    if (watched != 0 && row.id == watched) {
        // This client is one of the readers the server counted, so the row it
        // holds subtracts itself rather than reporting itself as company.
        const int others = row.readers > 1 ? row.readers - 1 : 0;
        if (others == 0) line += "  (this client)";
        else if (others == 1) line += "  (this client, and 1 other reader)";
        else line += "  (this client, and " + std::to_string(others) + " other readers)";
    } else if (row.readers == 1) {
        line += "  (1 reader — attaching takes it over)";
    } else if (row.readers > 1) {
        line += "  (" + std::to_string(row.readers) + " readers — attaching takes it over)";
    }
    return line;
}

void ClientApp::show_session_picker(std::vector<SessionRow> rows) {
    last_sessions_ = rows;
    // The name of the session being watched can have changed under this client
    // — another client can rename it — and every prompt that says it reads
    // from here.
    if (attached_session_ != 0) attached_session_name_ = session_name(attached_session_);
    w::DialogDescriptor descriptor;
    descriptor.title = "Sessions";

    // The sessions, as one selectable group: a reader picks the row they mean
    // and the buttons act on it. What each row says is what they need in order
    // to choose — how much is running in it, and whether somebody else is
    // watching it, because attaching to that one takes it from them.
    w::FieldDescriptor choice;
    choice.kind = w::FieldKind::Radio;
    // No mnemonic: a group label is not a control, and ckVision renders a '&'
    // in one literally — its mnemonics live on the options themselves.
    choice.label = rows.empty() ? "" : "Session";
    for (const SessionRow& row : rows)
        choice.options.push_back(session_row_label(row, attached_session_));
    if (!rows.empty()) {
        // The one this client is already watching, if any; otherwise the first.
        choice.initial_selection = 0;
        for (std::size_t index = 0; index < rows.size(); ++index)
            if (rows[index].id == attached_session_)
                choice.initial_selection = static_cast<int>(index);
        descriptor.fields.push_back(std::move(choice));
    } else {
        w::FieldDescriptor empty;
        empty.kind = w::FieldKind::Note;
        empty.label = "No sessions are running yet. ckmux keeps programs running after you "
                      "leave — Session \u25b8 New Session\u2026 starts one.";
        descriptor.fields.push_back(std::move(empty));
    }

    // And, when there is somebody to arrive AMONG, what arriving should do to
    // them (WP-50). Three answers rather than two, because the third — join and
    // only watch — is what makes sharing comfortable and is not a modifier of
    // either other one.
    //
    // A radio group rather than a Join button beside a Take Over button, and
    // the reason is the widget contract rather than taste: a dialog has at most
    // one Accept, and a second acting button is a `Dismiss` whose handler runs
    // with no access to the fields — so it could not know which session row was
    // selected. The property that mattered survives the change intact, and
    // arguably reads better: the destructive answer must be deliberately
    // chosen, it is never the default, and all three say what they do.
    //
    // Offered only when somebody other than this client is actually watching
    // something. On the ordinary machine — one reader, several sessions —
    // there is nobody to displace and no question to ask.
    bool anybody_else_is_watching = false;
    for (const SessionRow& row : rows) {
        const int others = row.id == attached_session_ ? row.readers - 1 : row.readers;
        if (others > 0) anybody_else_is_watching = true;
    }
    mode_choice_offered_ = anybody_else_is_watching;
    if (anybody_else_is_watching) {
        w::FieldDescriptor mode;
        mode.kind = w::FieldKind::Radio;
        mode.label = "If somebody is already watching it";
        mode.options.push_back("&Join them — you both see one session");
        mode.options.push_back("Take it &over — the others are dropped to their pickers");
        mode.options.push_back("Join and only &watch — nothing you type reaches it");
        // Joining is the default because it is the answer that costs a
        // colleague nothing. Taking over stays one keystroke away, which is
        // D-07: no reader can be locked out of their own session.
        mode.initial_selection = 0;
        descriptor.fields.push_back(std::move(mode));
    }

    // Attaching to a different session is a detach and an attach, which is what
    // "switch" means here (the session model): the windows this client is showing belong
    // to the session it is leaving.
    if (!rows.empty()) {
        std::vector<std::uint64_t> ids;
        ids.reserve(rows.size());
        for (const SessionRow& row : rows) ids.push_back(row.id);
        descriptor.buttons.push_back(w::ButtonDescriptor{"&Attach", w::ButtonRole::Accept, {}});
        attach_choice_ = std::move(ids);
    }
    // What dismissing MEANS depends on where this client stands. Attached, it is
    // "leave things as they are". With no session — at startup, or after one was
    // taken away — it is a reader choosing to carry on without one, which is a
    // real answer to the question and deserves to be named.
    descriptor.buttons.push_back(w::ButtonDescriptor{
        attached_session_ == 0 ? "&Without a session" : "&Cancel", w::ButtonRole::Dismiss, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler([this, presentation](const w::DialogResult& result) {
        if (!result.accepted || result.selected.empty()) return;
        const int index = result.selected.front();
        if (index < 0 || static_cast<std::size_t>(index) >= attach_choice_.size()) return;
        // `selected` is parallel to the FIELDS, so the mode is at index 1 — and
        // only when it was offered. Read by position rather than searched for,
        // which is safe here for the reason the session row is: this dialog
        // builds its own fields a dozen lines above, in this function.
        //
        // With no group offered there is nobody to displace, so take-over and
        // join differ in nothing — but WATCHING is a real difference, and a
        // reader who is watching and switches sessions was never asked whether
        // they wanted to stop. Preserved rather than reset, so the picker
        // cannot quietly promote a watcher into somebody who can type.
        proto::AttachMode mode =
            watching() ? proto::AttachMode::Watch : proto::AttachMode::TakeOver;
        if (mode_choice_offered_ && result.selected.size() > 1) {
            switch (result.selected[1]) {
                case 0: mode = proto::AttachMode::Join; break;
                case 1: mode = proto::AttachMode::TakeOver; break;
                case 2: mode = proto::AttachMode::Watch; break;
                default: break;
            }
        }
        if (options_.attach_to_session)
            options_.attach_to_session(attach_choice_[static_cast<std::size_t>(index)], mode);
    });
}

void ClientApp::forget_terminals() {
    if (desktop_ == nullptr) return;
    // Copy mode is a surface over a window that is about to stop existing.
    if (copy_mode_ != nullptr) leave_copy_mode();
    forgetting_terminals_ = true;
    std::vector<w::Window*> going;
    going.reserve(terminal_windows_.size());
    for (const auto& entry : terminal_windows_) going.push_back(entry.second);
    // Removed rather than closed: a close asks the window whether it may go,
    // and a window with a live program in it says no and puts a question in
    // front of the reader about a program that is not being ended at all.
    for (w::Window* window : going) (void)desktop_->remove_window(window);
    terminal_windows_.clear();
    titles_.clear();
    forgetting_terminals_ = false;
    // The world went with the session (WP-43): back to a desktop that is its
    // own viewport, at its own top-left, exactly as before any attach.
    desktop_->set_extent(ckv::Size{0, 0});
    desktop_->set_pan(ckv::Point{0, 0});
    // The numbering starts over with the session: "Terminal 1" is the first
    // terminal of what a reader is looking at now.
    next_terminal_number_ = 1;
    // And so does what this client has reported about an arrangement (WP-29).
    // Every window it was describing belonged to the session being left; the
    // next arrangement is a different session's, and comparing the two would be
    // comparing places in two different stacks.
    layout_seen_.clear();
    layout_reported_.clear();
    // And which windows the server had already placed (WP-30). The terminals
    // this held are gone from the desktop, and attaching again — to this
    // session or another — is a reattach, which is precisely the moment a
    // stored arrangement is meant to be laid back down.
    layout_settled_.clear();
    refresh_footer();
}

void ClientApp::prune_pending_dialogs() {
    // Called when another dialog is opened, which is the one moment no
    // completion handler is running: an answered dialog's handler holds its own
    // reference to the presentation while it runs (that is what the captures in
    // every handler below are for), so dropping the vector's reference here can
    // never free something mid-call.
    std::vector<PendingDialog> live;
    live.reserve(pending_dialogs_.size());
    for (PendingDialog& pending : pending_dialogs_)
        if (!pending.completed || !pending.completed()) live.push_back(std::move(pending));
    pending_dialogs_ = std::move(live);
}

void ClientApp::close_window_for_terminal(const ckv::term::TerminalSubsession& terminal) {
    const auto found = terminal_windows_.find(&terminal);
    if (found == terminal_windows_.end() || desktop_ == nullptr) return;
    w::Window* const window = found->second;
    // Copy mode is a surface over THIS window's history, and the window is
    // about to stop existing. `forget_terminals` has always taken it down
    // first; this path did not, and left the surface standing over a destroyed
    // window with the title poll writing a caption through the pointer several
    // times a second.
    if (copy_mode_ != nullptr && copy_mode_window_ == window) leave_copy_mode();
    // The terminal is already gone, so nothing is asked and nothing is sent:
    // this window is being taken down because the server said its terminal
    // ended, and asking the server to end it again would be an error frame.
    forgetting_terminals_ = true;
    terminal_windows_.erase(found);
    // And this terminal stops being one whose place the server has settled
    // (WP-30). An address, once freed, is one the next mirror may be allocated
    // at, and a stale entry would tell `apply_layout` that a terminal it has
    // never seen already belongs to the reader.
    layout_settled_.erase(&terminal);
    latest_stats_.erase(&terminal);
    local_cpu_.erase(&terminal);
    titles_.erase(window);
    (void)desktop_->remove_window(window);
    forgetting_terminals_ = false;
    focus_active_terminal();
    refresh_footer();
}

void ClientApp::remember_sessions(std::vector<SessionRow> rows) {
    const int before = readers_here();
    last_sessions_ = std::move(rows);
    if (attached_session_ != 0) attached_session_name_ = session_name(attached_session_);
    const int now = readers_here();
    if (now == before) return;

    // Somebody arrived or left, and the reader needs telling: nothing in the
    // grid says so, and what they may do — and who else may type into what they
    // are looking at — has just changed. WP-48 is what makes this reachable at
    // all; before it, an attach and a detach were the two events that did NOT
    // refresh this count.
    //
    // Only for the session this client is in. A reader joining some other
    // session on this machine is news about a row in a picker, not about the
    // terminal in front of them.
    if (attached_session_ != 0 && before > 0) {
        if (now > before)
            notify(now == 2 ? "Another reader joined this session"
                            : "Another reader joined — " + std::to_string(now) + " are watching");
        else if (now == 1)
            notify("You have this session to yourself again");
    }
    // A box describing company that has gone is a box describing nothing, and a
    // reader would have to reason about it to discover that. Cleared rather
    // than left ticked — and the server has already forgotten it too, since the
    // readers it applied to are the ones that left.
    if (now <= 1 && others_read_only_) {
        others_read_only_ = false;
        refresh_menu_marks();
    }
    // The footer carries the count while there is one to carry.
    refresh_footer();
}

std::string ClientApp::session_name(std::uint64_t id) const {
    for (const SessionRow& row : last_sessions_)
        if (row.id == id) return row.name;
    return {};
}

void ClientApp::set_attached_session(std::uint64_t id, std::string name) {
    attached_session_ = id;
    attached_session_name_ = std::move(name);
    // The keyboard follows the session. Posted rather than done here because
    // this usually runs while a dialog is closing, and a dialog restores focus
    // to whatever had it when it opened — the desktop, when a reader attached
    // from the picker, which left every keystroke going nowhere.
    if (id != 0) app_.post([this] {
        focus_active_terminal();
        refresh_footer();
    });
    // Nothing to notify: the registry asks each command's predicate when it
    // needs the answer, so the menu is right the next time it is drawn.
}

ckv::widgets::Window* ClientApp::new_terminal() {
    // Attached to a server, this is a request rather than an act: the window is
    // opened when the server says the terminal exists (the session model). Locally it is
    // both at once, which is what M1 is.
    if (options_.request_new_terminal) {
        (void)options_.request_new_terminal();
        return nullptr;
    }
    return open_terminal({});
}

ckv::widgets::Window* ClientApp::open_terminal(std::string title) {
    // The numbering lives HERE, in the one place a terminal window is made.
    // It used to live in `new_terminal()`, which an attached client never
    // reaches — it asks the server instead — so every remote window was called
    // "Terminal" and every one of them cascaded to the same spot, landing
    // exactly on top of the last. Two terminals looked like one.
    const int number = next_terminal_number_++;
    if (title.empty()) title = "Terminal " + std::to_string(number);
    const std::string fallback_title = title;
    auto window = std::make_unique<w::Window>(std::move(title));
    // Cascade from the top-left so a second terminal never lands exactly on
    // the first. Full placement policy is a session-model concern (M2).
    //
    // The VIEW's top-left, not the world's (WP-43): with a session desktop
    // larger than this client's screen, the world's origin may be a screenful
    // from where the reader is looking, and a terminal opened there would be
    // born invisible — and sized to the world, it would be larger than the
    // screen of the very reader who asked for it. Where no extent is set this
    // IS content_area(), unchanged.
    const ckv::Rect area = view_content_area();
    const int offset = static_cast<int>((number - 1) % 6) * 2;
    window->set_bounds(ckv::Rect{area.x + 2 + offset, area.y + 1 + offset,
                                 std::max(20, area.width - 8 - offset),
                                 std::max(6, area.height - 4 - offset)});

    const ckm::ShellLaunch shell = ckm::shell_launch(options_.settings.shell, options_.settings.login_shell);
    ckv::term::TerminalLaunchSpec launch =
        ckv::term::TerminalLaunchSpec::program(shell.executable, shell.arguments);
    // Bounded, and this is a serverless terminal running the READER'S OWN
    // login shell -- the case ckVision's unbounded policy cannot close. An
    // interactive shell ignores SIGTERM by design, so under `WaitForExit` a
    // single terminal whose shell declined to go could hold the client in its
    // destructor for as long as that shell felt like living. The server side
    // has always named this policy (`terminals.cpp`); the local path took the
    // library default and so inherited an unbounded close.
    launch.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
    launch.argv0 = shell.argv0;
    launch.profile = ckv::term::embedded_xterm_sixel_profile();
    // Let the child name its own window (OSC 0 and OSC 2). This is what makes
    // a caption say "vim: the architecture spec" instead of "Terminal 2".
    // The emulator neutralises control bytes in that text before it reaches
    // us, and its length is bounded there too.
    launch.profile.osc_policy = ckv::core::TerminalOscPolicy::StoreMetadata;
    // What the reader's configuration says this terminal can do (the configuration spec).
    // Told to the child through the profile rather than enforced afterwards:
    // a program asks a terminal what it supports and then behaves as though
    // the answer were true, so the answer has to be the setting.
    launch.profile.mouse_reporting = options_.settings.mouse;
    launch.profile.alternate_scroll = options_.settings.alternate_scroll;
    launch.profile.sixel = options_.settings.sixel == SixelMode::Auto;
    launch.profile.clipboard_policy = options_.settings.osc52
                                          ? ckv::core::TerminalClipboardPolicy::AllowWrite
                                          : ckv::core::TerminalClipboardPolicy::Deny;
    launch.environment = options_.environment;
    // `term = auto` leaves the host's own answer (default_environment()'s
    // xterm-256color); anything else is what the reader named, verbatim,
    // because a reader who names a terminfo entry has a reason.
    if (options_.settings.term != "auto") {
        for (std::pair<std::string, std::string>& variable : launch.environment)
            if (variable.first == "TERM") variable.second = options_.settings.term;
    }
    // A shell opens where a shell opens. Left at the spec's default a
    // terminal starts in "/", which is nobody's working directory.
    if (!options_.working_directory.empty()) launch.working_directory = options_.working_directory;
    // The largest picture this terminal will hold, which the reader can raise
    // (Settings ▸ [terminal] sixel-max-megapixels). It bounds the picture, not
    // the window: a picture too large for the window is cut off at its edge.
    ckv::term::TerminalSubsessionOptions subsession;
    subsession.max_image_pixels =
        static_cast<std::size_t>(options_.settings.sixel_max_megapixels) * 1024U * 1024U;
    // How much of what scrolls off the top this terminal keeps ([general]
    // scrollback). Zero is honoured as "remember nothing" rather than as the
    // smallest number — see ckVision's max_scrollback_lines (U0-c).
    subsession.max_scrollback_lines = static_cast<std::size_t>(options_.settings.scrollback);
    // The one line that differs between a terminal in this process and a
    // terminal in a server. Everything after it — the view, the prefix, the
    // selection, the close confirmation, the banner — is the same code either
    // way, which is what "the UI drives remote terminals unchanged" has to mean
    // to be worth claiming (the work queue WP-5).
    ckv::term::TerminalSubsession& session = [&]() -> ckv::term::TerminalSubsession& {
        if (options_.terminal_source)
            return options_.terminal_source(
                TerminalRequest{std::move(launch), subsession, fallback_title});
        return app_.launch_terminal_subsession(std::move(launch), subsession);
    }();

    auto view = std::make_unique<w::TerminalView>(session);
    w::TerminalView* const terminal_view = view.get();
    view->set_bounds(window->content_rect());
    view->set_help_context_key("ckmux.terminal");
    // The prefix rides on ckVision's parent-escape hook: the one key the
    // terminal view does not hand to the child.
    view->set_parent_escape(options_.settings.prefix);
    view->on_parent_escape = [this] { arm_prefix(); };
    view->on_selection_copy = [this](std::string text) { app_.set_clipboard_text(std::move(text)); };
    // The child asked to put text on the clipboard with OSC 52, and the policy
    // this terminal was opened with allows it ([terminal] osc52). It goes
    // wherever a yank from copy mode goes — the reader configured those targets
    // for their clipboard, not for one way of filling it — and this is the
    // handler that was missing entirely: the policy said yes to the child and
    // the text was then dropped on the floor (M-R1).
    //
    // Empty is the ordinary reattach: a snapshot restores the clipboard
    // WATERMARK without the text, so a view built over a mirror that already
    // holds one compares its own zero against it and asks once, with nothing to
    // give. `copy_to_targets` returns on empty, which is what makes a reattach
    // leave a reader's clipboard alone.
    view->on_clipboard_write = [this](std::string text) { copy_to_targets(std::move(text)); };
    window->set_content(std::move(view));
    // The scrollbar on the frame (ckVision D-051): on screen exactly while
    // the terminal is on its primary screen with history off the top, costing
    // the terminal neither a column nor a reflow. The wheel, PageUp and the
    // bar all move the same view.
    w::attach_terminal_scrollbar(*window, *terminal_view);

    w::Window* const terminal_window = window.get();
    ckv::term::TerminalSubsession* const terminal_session = &session;
    // The two keys a held window offers (the interface spec), claimed through
    // ckVision's `on_key_after_exit` — which exists because a `TerminalView`
    // writes what it is given to its child, and a child that has exited is not
    // there to receive it. Consulted only once the subsession has ended, so a
    // live terminal's keys are still entirely its own.
    //
    // Both are offered only where something can honour them: `request_respawn`
    // is unset in a client with no server, and the footer asks the same
    // question before printing the hint, so the advertisement and the binding
    // cannot come to disagree.
    const std::weak_ptr<void> alive_for_keys = alive_;
    terminal_view->on_key_after_exit = [this, alive_for_keys, terminal_session,
                                        terminal_window](const ckv::KeyEvent& event) {
        if (alive_for_keys.expired()) return false;
        if (event.action != ckv::KeyAction::Press) return false;
        if (event.chord.key == ckv::Key::Enter) {
            if (!options_.request_respawn) return false;
            options_.request_respawn(*terminal_session);
            return true;
        }
        if (event.chord.key == ckv::Key::Char && event.chord.modifiers == ckv::Modifier::None &&
            (event.chord.text == "x" || event.chord.text == "X")) {
            // Through the window's own close, so a held window leaves by
            // exactly the path the frame's × uses; `close_request` lets it go
            // without a confirmation because the child is already gone.
            terminal_window->close();
            return true;
        }
        return false;
    };

    window->close_request = [this, terminal_window] {
        if (!child_is_live(terminal_window)) return true;
        // [general] confirm-kill = false means the reader has said they do
        // not want to be asked. Honoured here rather than by suppressing the
        // dialog later, so "no confirmation" really is no round trip.
        if (!options_.settings.confirm_kill) return true;
        // The address AND the identity: an answer given about a window that has
        // since been destroyed must not be handed to whatever the allocator
        // put at that address next, which would be a terminal closing without
        // ever asking its reader.
        if (confirmed_close_ == terminal_window && !confirmed_close_alive_.expired()) {
            confirmed_close_ = nullptr;
            confirmed_close_alive_.reset();
            return true;
        }
        confirm_then_close(terminal_window);
        return false;  // vetoed for now; the confirmation decides
    };
    terminal_windows_[terminal_session] = terminal_window;
    window->on_closed = [this, terminal_window, terminal_session] {
        // Ended, unless this client is merely leaving the session — in which
        // case the terminal is somebody's to go back to and closing it here
        // would kill the very programs the session exists to keep.
        if (!forgetting_terminals_) terminal_session->close();
        terminal_windows_.erase(terminal_session);
        // Its place is nobody's to remember once the window is gone — and the
        // address may be a different terminal's tomorrow (WP-30).
        layout_settled_.erase(terminal_session);
        latest_stats_.erase(terminal_session);
        local_cpu_.erase(terminal_session);
        titles_.erase(terminal_window);
        w::schedule_self_detach(*terminal_window, app_);
        // Focus and the footer both follow whatever the desktop activates
        // next, once the detach has actually happened.
        app_.post([this] {
            focus_active_terminal();
            refresh_footer();
        });
    };
    // Named rather than positional: this initialiser has silently shifted a
    // field into the wrong slot twice as the struct grew (`badge`, then `lit`),
    // and each time it compiled or failed somewhere unrelated.
    titles_[terminal_window] = TerminalTitle{.fallback = fallback_title, .number = number};
    desktop_->add_window(std::move(window));
    app_.set_focus(terminal_view);
    refresh_footer();
    return terminal_window;
}

void ClientApp::confirm_then_kill(w::Window* window) {
    if (window == nullptr || desktop_ == nullptr || !options_.kill_terminal_in_session) return;
    ckv::term::TerminalSubsession* const terminal = terminal_shown_by(window);
    if (terminal == nullptr) return;

    // Deliberately NOT the close dialog with different words. That one is a
    // negotiation — ask the program, kill it after a grace, or move it
    // somewhere it keeps running — and every option it offers is a way to end
    // well. This dialog has no such options, because the operation has none,
    // and offering a checkbox here would suggest a middle course that does not
    // exist.
    w::DialogDescriptor descriptor;
    descriptor.title = "Kill terminal";
    w::FieldDescriptor what;
    what.kind = w::FieldKind::Note;
    what.label = "Kill \"" + window->title() + "\" and everything running in it, now.";
    descriptor.fields.push_back(std::move(what));
    w::FieldDescriptor how;
    how.kind = w::FieldKind::Note;
    // The mechanism, in the reader's terms rather than the kernel's: what
    // matters to them is that nothing gets a chance to save, and that this is
    // not the thing Close does.
    how.label = "  The program is not asked and gets no time to save. Close asks first.";
    descriptor.fields.push_back(std::move(how));
    if (terminal_windows_.size() == 1 && options_.settings.kill_empty_session) {
        // The same consequence the close dialog surfaces, for the same reason:
        // a reader cannot see it from here, and it is larger than what they
        // think they are doing.
        w::FieldDescriptor last;
        last.kind = w::FieldKind::Note;
        const std::string& name = attached_session_name_;
        last.label = "  This is the last terminal: killing it ends " +
                     (name.empty() ? std::string("the session") : "session \"" + name + "\"") + ".";
        descriptor.fields.push_back(std::move(last));
    }

    // Cancel FIRST and Kill second, which is the opposite order to the close
    // dialog and the opposite for a reason: the default button is the one a
    // reader hits by pressing Enter on a dialog they have not finished
    // reading, and on this dialog that must be the harmless one.
    descriptor.buttons.push_back(w::ButtonDescriptor{"Cancel", w::ButtonRole::Dismiss, {}});
    descriptor.buttons.push_back(w::ButtonDescriptor{"&Kill terminal", w::ButtonRole::Accept, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler([this, presentation, terminal](
                                             const w::DialogResult& result) {
        if (!result.accepted) return;
        // Asked of the server, not done here. The window stays until the
        // server says the terminal ended — a window that closed itself on the
        // strength of having asked would be claiming something it cannot know,
        // which is the same rule the close path follows.
        if (options_.kill_terminal_in_session) options_.kill_terminal_in_session(*terminal);
    });
}

void ClientApp::confirm_then_close(w::Window* window) {
    if (window == nullptr || desktop_ == nullptr) return;
    ckv::term::TerminalSubsession* const terminal = terminal_shown_by(window);
    if (terminal == nullptr) return;

    // Not "close it anyway?": the reader is deciding what happens to the
    // PROGRAM, and yes/no said nothing about that. The dialog names each
    // outcome — asked to quit and optionally killed, or moved somewhere the
    // program keeps running — and the End session dialog is its model.
    w::DialogDescriptor descriptor;
    descriptor.title = "Close terminal";
    w::FieldDescriptor what;
    what.kind = w::FieldKind::Note;
    what.label =
        "\"" + window->title() + "\" is still running a program. Closing asks it to quit.";
    descriptor.fields.push_back(std::move(what));

    std::optional<std::size_t> force_field;
    if (options_.close_terminal_in_session) {
        force_field = descriptor.fields.size();
        w::FieldDescriptor force;
        force.kind = w::FieldKind::Check;
        force.label = "&Kill it if it has not quit after " +
                      std::to_string(options_.settings.kill_grace_seconds) + " seconds";
        // Ticked by default, exactly as End session's is: a reader closing a
        // terminal means it to close. Unticking is the deliberate choice, and
        // the note says what that choice costs.
        force.initial_checked = true;
        descriptor.fields.push_back(std::move(force));
        w::FieldDescriptor cost;
        cost.kind = w::FieldKind::Note;
        cost.label = "  Unticked, a program that declines keeps running — window and all.";
        descriptor.fields.push_back(std::move(cost));
        w::FieldDescriptor where;
        where.kind = w::FieldKind::Note;
        where.label = "  The wait is Settings ▸ \"Give programs this long to quit\".";
        descriptor.fields.push_back(std::move(where));
        if (terminal_windows_.size() == 1 && options_.settings.kill_empty_session) {
            // The one consequence a reader cannot see from here: the session
            // goes with its last terminal (kill-empty-session, the session model).
            w::FieldDescriptor last;
            last.kind = w::FieldKind::Note;
            const std::string& name = attached_session_name_;
            last.label =
                "  This is the last terminal: closing it ends " +
                (name.empty() ? std::string("the session") : "session \"" + name + "\"") + ".";
            descriptor.fields.push_back(std::move(last));
        }
    } else {
        // M1: the terminal lives in this process and cannot outlive its
        // window, so there is no unticked variant to offer — only the truth
        // about what closing does.
        w::FieldDescriptor cost;
        cost.kind = w::FieldKind::Note;
        cost.label = "  A program that has not quit by the time the window goes is killed.";
        descriptor.fields.push_back(std::move(cost));
    }

    descriptor.buttons.push_back(w::ButtonDescriptor{"&Close terminal", w::ButtonRole::Accept, {}});
    if (options_.move_terminal) {
        // Dismiss-with-a-job: this dialog goes the way Cancel takes it, and
        // the move picker opens in its place — one question on screen at a
        // time, and no close happens that the reader did not choose.
        descriptor.buttons.push_back(w::ButtonDescriptor{
            "&Move instead…", w::ButtonRole::Dismiss, [this, terminal] {
                app_.post([this, terminal] { show_move_terminal_dialog(terminal); });
            }});
    }
    descriptor.buttons.push_back(w::ButtonDescriptor{"Cancel", w::ButtonRole::Dismiss, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler(
        [this, presentation, terminal, force_field](const w::DialogResult& result) {
            if (!result.accepted) return;
            // Looked up now, not captured: the terminal can end while the
            // dialog is open, and a window that is already gone means there
            // is nothing left to close.
            w::Window* const answered = window_showing(terminal);
            if (answered == nullptr) return;
            if (options_.close_terminal_in_session) {
                const bool force = force_field.has_value() &&
                                   *force_field < result.checked.size() &&
                                   result.checked[*force_field];
                options_.close_terminal_in_session(*terminal, force,
                                                   options_.settings.kill_grace_seconds);
                // The window stays. It goes when the server says the terminal
                // ended — or stays for good if the reader unticked the kill
                // and the program declined, which is what the dialog said.
            } else {
                confirmed_close_ = answered;
                confirmed_close_alive_ = answered->lifetime_token();
                answered->close();
            }
        });
    retain_dialog(presentation);
}

void ClientApp::show_move_terminal_dialog(ckv::term::TerminalSubsession* terminal) {
    if (desktop_ == nullptr || !options_.move_terminal) return;
    w::Window* const window = window_showing(terminal);
    if (window == nullptr) return;  // ended while the question was being asked

    w::DialogDescriptor descriptor;
    descriptor.title = "Move terminal";
    w::FieldDescriptor what;
    what.kind = w::FieldKind::Note;
    what.label = "Where \"" + window->title() + "\" goes on running.";
    descriptor.fields.push_back(std::move(what));
    w::FieldDescriptor unmoved;
    unmoved.kind = w::FieldKind::Note;
    unmoved.label = "Its window closes here; the program never notices the move.";
    descriptor.fields.push_back(std::move(unmoved));

    // The real destinations: every session that is not this one, then a fresh
    // one. The list of others can be empty and the dialog still works — "a
    // new session" is always true, which is what keeps the close dialog's
    // Move button from ever being the greyed kind of lie.
    // The radio's own index in the result: `selected` is parallel to the
    // FIELDS, and the notes above it answer -1 — reading front() here is the
    // mistake the session picker cannot make, because its radio is first.
    const std::size_t to_field = descriptor.fields.size();
    w::FieldDescriptor to;
    to.kind = w::FieldKind::Radio;
    to.label = "To";
    move_choice_.clear();
    for (const SessionRow& row : last_sessions_) {
        if (row.id == attached_session_) continue;
        std::string line = row.name.empty() ? "session " + std::to_string(row.id) : row.name;
        line += row.terminals == 1 ? " — 1 terminal"
                                   : " — " + std::to_string(row.terminals) + " terminals";
        to.options.push_back(std::move(line));
        move_choice_.push_back(row.id);
    }
    to.options.push_back("a &new session");
    to.initial_selection = 0;
    descriptor.fields.push_back(std::move(to));

    descriptor.buttons.push_back(w::ButtonDescriptor{"&Move", w::ButtonRole::Accept, {}});
    descriptor.buttons.push_back(w::ButtonDescriptor{"Cancel", w::ButtonRole::Dismiss, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler(
        [this, presentation, terminal, to_field](const w::DialogResult& result) {
            if (!result.accepted || result.selected.size() <= to_field) return;
            const int chosen = result.selected[to_field];
            if (chosen < 0) return;
            if (window_showing(terminal) == nullptr) return;  // ended meanwhile
            const std::size_t index = static_cast<std::size_t>(chosen);
            if (index < move_choice_.size())
                options_.move_terminal(*terminal, move_choice_[index], false);
            else
                options_.move_terminal(*terminal, 0, true);
            // The window leaves when the server says the terminal left; a
            // window taken down here would vanish even when the move failed.
        });
    retain_dialog(presentation);
}

ckv::widgets::Window* ClientApp::window_showing(const ckv::term::TerminalSubsession* terminal) const {
    if (terminal == nullptr) return nullptr;
    const auto found = terminal_windows_.find(terminal);
    return found != terminal_windows_.end() ? found->second : nullptr;
}

ckv::term::TerminalSubsession* ClientApp::terminal_shown_by(w::Window* window) const {
    if (window == nullptr) return nullptr;
    for (const auto& [subsession, shown] : terminal_windows_)
        if (shown == window)
            return const_cast<ckv::term::TerminalSubsession*>(subsession);
    return nullptr;
}

void ClientApp::close_terminal(w::Window* window) {
    if (window != nullptr) window->close();
}

ckv::widgets::Window* ClientApp::active_terminal() const {
    return desktop_ != nullptr ? desktop_->active_window() : nullptr;
}

// The bell a window wears in front of its name (WP-19, owner ruling
// 2026-08-20). One cell, always: `⍾` is U+237E, measured at width 1 by
// ckVision's own `text::text_width`, and the blink's off phase is a SPACE
// rather than nothing so the title never shifts a column. The obvious glyph,
// U+1F514 🔔, measures 2 — it would re-flow every unfocused title once per
// second, which is the invariant the whitespace rule exists to protect.
constexpr std::string_view kBellGlyph = "\u237E";
// Both in POLLS, because the poll is the only regular tick this client has.
// At `title_poll_nanos` = 100 ms: five polls is half a second, so ten is one
// blink cycle; fifty is five seconds.
constexpr int kBellBlinkPolls = 5;
constexpr int kBellFocusedPolls = 50;

std::string ClientApp::effective_title(const TerminalTitle& title) {
    // The reader's name outranks everything, which is what makes it an
    // override rather than one more writer of the same string. Underneath it
    // the program's own claim outranks ckmux's, and an EMPTY claim is how a
    // program hands the caption back — on exit, or when it simply stops
    // claiming one — so the window returns to the name ckmux gave it rather
    // than keeping stale text forever.
    const std::string name =
        !title.custom.empty() ? title.custom
                              : (title.child.empty() ? title.fallback : title.child);
    // And the badge, which outranks nothing and is appended to whatever the
    // name turned out to be (the interface spec). `[exit 1]` and `[exit 0]` are
    // different news — one is a build that failed and one is a build that
    // finished — so the number is stated when the host can supply it, and
    // `[exited]` stands in when it cannot rather than inventing a zero.
    std::string composed = name;
    if (!title.badge.empty()) composed += " " + title.badge;
    // In FRONT of the name, because it is the thing a reader is meant to catch
    // out of the corner of an eye, and because the badge is about the program
    // that ended while this is about the one that is still there.
    if (!title.bell_cell.empty()) composed = title.bell_cell + composed;
    return composed;
}


void ClientApp::refresh_terminal_titles() {
    if (desktop_ == nullptr) return;
    // The printer button rides this poll rather than one of its own: its byte
    // counter must move while a child prints, and the same tick that re-reads
    // a caption is exactly as often as either can visibly change.
    refresh_printer_buttons();
    for (w::Window* window : desktop_->windows()) {
        const auto entry = titles_.find(window);
        if (entry == titles_.end()) continue;  // a dialog, not a terminal
        w::TerminalView* const view = terminal_view_of(window);
        if (view == nullptr) continue;
        // `status()`, not `snapshot()`: this runs on a timer for every window,
        // and a snapshot copies the grid and the history to hand back one
        // string — which on a client's mirror means rebuilding its whole
        // scrollback, several times a second, to ask what the caption is.
        const std::string wanted = view->session().status().title;
        // And what the SESSION says the reader named it, where there is a
        // session layer to ask. Polled beside the child's title rather than
        // pushed, because the two answer the same question at the same moment
        // and a caption assembled from two readings taken a tick apart is a
        // caption that can flicker between them. An M1 client asks nobody: it
        // owns the override itself, and `titles_` already holds it.
        const ckv::term::TerminalSubsession* const terminal = terminal_shown_by(window);
        const std::string pinned = options_.custom_title && terminal != nullptr
                                       ? options_.custom_title(*terminal)
                                       : entry->second.custom;
        // What this window is saying to a reader who is elsewhere (WP-19).
        //
        // The border is HELD rather than blinked: a blink needs a timer per
        // window and shows a reader who looks away between frames nothing at
        // all, while a border that stays lit until they visit says the same
        // thing the footer count says and keeps saying it. Deliberate
        // deviation from the interface spec's word "flash", recorded in the row.
        //
        // No new colour either: the window is drawn with its own ACTIVE frame
        // role while unfocused, so a window asking for attention looks like
        // the window a reader is in. That reuses a colour every theme already
        // guarantees is legible rather than inventing one.
        if (options_.terminal_marks && terminal != nullptr && desktop_ != nullptr) {
            const bool elsewhere = window != desktop_->active_window();
            const TerminalMarks marks = options_.terminal_marks(*terminal);
            // A fact that has gone away is a mark a reader may be told about
            // again. The server does not retract one today — its flag is
            // sticky until respawn — but this is written against the fact,
            // so it needs no change when the wire learns to say "rang again".
            // Nothing to reset: a serial only goes up, so "answered" is a
            // number this reader has reached rather than a flag something else
            // has to lower. That is the whole gain over the level.
            // Looking at it IS the answer, and it is THIS reader's answer:
            // the server's flag says "this terminal has rung at some point",
            // which is not what a footer flag means. Without this a visited
            // bell returns the moment the reader steps out, and the footer
            // becomes an ornament they learn to ignore — the same as not
            // having one.
            if (!elsewhere) {
                // Looking answers everything it has said so far — and only so
                // far. A ring after this moment carries a higher serial and is
                // a new thing to be told about.
                entry->second.bell_answered = marks.bell_serial;
                entry->second.activity_answered = marks.activity_serial;
            }
            const bool wants = elsewhere && marks.bell_serial > entry->second.bell_answered;
            const bool busy =
                elsewhere && marks.activity_serial > entry->second.activity_answered;
            if (wants != entry->second.lit || busy != entry->second.noted_activity) {
                // The rising edge is the bell, and only the rising edge: this
                // poll runs several times a second, and a reader whose host
                // rang on every tick would turn the setting off and never
                // learn what it was for.
                if (wants && !entry->second.lit && options_.settings.audible_bell &&
                    options_.ring_host_bell)
                    options_.ring_host_bell();
                if (wants != entry->second.lit) {
                    const ckv::ui::RoleId frame_active =
                        app_.roles().find("ckv.window.frame.active");
                    const ckv::ui::RoleId title_active =
                        app_.roles().find("ckv.window.title.active");
                    window->set_role_override(
                        frame_active,
                        wants ? frame_active : app_.roles().find("ckv.window.frame.inactive"),
                        title_active,
                        wants ? title_active : app_.roles().find("ckv.window.title.inactive"));
                }
                entry->second.lit = wants;
                entry->second.noted_activity = busy;
                // And the footer, which counts these windows. Asked here
                // because this poll is the moment ckmux LEARNS a mark changed —
                // nothing pushes it — so without this the flags would appear
                // only when some unrelated event happened to redraw the
                // footer, which for a reader waiting on a build is never.
                refresh_footer();
            }

            // The bell glyph, whose whole behaviour is counted in polls.
            //
            // A reader IN the window is told briefly and then left alone: the
            // program rang while they were looking at it, so five seconds is
            // an acknowledgement rather than a summons. A reader ELSEWHERE is
            // summoned, and a blink is what carries across a screen they are
            // not pointed at — until they answer it by visiting, which is what
            // `bell_seen` records.
            if (!marks.bell) {
                entry->second.bell_ticks = 0;
            } else {
                ++entry->second.bell_ticks;
            }
            const int ticks = entry->second.bell_ticks;
            std::string cell;
            if (marks.bell) {
                if (!elsewhere) {
                    // 50 polls at 100 ms. Steady, not blinking: a reader
                    // looking straight at it does not need to be waved at.
                    if (ticks <= kBellFocusedPolls) cell = std::string(kBellGlyph);
                } else if (marks.bell_serial > entry->second.bell_answered) {
                    // 5 polls on, 5 off — one second a cycle. The off phase is
                    // a SPACE and not an empty string, or the title shifts a
                    // column twice a second for as long as the bell stands.
                    const bool on = ((ticks - 1) / kBellBlinkPolls) % 2 == 0;
                    cell = on ? std::string(kBellGlyph) : std::string(" ");
                }
            }
            if (cell != entry->second.bell_cell) {
                entry->second.bell_cell = cell;
                window->set_title(effective_title(entry->second));
            }
        }
        // While its history is being read the caption belongs to copy mode,
        // which writes the badge and the scroll position into it. Both names
        // are still recorded, so leaving copy mode restores whatever the
        // program — or the reader — asked for in the meantime rather than a
        // stale name.
        if (window == copy_mode_window_) {
            entry->second.child = wanted;
            entry->second.custom = pinned;
            refresh_copy_mode_caption();
            continue;
        }
        // The badge is recomputed on the same poll as the title, because it
        // answers the same question at the same moment: a child that exits
        // between two ticks changes both, and a caption assembled from two
        // readings taken a tick apart can flicker between them.
        std::string badge;
        if (child_has_ended(view)) {
            const std::optional<int> status = options_.exit_status && terminal != nullptr
                                                  ? options_.exit_status(*terminal)
                                                  : std::nullopt;
            badge = status.has_value() ? "[exit " + std::to_string(*status) + "]" : "[exited]";
        }
        if (wanted == entry->second.child && pinned == entry->second.custom &&
            badge == entry->second.badge)
            continue;  // nothing changed
        entry->second.child = wanted;
        entry->second.custom = pinned;
        const bool badge_changed = badge != entry->second.badge;
        entry->second.badge = badge;
        window->set_title(effective_title(entry->second));
        // A child that ended changed what this window offers, not just what it
        // is called. The footer is asked again here because this poll is the
        // moment ckmux LEARNS about the exit — nothing pushes it, so without
        // this the hint would appear only when some unrelated event happened
        // to refresh the footer, which for a reader watching a build finish is
        // never.
        if (badge_changed) refresh_footer();
    }
}

void ClientApp::rename_terminal(ckv::term::TerminalSubsession* terminal, std::string name) {
    // Looked up before anything is done with the terminal, not captured and
    // trusted: this runs when a reader presses a button, and the terminal can
    // have ended while the dialog stood open. A window that is already gone
    // means there is nothing left to name — and it is also what makes the
    // dereference below safe, since `window_showing` only compares addresses.
    w::Window* const window = window_showing(terminal);
    if (window == nullptr) return;
    const auto entry = titles_.find(window);
    if (entry == titles_.end()) return;
    // The server next, where there is one, because the name is session state
    // and the server is what makes it survive a detach (the session model). What comes
    // back through `custom_title` is then the same string, so the assignment
    // below is not a second opinion — it is the one that stops the caption
    // waiting a round trip to change.
    if (options_.rename_terminal) options_.rename_terminal(*terminal, name);
    entry->second.custom = std::move(name);
    window->set_title(effective_title(entry->second));
    // The window bar damps how often a button may change length, to stop a
    // caption that a program rewrites at every prompt from re-flowing the row
    // (the interface spec). A rename is not that: the reader has just asked for this
    // name, and a button that took half a minute to fit it would read as a
    // command that did not work. So this one change is exempt, by telling the
    // bar the change was asked for rather than observed.
    if (switcher_ != nullptr) switcher_->settle_width(*window);
}

void ClientApp::show_rename_terminal_dialog(ckv::term::TerminalSubsession* terminal) {
    if (terminal == nullptr || desktop_ == nullptr) return;
    w::Window* const window = window_showing(terminal);
    if (window == nullptr) return;
    const auto entry = titles_.find(window);
    if (entry == titles_.end()) return;

    w::DialogDescriptor descriptor;
    descriptor.title = "Rename terminal";
    w::FieldDescriptor name;
    name.label = "&Name";
    name.kind = w::FieldKind::Text;
    // Whatever the caption says right now, whether that is the reader's own
    // name or the one the program claimed. A field that started empty would
    // make the commonest rename — adjusting a name that is nearly right —
    // into retyping it.
    name.initial_text = effective_title(entry->second);
    descriptor.fields.push_back(std::move(name));

    // What the reader is choosing between, said before they choose. A pinned
    // caption looks no different from an unpinned one on screen, and the
    // difference is the whole feature: the program goes on renaming itself
    // underneath and simply stops being shown.
    //
    // Four short notes rather than two long ones. Consecutive notes are one
    // paragraph to ckVision and each is ONE ROW — so a sentence longer than the
    // dialog is clipped rather than wrapped, and the half that says what a
    // button does is the half that goes. Each line here is written to fit the
    // 80-column terminal the interface spec sizes every dialog against.
    const auto note = [&descriptor](std::string line) {
        w::FieldDescriptor field;
        field.kind = w::FieldKind::Note;
        field.label = std::move(line);
        descriptor.fields.push_back(std::move(field));
    };
    note("The program in this terminal renames its own window; a name you");
    note("give here is shown instead, until you change it.");
    // What the third button hands the caption back TO, named rather than
    // described: it is the program's CURRENT title, which is not necessarily
    // the name this window started with and not necessarily what it said when
    // the reader pinned it.
    note("Use Default Title goes back to the program's own name, now:");
    // On its own line, and elided, because this is a CHILD's string on ckmux's
    // chrome. The wire lets a caption be four kilobytes; on one clipped row
    // that would be a wall of the child's text and none of the sentence around
    // it. The same reasoning that length-bounds the caption itself
    // ([02-architecture.md](02-architecture.md) security posture), applied at
    // the second place a child's text is drawn.
    const std::string fallback = entry->second.child.empty() ? entry->second.fallback
                                                             : entry->second.child;
    note("  \"" + ckv::text::elide_to_width(fallback, kDialogTitleColumns) + "\"");

    ckv::term::TerminalSubsession* const named = terminal;
    const std::weak_ptr<void> alive = alive_;

    descriptor.buttons.push_back(w::ButtonDescriptor{"&Rename", w::ButtonRole::Accept, {}});
    // Dismiss rather than Neutral, and deliberately: `Dismiss` is "this
    // dialog ends now, having run its handler", which is exactly what this
    // button does — the reader has made their decision and there is nothing
    // left to fill in. Cancel is the same role with nothing to run, which is
    // what makes them siblings rather than one of them a special case.
    descriptor.buttons.push_back(w::ButtonDescriptor{
        "Use &Default Title", w::ButtonRole::Dismiss, [this, alive, named] {
            if (alive.expired()) return;
            // An empty name is the request: hand the caption back, and let
            // it follow the program again. `rename_terminal` re-checks that
            // the terminal is still there, which it may not be.
            rename_terminal(named, {});
        }});
    descriptor.buttons.push_back(w::ButtonDescriptor{"Cancel", w::ButtonRole::Dismiss, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler(
        [this, presentation, alive, named](const w::DialogResult& result) {
            if (alive.expired() || !result.accepted || result.values.empty()) return;
            // An empty field is the same request the Default button makes, and
            // is answered the same way rather than refused: a reader who
            // cleared the name has said what they want as plainly as one who
            // pressed the button for it.
            rename_terminal(named, result.values.front());
        });
    retain_dialog(presentation);
}

// --- The virtual printer (PRINT-3…6) ---------------------------------------

PrinterButtonModel ClientApp::printer_model_for(w::Window* window) const {
    PrinterButtonModel model;
    if (window == nullptr) return model;
    const ckv::term::TerminalSubsession* const terminal = terminal_shown_by(
        const_cast<w::Window*>(window));
    if (terminal == nullptr) return model;
    // Whatever the session layer knows. Unset is M1, where the client owns its
    // terminals directly and reads the emulator's own scalars.
    if (options_.printer_status) {
        model = options_.printer_status(*terminal);
    } else {
        const ckv::term::TerminalStatus status = terminal->status();
        model.mode = wire_printer_mode_of(options_.settings.printer_mode);
        model.state = status.printer_sunk       ? proto::PrinterState::Sunk
                      : status.printer_controller_active ? proto::PrinterState::Capturing
                                                         : proto::PrinterState::Idle;
        model.pending_bytes = status.printer_pending_bytes;
        model.jobs = status.printer_jobs_ready;
    }
    const auto surface = printers_.find(const_cast<w::Window*>(window));
    model.answered = surface != printers_.end() && surface->second.answered;
    return model;
}

w::Window* ClientApp::window_showing(const ckv::term::TerminalSubsession& terminal) {
    if (desktop_ == nullptr) return nullptr;
    for (w::Window* window : desktop_->windows()) {
        if (titles_.find(window) == titles_.end()) continue;  // a dialog, not a terminal
        if (terminal_shown_by(window) == &terminal) return window;
    }
    return nullptr;
}

void ClientApp::finish_pending_save() {
    if (awaiting_print_job_ == 0 || pending_save_window_ == nullptr) return;
    if (desktop_ == nullptr) return;
    // Still open? A reader who closed the window while the text was on its way
    // has withdrawn the question, and nothing should be written on their behalf.
    const std::vector<w::Window*> windows = desktop_->windows();
    if (std::find(windows.begin(), windows.end(), pending_save_window_) == windows.end()) {
        awaiting_print_job_ = 0;
        pending_save_window_ = nullptr;
        pending_save_path_.clear();
        return;
    }
    ckv::term::TerminalSubsession* const terminal = terminal_shown_by(pending_save_window_);
    if (terminal == nullptr) return;
    if (!options_.print_job_text) return;
    const std::string* const text = options_.print_job_text(*terminal, awaiting_print_job_);
    if (text == nullptr) return;  // not here yet; ask again next poll
    // Taken before the write, so a save that fails cannot leave the request
    // armed and repeat itself on every poll for the rest of the session.
    const std::uint64_t job = awaiting_print_job_;
    const std::string path = pending_save_path_;
    awaiting_print_job_ = 0;
    pending_save_window_ = nullptr;
    pending_save_path_.clear();
    save_print_job(*terminal, job, path);
}

void ClientApp::refresh_printer_buttons() {
    if (desktop_ == nullptr) return;
    // A save waiting on its text finishes here, on the same poll that moves the
    // byte counter. Until this existed, `save_print_job` asked for the text,
    // recorded what it was waiting for, and nothing ever read those fields
    // back: the fetch went out, the answer arrived, and the reader who pressed
    // Save got no file and no message — the silent failure the save path's own
    // comment calls the worst of the four outcomes.
    finish_pending_save();
    for (w::Window* window : desktop_->windows()) {
        if (titles_.find(window) == titles_.end()) continue;  // a dialog, not a terminal
        const PrinterButtonModel model = printer_model_for(window);
        PrinterSurface& surface = printers_[window];
        const bool wanted = printer_button_state(model) != PrinterButtonState::Hidden;

        if (!wanted) {
            // Taken away rather than left blank. An overlay that draws nothing
            // still occupies its slot, and a window that once printed would
            // keep a dead cell on its border for the rest of its life.
            if (surface.button != nullptr) {
                window->remove_frame_overlay(surface.button).reset();
                surface.button = nullptr;
            }
            continue;
        }
        if (surface.button == nullptr) {
            auto button = std::make_unique<PrinterButton>();
            const std::weak_ptr<void> alive = alive_;
            // Bound to the WINDOW, not to whatever is active: the button is
            // drawn on one window's frame and must act on that window, which
            // is the same rule the switcher bar's context menu follows.
            w::Window* const owner = window;
            button->on_activate = [this, alive, owner](PrinterButtonState state) {
                if (alive.expired()) return;
                ckv::term::TerminalSubsession* const target = terminal_shown_by(owner);
                if (target == nullptr) return;
                switch (state) {
                    case PrinterButtonState::Asking:
                        show_printer_ask_dialog(target);
                        break;
                    case PrinterButtonState::Sunk:
                        // The only useful next move: what would have let the
                        // document through. Print Output would show an empty
                        // list, which answers nothing.
                        show_printer_settings_dialog(target);
                        break;
                    case PrinterButtonState::Holding:
                    case PrinterButtonState::Full:
                        show_print_output_dialog(target);
                        break;
                    case PrinterButtonState::Hidden:
                        break;
                }
            };
            // Bottom border, left-aligned: the bottom inset clears the resize
            // grips exactly, and frame overlays cast no shadow (the interface spec).
            surface.button = window->add_frame_overlay(
                std::move(button),
                w::FrameSlot{w::Edge::Bottom, u::Alignment::Start, 0});
        }
        surface.button->set_model(model);
    }
}

void ClientApp::show_printer_ask_dialog(ckv::term::TerminalSubsession* terminal) {
    if (terminal == nullptr || desktop_ == nullptr) return;
    w::Window* const window = window_showing(terminal);
    if (window == nullptr) return;
    const PrinterButtonModel model = printer_model_for(window);

    w::DialogDescriptor descriptor;
    descriptor.title = "Print output";
    const auto note = [&descriptor](std::string line) {
        w::FieldDescriptor field;
        field.kind = w::FieldKind::Note;
        field.label = std::move(line);
        descriptor.fields.push_back(std::move(field));
    };
    note("A program in this terminal is sending output to a printer.");
    note(format_bytes(model.pending_bytes) + " captured so far.");

    // The four answers, and each names the scope it applies to — because "yes"
    // to one terminal and "yes" to everything are different promises, and a
    // dialog that collapsed them would make the reader's answer bigger than
    // their intent.
    const std::size_t answer_field = descriptor.fields.size();
    w::FieldDescriptor answer;
    answer.kind = w::FieldKind::Radio;
    answer.label = "";
    answer.options = {"Keep capturing — this terminal", "Keep capturing — this session",
                      "Keep capturing — always (save as default)",
                      "Stop capturing (report \"no printer\")"};
    // The session, because it is the answer that is almost always meant: a
    // reader saying yes to one print usually means yes to this piece of work,
    // not to this window and not to every terminal they will ever open.
    answer.initial_selection = 1;
    descriptor.fields.push_back(std::move(answer));

    ckv::term::TerminalSubsession* const asked = terminal;
    const std::weak_ptr<void> alive = alive_;
    w::Window* const owner = window;
    descriptor.buttons.push_back(w::ButtonDescriptor{
        "&View / Save…", w::ButtonRole::Dismiss, [this, alive, asked] {
            if (alive.expired()) return;
            // Looking counts as answering: a reader who opened the document
            // has engaged with the question, and asking again while they read
            // it would be the same prompt twice.
            if (w::Window* const shown = window_showing(asked)) printers_[shown].answered = true;
            show_print_output_dialog(asked);
        }});
    descriptor.buttons.push_back(w::ButtonDescriptor{
        "&Discard", w::ButtonRole::Dismiss, [this, alive, asked] {
            if (alive.expired() || window_showing(asked) == nullptr) return;
            if (options_.discard_print_job) options_.discard_print_job(*asked, 0);
        }});
    descriptor.buttons.push_back(w::ButtonDescriptor{
        "&Settings…", w::ButtonRole::Dismiss, [this, alive, asked] {
            if (alive.expired()) return;
            show_printer_settings_dialog(asked);
        }});
    descriptor.buttons.push_back(w::ButtonDescriptor{"&OK", w::ButtonRole::Accept, {}});
    descriptor.buttons.push_back(w::ButtonDescriptor{"Cancel", w::ButtonRole::Dismiss, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler(
        [this, presentation, alive, asked, owner, answer_field](const w::DialogResult& result) {
            if (alive.expired() || !result.accepted) return;
            if (result.selected.size() <= answer_field) return;
            const int chosen = result.selected[answer_field];
            if (chosen < 0) return;
            // Answered either way — including "stop", which is an answer and
            // must not leave the button asking the same question again.
            if (owner != nullptr) printers_[owner].answered = true;
            const PrinterMode mode = chosen == 3 ? PrinterMode::Off : PrinterMode::Capture;
            const proto::PrinterScope scope = chosen == 0   ? proto::PrinterScope::Terminal
                                              : chosen == 1 ? proto::PrinterScope::Session
                                                            : proto::PrinterScope::Global;
            apply_printer_choice(asked, scope, mode);
        });
    retain_dialog(presentation);
}

void ClientApp::apply_printer_choice(ckv::term::TerminalSubsession* terminal,
                                     proto::PrinterScope scope, PrinterMode mode) {
    if (terminal == nullptr) return;
    if (options_.set_printer_policy) {
        options_.set_printer_policy(0, terminal, scope, mode,
                                    static_cast<std::uint32_t>(
                                        options_.settings.printer_ask_cache_bytes),
                                    static_cast<std::uint32_t>(
                                        options_.settings.printer_spool_limit_bytes));
    } else {
        // M1: no scopes above this process, so every scope is the same one and
        // the client's own setting IS the policy.
        options_.settings.printer_mode = mode;
    }
    // "Always" is the one answer that outlives the session, so it is the one
    // that touches the reader's file. The others are deliberately not written:
    // a per-terminal yes that quietly became a permanent default would be an
    // answer larger than the question asked.
    if (scope == proto::PrinterScope::Global) {
        const char* const written = mode == PrinterMode::Off      ? "off"
                                    : mode == PrinterMode::Capture ? "capture"
                                                                   : "ask";
        (void)ckm::save_setting(ckm::platform::config_file_path(), "printer", "mode", written);
    }
}

void ClientApp::show_printer_settings_dialog(ckv::term::TerminalSubsession* terminal) {
    if (desktop_ == nullptr) return;
    const Settings& now = options_.settings;

    w::DialogDescriptor descriptor;
    descriptor.title = "Printer Settings";
    const auto note = [&descriptor](std::string line) {
        w::FieldDescriptor field;
        field.kind = w::FieldKind::Note;
        field.label = std::move(line);
        descriptor.fields.push_back(std::move(field));
    };

    const std::size_t scope_field = descriptor.fields.size();
    w::FieldDescriptor scope;
    scope.kind = w::FieldKind::Radio;
    scope.label = "Scope";
    scope.options = {"This terminal", "This session", "Global"};
    // The session again, for the reason the Ask popup defaults there: it is
    // the scope a reader almost always means, and the one whose effect they
    // can see without it following them into tomorrow.
    scope.initial_selection = terminal == nullptr ? 2 : 1;
    descriptor.fields.push_back(std::move(scope));

    const std::size_t mode_field = descriptor.fields.size();
    w::FieldDescriptor mode;
    mode.kind = w::FieldKind::Radio;
    mode.label = "When a program prints";
    mode.options = {"Ask me (capture, show the button)", "Always capture",
                    "Off — report \"no printer\""};
    mode.initial_selection = now.printer_mode == PrinterMode::Ask       ? 0
                             : now.printer_mode == PrinterMode::Capture ? 1
                                                                        : 2;
    descriptor.fields.push_back(std::move(mode));

    const std::size_t ask_cache_field = descriptor.fields.size();
    w::FieldDescriptor ask_cache;
    ask_cache.kind = w::FieldKind::Number;
    ask_cache.label = "Ask cache (KB)";
    ask_cache.initial_text = std::to_string(now.printer_ask_cache_bytes / 1024);
    ask_cache.minimum = 1;
    ask_cache.maximum = 1024 * 1024;
    descriptor.fields.push_back(std::move(ask_cache));
    note("  Discarded whole if exceeded — nothing has been consented to yet.");

    const std::size_t spool_field = descriptor.fields.size();
    w::FieldDescriptor spool;
    spool.kind = w::FieldKind::Number;
    spool.label = "Spool limit (KB)";
    spool.initial_text = std::to_string(now.printer_spool_limit_bytes / 1024);
    spool.minimum = 1;
    spool.maximum = 1024 * 1024;
    descriptor.fields.push_back(std::move(spool));
    note("  Per terminal, once capturing. Finished jobs are kept.");

    const std::size_t format_field = descriptor.fields.size();
    w::FieldDescriptor format;
    format.kind = w::FieldKind::Combo;
    format.label = "Default format";
    format.options = {"Plain text (.txt)", "Original stream (.ansi)"};
    format.initial_selection = now.printer_save_format == PrinterSaveFormat::Ansi ? 1 : 0;
    descriptor.fields.push_back(std::move(format));

    const std::size_t folder_field = descriptor.fields.size();
    w::FieldDescriptor folder;
    folder.kind = w::FieldKind::Text;
    folder.label = "Default folder";
    folder.initial_text = now.printer_save_folder;
    descriptor.fields.push_back(std::move(folder));

    const std::size_t ask_name_field = descriptor.fields.size();
    w::FieldDescriptor ask_name;
    ask_name.kind = w::FieldKind::Check;
    ask_name.label = "Ask for a filename every time (else auto-name)";
    ask_name.initial_checked = now.printer_save_ask_name;
    descriptor.fields.push_back(std::move(ask_name));

    // What is actually in force here, and WHERE IT CAME FROM. The interface spec makes
    // the provenance a requirement rather than a nicety: a per-session
    // override a reader set an hour ago and forgot is precisely the thing an
    // effective-value display must not hide, and a dialog showing only the
    // number would leave them editing a scope that loses.
    if (options_.printer_effective && terminal != nullptr) {
        note("");
        note("Effective here: " + options_.printer_effective(*terminal));
    }

    descriptor.buttons.push_back(w::ButtonDescriptor{"&Save as default", w::ButtonRole::Neutral, {}});
    descriptor.buttons.push_back(w::ButtonDescriptor{"&OK", w::ButtonRole::Accept, {}});
    descriptor.buttons.push_back(w::ButtonDescriptor{"Cancel", w::ButtonRole::Dismiss, {}});

    ckv::term::TerminalSubsession* const target = terminal;
    const std::weak_ptr<void> alive = alive_;
    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler(
        [this, presentation, alive, target, scope_field, mode_field, ask_cache_field, spool_field,
         format_field, folder_field, ask_name_field](const w::DialogResult& result) {
            if (alive.expired() || !result.accepted) return;
            const auto selected = [&result](std::size_t field, int fallback) {
                return field < result.selected.size() && result.selected[field] >= 0
                           ? result.selected[field]
                           : fallback;
            };
            const auto number = [&result](std::size_t field, std::uint32_t fallback) {
                if (field >= result.values.size() || result.values[field].empty()) return fallback;
                return static_cast<std::uint32_t>(std::strtoul(result.values[field].c_str(),
                                                              nullptr, 10));
            };
            const int chosen_mode = selected(mode_field, 0);
            const PrinterMode chosen_printer_mode = chosen_mode == 0   ? PrinterMode::Ask
                                     : chosen_mode == 1 ? PrinterMode::Capture
                                                        : PrinterMode::Off;
            const int chosen_scope = selected(scope_field, 1);
            const proto::PrinterScope chosen_printer_scope = chosen_scope == 0   ? proto::PrinterScope::Terminal
                                              : chosen_scope == 1 ? proto::PrinterScope::Session
                                                                  : proto::PrinterScope::Global;
            // KB in the dialog, bytes everywhere else. A reader types 256, not
            // 262144; the conversion belongs here, once, rather than in each
            // place that reads the setting.
            const std::uint32_t ask_cache_bytes = number(ask_cache_field, 256) * 1024U;
            const std::uint32_t spool_bytes = number(spool_field, 1024) * 1024U;

            options_.settings.printer_save_format =
                selected(format_field, 0) == 1 ? PrinterSaveFormat::Ansi : PrinterSaveFormat::Text;
            if (folder_field < result.values.size() && !result.values[folder_field].empty())
                options_.settings.printer_save_folder = result.values[folder_field];
            if (ask_name_field < result.checked.size())
                options_.settings.printer_save_ask_name = result.checked[ask_name_field];

            if (options_.set_printer_policy) {
                options_.set_printer_policy(0, target, chosen_printer_scope, chosen_printer_mode, ask_cache_bytes, spool_bytes);
            } else {
                options_.settings.printer_mode = chosen_printer_mode;
                options_.settings.printer_ask_cache_bytes = ask_cache_bytes;
                options_.settings.printer_spool_limit_bytes = spool_bytes;
            }
        });
    retain_dialog(presentation);
}

void ClientApp::show_print_output_dialog(ckv::term::TerminalSubsession* terminal) {
    if (terminal == nullptr || desktop_ == nullptr) return;
    w::Window* const window = window_showing(terminal);
    if (window == nullptr) return;

    std::vector<proto::PrintJobInfo> jobs;
    if (options_.printer_jobs) jobs = options_.printer_jobs(*terminal);

    w::DialogDescriptor descriptor;
    descriptor.title = "Print output";
    const auto note = [&descriptor](std::string line) {
        w::FieldDescriptor field;
        field.kind = w::FieldKind::Note;
        field.label = std::move(line);
        descriptor.fields.push_back(std::move(field));
    };

    if (jobs.empty()) {
        // Reachable: a reader opens the window, another client discards, or
        // the ask cache overflowed and freed everything. An empty list with no
        // sentence would read as a broken dialog rather than an answer.
        note("Nothing has been captured from this terminal.");
        descriptor.buttons.push_back(w::ButtonDescriptor{"&Settings…", w::ButtonRole::Dismiss,
                                                        [this, terminal] {
                                                            show_printer_settings_dialog(terminal);
                                                        }});
        descriptor.buttons.push_back(w::ButtonDescriptor{"Close", w::ButtonRole::Dismiss, {}});
        auto empty = std::make_shared<w::DescriptorDialogPresentation>(
            w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
        retain_dialog(empty);
        return;
    }

    const PrinterButtonModel model = printer_model_for(window);
    if (model.state == proto::PrinterState::Full) {
        // The warning line the interface spec asks for. What was captured is whole and
        // worth opening; what is added is that nothing further will be kept.
        note("The spool is full — these are kept, but nothing more will be.");
        note("");
    }

    const std::size_t choice_field = descriptor.fields.size();
    w::FieldDescriptor choice;
    choice.kind = w::FieldKind::Radio;
    choice.label = jobs.size() == 1 ? "1 capture" : std::to_string(jobs.size()) + " captures";
    for (const proto::PrintJobInfo& job : jobs) choice.options.push_back(print_job_summary(job));
    // The newest, because a reader opening this has almost always just watched
    // it arrive.
    choice.initial_selection = static_cast<int>(jobs.size()) - 1;
    descriptor.fields.push_back(std::move(choice));

    note("");
    note("Saved as " + std::string(print_job_extension(options_.settings.printer_save_format)) +
         " in " + options_.settings.printer_save_folder + ".");

    ckv::term::TerminalSubsession* const source = terminal;
    const std::weak_ptr<void> alive = alive_;
    // Captured by value: the dialog outlives this call, and the server may
    // have discarded a job by the time a button is pressed — an index into a
    // list that has since changed is how the wrong document gets saved.
    const std::vector<proto::PrintJobInfo> listed = jobs;

    descriptor.buttons.push_back(w::ButtonDescriptor{"&Discard all", w::ButtonRole::Dismiss,
                                                    [this, alive, source] {
                                                        if (alive.expired()) return;
                                                        if (window_showing(source) == nullptr)
                                                            return;
                                                        if (options_.discard_print_job)
                                                            options_.discard_print_job(*source, 0);
                                                    }});
    descriptor.buttons.push_back(w::ButtonDescriptor{"&Settings…", w::ButtonRole::Dismiss,
                                                    [this, alive, source] {
                                                        if (alive.expired()) return;
                                                        show_printer_settings_dialog(source);
                                                    }});
    descriptor.buttons.push_back(w::ButtonDescriptor{"&Save", w::ButtonRole::Accept, {}});
    descriptor.buttons.push_back(w::ButtonDescriptor{"Close", w::ButtonRole::Dismiss, {}});

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler(
        [this, presentation, alive, source, listed, choice_field](const w::DialogResult& result) {
            if (alive.expired() || !result.accepted) return;
            if (choice_field >= result.selected.size()) return;
            const int chosen = result.selected[choice_field];
            if (chosen < 0 || static_cast<std::size_t>(chosen) >= listed.size()) return;
            if (window_showing(source) == nullptr) return;  // the terminal ended meanwhile
            const proto::PrintJobInfo& job = listed[static_cast<std::size_t>(chosen)];
            if (job.bytes == 0) {
                // An overflowed job has nothing to write. Saying so beats
                // producing an empty file the reader would later mistake for
                // the document.
                show_printer_problem("That capture was too large to keep, so there is "
                                     "nothing to save.");
                return;
            }
            save_print_job(*source, job.job,
                           options_.settings.printer_save_folder + "/" +
                               print_job_auto_name(0, job.job,
                                                   options_.settings.printer_save_format));
        });
    retain_dialog(presentation);
}

void ClientApp::save_print_job(ckv::term::TerminalSubsession& terminal, std::uint64_t job,
                               const std::string& path) {
    // The text may not be here yet: the payload never travels unasked, so a
    // reader who has not opened this job before is asking for it now and the
    // answer arrives on a later tick.
    const std::string* text =
        options_.print_job_text ? options_.print_job_text(terminal, job) : nullptr;
    if (text == nullptr) {
        if (options_.fetch_print_job) {
            awaiting_print_job_ = job;
            pending_save_path_ = path;
            // The WINDOW showing this terminal, not the terminal itself: a
            // pointer to the subsession would dangle if the reader closed the
            // window while the fetch was in flight, whereas a window that has
            // gone is simply absent from `desktop_->windows()` on the next
            // poll — which makes the liveness check exact rather than hopeful.
            //
            // Job ids are per terminal, so the window is also what stops the
            // resumed save from matching the same id on a different one.
            pending_save_window_ = window_showing(terminal);
            options_.fetch_print_job(terminal, job);
            return;
        }
        show_printer_problem("That capture could not be read back.");
        return;
    }
    const std::string bytes =
        print_job_formatted(*text, options_.settings.printer_save_format);
    if (!options_.write_print_file) {
        show_printer_problem("This build cannot write files.");
        return;
    }
    if (options_.write_print_file(path, bytes)) return;
    // A save that failed silently is the worst of the four outcomes: the
    // reader believes they have the document and finds out when they need it.
    std::string why = "Could not save to " + path + ".";
    if (options_.printer_save_problem) {
        const std::string said = options_.printer_save_problem();
        if (!said.empty()) why += "\n\n" + said;
    }
    show_printer_problem(why);
}

void ClientApp::show_printer_problem(const std::string& what) {
    if (desktop_ == nullptr) return;
    retain_dialog(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
        app_, *desktop_, roles_,
        w::MessageBoxDescriptor{w::MessageBoxKind::Warning, "Print output", what,
                                w::MessageBoxButtons::Ok})));
}

std::vector<WindowPlacement> ClientApp::capture_layout() const {
    std::vector<WindowPlacement> arrangement;
    if (desktop_ == nullptr) return arrangement;
    // One reading of the whole desktop rather than a walk of `windows()` asking
    // each window where it is: `windows()` is INSERTION order (ckVision keeps
    // it deliberately independent of z-order so that window cycling reaches
    // more than the top two), and the stack is what a layout has to state.
    // `snapshot()` walks the children, which is the stack, bottom-most first.
    const w::Desktop::Snapshot snapshot = desktop_->snapshot();
    // And whether what the reader is looking at is a filled, non-overlapping
    // tiling — ckVision's own answer (U4-b), asked once for the whole desktop
    // because that is what it is about: a tiling is a property of the
    // ARRANGEMENT, so either every participating window is in the answer or it
    // is empty. Asking per window would be asking one question n times, and
    // deriving it here a second time would be a rule that could disagree with
    // the one the reader's shadows are drawn from (`child_casts_shadow`).
    const std::vector<w::Desktop::TileFraction> tiling = desktop_->filled_tile_fractions();
    arrangement.reserve(snapshot.windows.size());
    for (const w::Desktop::WindowSnapshot& window : snapshot.windows) {
        const ckv::term::TerminalSubsession* const terminal = terminal_shown_by(window.window);
        if (terminal == nullptr) continue;  // a dialog or the picker, not a terminal
        // `zoomed`, not `maximized()`: the second also answers true for a
        // window whose GROW POLICY keeps it filling the desktop, which is a
        // standing arrangement decision this client makes for itself rather
        // than a place a reader put a window and expects to find it in again.
        WindowPlacement placement{terminal, window.bounds,
                                  static_cast<std::uint16_t>(arrangement.size()), window.zoomed,
                                  TileShare{}};
        for (const w::Desktop::TileFraction& share : tiling)
            if (share.window == window.window)
                placement.tile = TileShare{share.x, share.y, share.width, share.height};
        arrangement.push_back(placement);
    }
    return arrangement;
}

void ClientApp::report_layout_if_settled() {
    if (!options_.report_layout) return;
    std::vector<WindowPlacement> now = capture_layout();
    // Still moving. A drag, a resize, a window sliding under `Tile` — whatever
    // it is, the reader has not finished, and an arrangement reported mid-drag
    // is a position no window ends up in. This is the whole of the debounce:
    // every intermediate frame lands here and goes no further.
    if (now != layout_seen_) {
        layout_seen_ = std::move(now);
        return;
    }
    // Settled, and the same as what the server was last told. Edge-triggered
    // like the server's own announce: a desktop nobody is touching puts nothing
    // on the wire, however long this timer runs.
    if (layout_seen_ == layout_reported_) return;
    layout_reported_ = layout_seen_;
    options_.report_layout(layout_reported_);
}

ckv::Rect ClientApp::restored_bounds(const WindowPlacement& placed, ckv::Rect area) const {
    if (placed.tile.filled()) {
        // The same SHARE of this desktop that the window had of the last one.
        // A 50/50 split stays 50/50 whatever size the reader has reattached at,
        // where replaying the stored cell rect would leave a strip of bare
        // desktop on a wider terminal and an overlap on a narrower one.
        const int left = area.x + tile_edge(placed.tile.x, area.width);
        const int right = area.x + tile_edge(placed.tile.x + placed.tile.width, area.width);
        const int top = area.y + tile_edge(placed.tile.y, area.height);
        const int bottom = area.y + tile_edge(placed.tile.y + placed.tile.height, area.height);
        // A desktop too small to give every tile a whole cell still gets a
        // window per tile: a zero-sized window is not a smaller share, it is
        // one a reader can neither see nor reach.
        return ckv::Rect{left, top, std::max(1, right - left), std::max(1, bottom - top)};
    }

    // An ordinary floating window. Its SIZE is what the reader made it, and
    // nothing below changes that unless they asked for it to be changed.
    //
    // **Several windows that each need moving move independently**, rather than
    // shifting as one group with their relative offsets preserved. The work queue
    // left this open and named independent movement the simpler default; it is
    // also the more defensible one. A group shift is bounded by the WORST
    // window in it, so one oversized window would drag every other window off
    // the place its reader put it — turning one window's problem into
    // everybody's, and moving windows that were already perfectly inside the
    // desktop and needed nothing. And the arrangement in which relative offsets
    // genuinely carry meaning is a deliberate one, which is precisely the
    // filled tiling above: that case is restored as a whole and proportionally,
    // and never reaches this rule. What is left here is windows a reader placed
    // one at a time, which is how they are best put back.
    ckv::Rect rect = placed.rect;
    // The move, which always happens and only ever goes up and left. `min` IS
    // that rule — it can lower a coordinate and never raise one — so a window
    // hanging off the right edge is pulled left by exactly its overhang, one
    // hanging off the bottom is pulled up by its own, and a window already
    // inside is not touched at all.
    rect.x = std::min(rect.x, area.x + area.width - rect.width);
    rect.y = std::min(rect.y, area.y + area.height - rect.height);
    // And no further than the corner. This fires for a window WIDER or TALLER
    // than the desktop, where the shift above would push its own left edge out
    // of sight chasing a right edge that cannot be reached — the top-left is
    // where such a window shows the most of itself. It also catches an
    // arrangement stored on a desktop with less chrome than this one has:
    // toward the content area's origin is the direction that brings a window
    // in, whichever side it was hanging off.
    rect.x = std::max(rect.x, area.x);
    rect.y = std::max(rect.y, area.y);
    // Only now, only if it STILL does not fit, and only if the reader opted in:
    // the largest size that does. A distinct second operation, always after the
    // move and never instead of it — and off by default, because a window too
    // big for a smaller terminal is still the size its reader made it, and
    // reattaching on the bigger one puts it back exactly as it was. Shrinking
    // it would not be undone by anything.
    if (options_.settings.resize_windows_to_fit) {
        rect.width = std::min(rect.width, area.width);
        rect.height = std::min(rect.height, area.height);
    }
    return rect;
}

void ClientApp::apply_layout(const std::vector<WindowPlacement>& arrangement) {
    if (desktop_ == nullptr) return;
    // The desktop as it is NOW, chrome excluded — which is the whole difference
    // between restoring an arrangement and replaying one. Every case below is
    // measured against this rather than against whatever the reader's terminal
    // was the last time they were here.
    const ckv::Rect area = desktop_->content_area();
    // Bottom of the stack first, because the stack is restored by activating
    // each window in turn and activation RAISES: applied in ascending z-order,
    // the last one activated ends up on top, which is where the arrangement
    // says it was. Sorted rather than indexed, since a `z_order` is only ever a
    // comparison with the others in the same statement.
    std::vector<const WindowPlacement*> ordered;
    ordered.reserve(arrangement.size());
    for (const WindowPlacement& placed : arrangement) ordered.push_back(&placed);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const WindowPlacement* left, const WindowPlacement* right) {
                         return left->z_order < right->z_order;
                     });

    bool restored_any = false;
    for (const WindowPlacement* const placed : ordered) {
        if (placed->terminal == nullptr) continue;
        w::Window* const window = window_showing(placed->terminal);
        // A terminal this client has no window for yet — the server naming a
        // place before anything has been opened to put in it. Left unsettled on
        // purpose, so the next statement about it still counts.
        if (window == nullptr) continue;
        // Once per terminal, and after this the window is the reader's. See
        // `layout_settled_`: the server states an arrangement back to the very
        // client that reported it, so a policy applied twice would answer a
        // reader's own drag by moving the window somewhere else.
        if (!layout_settled_.insert(placed->terminal).second) continue;
        // A zero-size rect is the wire's "nobody has ever placed this window"
        // (the protocol spec), not an arrangement with a sizeless window in it. The
        // cascade placement this client already gave it stands — but the server
        // HAS now spoken about this terminal, so it settles like any other and
        // the reader owns it from here.
        if (placed->rect.width <= 0 || placed->rect.height <= 0) continue;
        if (placed->zoomed) {
            // Maximized against the current desktop, which is the entire point:
            // the rect it filled belonged to the terminal it was maximized on.
            // `refresh_zoom_area` for a window that already is one, because a
            // second `toggle_zoom` would RESTORE it — the opposite of what the
            // arrangement asked for.
            if (window->zoomed())
                window->refresh_zoom_area(area);
            else
                window->toggle_zoom(area);
        } else {
            // Un-maximized first, so the rect below is the window's own bounds
            // rather than something recorded underneath a zoom nobody asked to
            // keep.
            if (window->zoomed()) window->toggle_zoom(area);
            window->set_bounds(restored_bounds(*placed, area));
        }
        // Geometry unconditionally; the STACK only when nothing is in front of
        // it. `Desktop::activate` raises a window above every other window, a
        // dialog included — and a dialog is above the arrangement whatever the
        // arrangement covers, which is the same reason one is not a
        // participant in a filled tiling. Restoring a reader's stacking is
        // worth doing; doing it out from under the dialog they are reading is
        // not, and that is not hypothetical: it put a terminal window over the
        // modal asking whether to close that very terminal.
        if (!app_.is_modal()) desktop_->activate(window);
        restored_any = true;
    }
    // Activation left the topmost restored window active, and the keys belong
    // in its terminal rather than on the frame around it — but only if the keys
    // were in a terminal to begin with. A reader part-way through a menu, or
    // answering a dialog, or reading history in copy mode, is not somebody
    // whose focus an arrangement may move: a layout has an opinion about where
    // the windows are and none at all about where somebody is typing.
    if (restored_any && !app_.is_modal() &&
        dynamic_cast<w::TerminalView*>(app_.focused()) != nullptr)
        focus_active_terminal();
}

void ClientApp::focus_active_terminal() {
    if (w::TerminalView* const view = terminal_view_of(active_terminal())) app_.set_focus(view);
}

void ClientApp::keep_keyboard_off_hidden_terminals() {
    // Only a keyboard that is IN a terminal moves. A reader answering a
    // dialog, or reading history in copy mode, or part-way through a menu is
    // not somebody whose focus a window leaving the desktop may take — the
    // same rule `apply_layout` states about arrangements.
    w::TerminalView* const focused = dynamic_cast<w::TerminalView*>(app_.focused());
    if (focused == nullptr || app_.is_modal() || focused->visible_in_tree()) return;
    // Whatever the desktop activated in its place — and NOTHING when every
    // terminal is now minimized, which is a state ckmux already has: a client
    // started without a session focuses nothing, and the prefix key still
    // reaches the command table from there (see the kArmPrefix binding). The
    // alternative is a shell being typed into blind behind a bar.
    app_.set_focus(terminal_view_of(active_terminal()));
    refresh_footer();
}

void ClientApp::focus_terminal_number(int number) {
    if (desktop_ == nullptr) return;
    // The number a reader can SEE wins. Window captions carry ckmux's own
    // terminal number, and the desktop's 1-9 convention is an index into
    // insertion order (ckVision Desktop::select_by_number); the two agree until
    // a window in the middle closes, and after that `^B 2` focused the window
    // captioned "Terminal 3" while "Terminal 2" sat there being ignored.
    //
    // The desktop's own order is still the answer when no window carries the
    // number — a caption a child has renamed, or a terminal past nine — because
    // The interface spec promises "1-9 focus window" and a digit that does nothing at all
    // would be a worse answer than the conventional one.
    for (w::Window* const window : desktop_->windows()) {
        const auto title = titles_.find(window);
        if (title == titles_.end() || title->second.number != number) continue;
        desktop_->activate(window);
        return;
    }
    desktop_->select_by_number(number);
}

Context ClientApp::context() const {
    if (overlay_ != nullptr) return Context::Prefix;
    if (dynamic_cast<w::TerminalView*>(app_.focused()) != nullptr) return Context::Terminal;
    return Context::Desktop;
}

void ClientApp::refresh_footer() {
    if (footer_ == nullptr) return;
    // Copy mode owns the keyboard and its keys are its own — modal motions and
    // a selection, not commands anything else can execute — so the footer says
    // what they are rather than advertising prefix keys that are not reachable
    // from here (the interface spec's context table).
    if (copy_mode_ != nullptr) {
        std::vector<w::StatusLineItem> keys;
        keys.emplace_back("↑↓ scroll", ckv::ui::kInvalidCommand, 90);
        keys.emplace_back("v select", ckv::ui::kInvalidCommand, 80);
        keys.emplace_back("y copy", ckv::ui::kInvalidCommand, 70);
        keys.emplace_back("/ search", ckv::ui::kInvalidCommand, 60);
        keys.emplace_back("q exit", ckv::ui::kInvalidCommand, 50);
        footer_->set_items(std::move(keys));
        return;
    }
    // A window whose program has ended offers two keys and nothing else
    // reachable from inside it (the interface spec). Said before the context table,
    // because the prefix chords the table would advertise are still true but
    // are not what a reader is looking at: they are looking at a dead window
    // and want to know what to do with it.
    //
    // `Enter restart` appears only where something can honour it — a client
    // with no server has nobody to ask — so the footer and the key binding ask
    // the same question and cannot come to disagree.
    if (w::Window* const active = desktop_ == nullptr ? nullptr : desktop_->active_window();
        child_has_ended(terminal_view_of(active))) {
        std::vector<w::StatusLineItem> keys;
        if (options_.request_respawn)
            keys.emplace_back("Enter restart", ckv::ui::kInvalidCommand, 90);
        keys.emplace_back("x close", ckv::ui::kInvalidCommand, 80);
        footer_->set_items(std::move(keys));
        return;
    }
    const Context current = context();
    std::vector<w::StatusLineItem> items;
    // What the terminals a reader is NOT looking at have been doing
    // (the interface spec's status area), at the highest priority in the line: a
    // bell dropped when the window narrows is a bell that did not happen as
    // far as the reader is concerned, and these are two glyphs.
    //
    // The counts are of WINDOWS wanting attention, which is the actionable
    // number — "eleven bells" is not — and they are read off what the title
    // poll decided rather than recomputed. There used to be a second copy of
    // the "is this the active window" rule here, and a mutation showed why
    // that is a trap: inverting the poll's copy changed nothing any test
    // could see, because every assertion read the footer and the footer had
    // its own. One behaviour, one place.
    {
        int bells = 0;
        int busy = 0;
        for (const auto& [flagged, title] : titles_) {
            (void)flagged;
            if (title.lit) ++bells;
            if (title.noted_activity) ++busy;
        }
        if (bells > 0)
            items.emplace_back("\u2022 " + std::to_string(bells), ckv::ui::kInvalidCommand, 100);
        if (busy > 0)
            items.emplace_back("! " + std::to_string(busy), ckv::ui::kInvalidCommand, 99);
    }
    // Who else is in here, and whether this reader may type (WP-50). High in
    // the order for the reason the marks above are: a toast expires and these
    // facts do not, and a reader who cannot see that somebody else is typing
    // into the terminal in front of them will blame the program.
    //
    // "read-only" outranks the count, because it is the one that changes what
    // the next keystroke does.
    if (watching()) items.emplace_back("read-only", ckv::ui::kInvalidCommand, 98);
    if (const int readers = readers_here(); readers > 1)
        items.emplace_back(std::to_string(readers) + " readers", ckv::ui::kInvalidCommand, 97);
    for (const KeyBinding* binding : keymap_.footer(current)) {
        // In the prefix context the reader has already pressed the prefix, so
        // the bare key is what they need to see next; otherwise the full
        // "^B c" is the reachable spelling. Either way the key is stated
        // rather than derived: several of these commands are ckVision's own,
        // whose registered chords (F6, F10) are consumed by the child
        // program while a terminal has focus and so would advertise a key
        // that never arrives.
        const std::string key = current == Context::Prefix
                                    ? std::string(binding->chord)
                                    : binding_label(options_.settings.prefix, *binding);
        items.emplace_back(w::CommandPresentation{binding->command, std::string(binding->hint), key},
                           binding->footer_priority);
    }
    footer_->set_items(std::move(items));
}

std::vector<std::string> ClientApp::footer_labels() const {
    std::vector<std::string> labels;
    if (footer_ == nullptr) return labels;
    // Composed the same way ckVision's StatusLine composes it, so a test
    // reads what the reader reads without scraping painted cells.
    for (const w::StatusLineItem& item : footer_->items()) {
        const std::string text = item.presentation.label.empty() ? item.label : item.presentation.label;
        labels.push_back(item.presentation.chord.empty() ? text
                                                         : item.presentation.chord + " " + text);
    }
    return labels;
}

void ClientApp::arm_prefix() {
    if (overlay_ != nullptr || desktop_ == nullptr) return;
    // The key table as it stands, handed over as values: the popup shows
    // what a reader's rebinding actually did, and does not have to reach back
    // into the client to find out.
    std::vector<std::pair<std::string, std::string>> rows;
    for (const KeyBinding* binding : keymap_.footer(Context::Prefix))
        rows.emplace_back(binding->chord, std::string(binding->hint));
    auto overlay = std::make_unique<PrefixOverlay>(options_.settings.prefix, std::move(rows), roles_);
    overlay->on_chord = [this](std::string chord) {
        // The overlay is mid-callback inside its own on_key; unwinding first
        // keeps its teardown off the stack that is still running it.
        app_.post([this, chord = std::move(chord)] { resolve_prefix(chord); });
    };
    overlay_ = desktop_->add_popup(std::move(overlay));
    overlay_->set_bounds(overlay_->preferred_bounds(view_content_area()));  // the reader's eyes, not the world's origin (WP-43)
    app_.set_focus(overlay_);
    which_key_timer_ = app_.start_timer(options_.which_key_delay_nanos, /*repeating=*/false, [this] {
        which_key_timer_ = 0;
        if (overlay_ == nullptr) return;
        overlay_->expand();
        overlay_->set_bounds(overlay_->preferred_bounds(view_content_area()));  // the reader's eyes, not the world's origin (WP-43)
    });
    refresh_footer();
}

void ClientApp::dismiss_prefix_overlay() {
    if (which_key_timer_ != 0) {
        app_.cancel_timer(which_key_timer_);
        which_key_timer_ = 0;
    }
    if (overlay_ == nullptr) return;
    PrefixOverlay* const overlay = overlay_;
    overlay_ = nullptr;
    desktop_->remove_popup(overlay);
}

void ClientApp::resolve_prefix(const std::string& chord) {
    dismiss_prefix_overlay();
    // Focus returns to the terminal first, so a command that acts on "the
    // focused terminal" sees the same window the reader was looking at when
    // they pressed the prefix.
    focus_active_terminal();

    if (!chord.empty()) {
        // The prefix pressed twice reaches send-prefix through the same table
        // as every other chord: its row is seeded with the prefix's own
        // spelling at startup, so a reader's rebinding — of it, or of the
        // chord — is honoured here like anywhere else.
        if (const int index = digit_chord(chord); index != 0) {
            focus_terminal_number(index);
            focus_active_terminal();
        } else if (const KeyBinding* const binding = keymap_.find(chord)) {
            app_.execute_command(binding->command);
        }
    }
    refresh_footer();
}

void ClientApp::set_theme(u::Theme theme) {
    app_.theme() = std::move(theme);
    app_.invalidate_all();
}

void ClientApp::show_key_reference() { app_.execute_command(app_.commands().standard().help); }

void ClientApp::fit_desktop_to_screen() {
    if (desktop_ == nullptr || !options_.fit_session_desktop) return;
    // The whole screen, not `content_area()`: the session's desktop INCLUDES
    // the rows this client spends on its own chrome, because another reader's
    // chrome may be a different height — a collapsed status bar (WP-35) must
    // not make the shared world one row shorter for everybody.
    options_.fit_session_desktop(ckv::Size{desktop_->bounds().width, desktop_->bounds().height});
    notify("Session desktop is now " + std::to_string(desktop_->bounds().width) + "×" +
           std::to_string(desktop_->bounds().height));
}

void ClientApp::show_all_keys() {
    // Straight to the topic rather than through the standard help command,
    // which answers with the page for whatever has focus: this item exists
    // precisely to be the page that does NOT depend on where the reader is.
    if (desktop_ == nullptr) return;
    retain_dialog(std::make_shared<w::HelpViewerPresentation>(
        w::present_help_viewer(help_, "ckmux.keys.all", app_, *desktop_, roles_)));
}

void ClientApp::show_about() {
    if (desktop_ == nullptr) return;
    // The attribution line is CK Office's, character for character: the suite's
    // shared cworks::about_lines() closes every one of its About boxes with
    // this sentence, spelled "(c)" rather than the sign. Programs from the same
    // hand that word their own authorship differently read as programs from
    // different hands, so this one follows the suite rather than deciding
    // again. Last, after a blank line, where the suite puts it too.
    retain_dialog(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
        app_, *desktop_, roles_,
        w::MessageBoxDescriptor{w::MessageBoxKind::Info, "About ckmux",
                                std::string("ckmux ") + kVersion +
                                    "\nA terminal multiplexer with a visible interface.\n\n"
                                    "Built on ckVision.\n\n"
                                    "(c) 2026 by Dr. Christian Klukas",
                                w::MessageBoxButtons::Ok})));
}

void ClientApp::show_terminal_report() {
    if (desktop_ == nullptr) return;
    // ckVision's own dialog (L-25): the report describes the OUTER host as
    // this application detected it. The probe is the host's when the host
    // gave one (main.cpp), and the line is simply absent under a test's
    // HeadlessTerminal — which has no byte stream to count.
    ckv::widgets::TerminalReportDialogOptions options;
    options.mouse_reports_decoded = options_.mouse_reports_probe;
    retain_dialog(std::make_shared<w::TerminalReportDialogPresentation>(
        w::present_terminal_report_dialog(*desktop_, app_, roles_, std::move(options))));
}

void ClientApp::enter_copy_mode() {
    if (copy_mode_ != nullptr || desktop_ == nullptr) return;
    w::Window* const window = active_terminal();
    w::TerminalView* const view = terminal_view_of(window);
    if (view == nullptr) return;

    // The history is read once, here, and does not move again: a program that
    // keeps printing while its reader is reading must not shift the line they
    // are about to select out from under them (client/copy_mode.hpp).
    const ckv::core::TerminalSnapshot snapshot = view->session().snapshot();
    std::vector<CopyLine> lines = compose_history(snapshot);
    // Opening at the bottom, where the reader was looking, rather than at the
    // top of a history that may be ten thousand lines old.
    const int start = std::max(0, static_cast<int>(lines.size()) - 1);

    auto surface = std::make_unique<CopyModeView>(std::move(lines), start, roles_);
    surface->on_copy = [this](std::string text) { copy_to_targets(std::move(text)); };
    surface->on_exit = [this] {
        // Posted, because this runs inside the surface's own key handler and
        // leaving destroys it.
        app_.post([this] { leave_copy_mode(); });
    };
    copy_mode_ = desktop_->add_popup(std::move(surface));
    // F1 here is about copy mode, not about the terminal underneath it: its
    // keys are its own, they are fixed rather than bindable, and a reader who
    // asks for help while reading history is asking about what they are doing
    // now (WP-14).
    copy_mode_->set_help_context_key("ckmux.copy");
    copy_mode_window_ = window;
    // Beside the pointer, the proof it is still this window: the caption
    // refresh below runs on a timer, and the window can be taken down under it
    // by a server that ends the terminal while its reader is reading.
    copy_mode_window_alive_ = window->lifetime_token();
    copy_mode_->set_bounds(window->content_rect());
    app_.set_focus(copy_mode_);
    refresh_copy_mode_caption();
    refresh_footer();
}

void ClientApp::leave_copy_mode() {
    if (copy_mode_ == nullptr || desktop_ == nullptr) return;
    CopyModeView* const surface = copy_mode_;
    copy_mode_ = nullptr;
    desktop_->remove_popup(surface);
    // The caption goes back only if there is still a window to put it on: copy
    // mode outlives its window whenever the server ends the terminal underneath
    // it, and an expired token is the proof of exactly that.
    if (copy_mode_window_ != nullptr && !copy_mode_window_alive_.expired()) {
        const auto title = titles_.find(copy_mode_window_);
        if (title != titles_.end()) copy_mode_window_->set_title(effective_title(title->second));
    }
    copy_mode_window_ = nullptr;
    copy_mode_window_alive_.reset();
    focus_active_terminal();
    refresh_footer();
}

void ClientApp::refresh_copy_mode_caption() {
    // The badge and the position, on the window's own caption, because that is
    // where a reader looks to find out which window they are in and how far
    // back (the interface spec). Restored on exit from the same store the title poll
    // reads, so a caption cannot be left saying COPY.
    if (copy_mode_ == nullptr || copy_mode_window_ == nullptr) return;
    // Nothing is written through a window that has been destroyed. This runs on
    // the title timer, so "the window went while copy mode was up" is not a
    // hypothetical race — it is a tenth of a second wide, every tenth of a
    // second, for as long as a reader stays in copy mode.
    if (copy_mode_window_alive_.expired()) return;
    const auto title = titles_.find(copy_mode_window_);
    const std::string base = title == titles_.end() ? copy_mode_window_->title()
                                                   : effective_title(title->second);
    const int from_bottom = copy_mode_->cursor().y - (copy_mode_->history_size() - 1);
    copy_mode_window_->set_title(base + " — COPY " + std::to_string(from_bottom) + "/" +
                                 std::to_string(copy_mode_->history_size()));
}

void ClientApp::copy_to_targets(std::string text) {
    if (text.empty()) return;
    // ckmux's own clipboard always gets it: `^B ]` has to work whether or not
    // anything outside this process would accept the text, which over SSH is
    // the ordinary case.
    internal_clipboard_ = text;
    // The targets that would not take it. A copy that silently reaches nothing
    // is the failure a reader discovers later, in another program, by pasting
    // something else entirely — so the ones that refused are collected and
    // said once, at the moment they refused.
    std::vector<std::string> refused;
    for (const ClipboardTarget& target : options_.settings.clipboard) {
        switch (target.kind) {
            case ClipboardTarget::Kind::Osc52:
                // The outer terminal, through ckVision's clipboard writer —
                // which emits OSC 52 and therefore works over a connection.
                app_.set_clipboard_text(text);
                break;
            case ClipboardTarget::Kind::Pbcopy:
                if (options_.clipboard_writer && !options_.clipboard_writer("pbcopy", text))
                    refused.emplace_back("pbcopy");
                break;
            case ClipboardTarget::Kind::Exec:
                if (options_.clipboard_writer && !options_.clipboard_writer(target.command, text))
                    refused.push_back(target.command);
                break;
        }
    }
    if (!refused.empty()) report_clipboard_problem(refused);
}

void ClientApp::show_server_error(std::uint16_t code, const std::string& context,
                                  const std::string& human) {
    if (desktop_ == nullptr) return;
    // The code and the context as well as the sentence, because those two are
    // what a bug report can be written from: "NoSuchTerminal (2) in
    // MoveTerminal" says which request failed and how, where the sentence alone
    // says only what a reader could already see.
    std::string body = human.empty() ? std::string("The server could not do that.") : human;
    if (!context.empty()) body += "\n\nRequest: " + context;
    body += "\nCode: " + std::to_string(static_cast<unsigned>(code));
    retain_dialog(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
        app_, *desktop_, roles_,
        w::MessageBoxDescriptor{w::MessageBoxKind::Warning, "ckmux server", std::move(body),
                                w::MessageBoxButtons::Ok})));
}

void ClientApp::report_clipboard_problem(const std::vector<std::string>& refused) {
    if (desktop_ == nullptr || refused.empty()) return;
    std::string body;
    if (refused.size() == 1) {
        body = "The copy did not reach " + refused.front() + ".";
    } else {
        body = "The copy did not reach these clipboard helpers:";
        for (const std::string& one : refused) body += "\n  " + one;
    }
    // What the helper itself said, which is the difference between "a copy
    // failed" and "xclip is not installed". It is captured rather than
    // inherited precisely so it can be shown here instead of landing in the
    // middle of a drawn frame (platform/clipboard.hpp).
    if (options_.clipboard_problem) {
        const std::string said = options_.clipboard_problem();
        if (!said.empty()) body += "\n\n" + said;
    }
    // The part that stops this being only bad news: the text is not lost. The
    // chord comes from the one table every surface reads, so a reader who
    // rebound paste is told the key they actually have.
    std::string paste_chord;
    for (const KeyBinding& binding : keymap_.bindings())
        if (binding.key == commands::kPaste && !binding.chord.empty())
            paste_chord = binding_label(options_.settings.prefix, binding);
    body += paste_chord.empty()
                ? "\n\nckmux kept it — Terminal ▸ Paste puts it into a terminal."
                : "\n\nckmux kept it — " + paste_chord + " pastes it into a terminal.";
    retain_dialog(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
        app_, *desktop_, roles_,
        w::MessageBoxDescriptor{w::MessageBoxKind::Warning, "Copy", std::move(body),
                                w::MessageBoxButtons::Ok})));
}

void ClientApp::paste_into_terminal() {
    if (internal_clipboard_.empty()) return;
    w::TerminalView* const view = terminal_view_of(active_terminal());
    if (view == nullptr) return;
    std::string bytes =
        paste_bytes(internal_clipboard_, view->session().snapshot().bracketed_paste_enabled);
    // Credit-paced where there is a wire to pace (WP-18), and straight into
    // the PTY where there is not. A local terminal needs none of this: the
    // write is to a pipe this process owns, and the kernel's own buffer is
    // the flow control.
    // Asked of the handle THIS client made, not of the view's: a
    // `TerminalView` answers with `core::TerminalSubsession`, the base every
    // terminal is, while a wire id belongs to the `term::` subsession ckmux
    // holds — and the two are related by inheritance, so the base cannot
    // stand in for the derived one.
    ckv::term::TerminalSubsession* const terminal = terminal_shown_by(active_terminal());
    if (terminal != nullptr && options_.paste_credited &&
        options_.paste_credited(*terminal, bytes))
        return;
    view->session().send_input(std::move(bytes));
}

void ClientApp::show_settings() {
    if (desktop_ == nullptr) return;
    // Labelled for a reader who has never heard the phrase "login shell" and
    // should not have to: what they can check is whether their terminals
    // behave like the ones they open everywhere else, and the note says which
    // files that turns on and when the change shows up.
    w::DialogDescriptor descriptor;
    descriptor.title = "Settings";
    const std::size_t login_field = descriptor.fields.size();
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "&Start terminals the way your desktop terminal does",
                           .kind = w::FieldKind::Check,
                           .initial_checked = options_.settings.login_shell});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  Runs ~/.zprofile and ~/.profile, so a terminal here has the Dock's PATH.",
                           .kind = w::FieldKind::Note});

    // The picture limit and the grace, then ONE paragraph answering both — and
    // the shape is a vertical budget rather than a preference. This dialog has
    // to keep fitting a 24-row terminal, which leaves it 22 rows, and it was
    // already asking for all of them: a `Column` puts a blank row between every
    // item, so each field and each note paragraph costs two rows, not one.
    // Consecutive notes are ONE item (ckVision groups them into a paragraph
    // with no spacing), so merging these two notes into one paragraph pays for
    // the checkbox below exactly, with nothing a reader was told left out. The
    // lines stay in field order, so each still answers the field above it.
    //
    // Each unit is in its label, because a bare number in a field is a question
    // ("of what?"), and "a new terminal" says the other thing a reader has to
    // know: changing these does nothing to the terminals already open.
    const std::size_t sixel_field = descriptor.fields.size();
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "&Largest picture a new terminal may draw (megapixels):",
                           .initial_text = std::to_string(options_.settings.sixel_max_megapixels),
                           .validate = [](const std::string& text) {
                               return ckm::parse_sixel_megapixels(text).has_value();
                           },
                           .kind = w::FieldKind::Text});

    const std::size_t grace_field = descriptor.fields.size();
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "&Give programs this long to quit (seconds):",
                           .initial_text = std::to_string(options_.settings.kill_grace_seconds),
                           .validate = [](const std::string& text) {
                               return ckm::parse_int_in_range(text, 0, 600).has_value();
                           },
                           .kind = w::FieldKind::Text});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  Pictures larger than the window are cut off at its edge (" +
                                    std::to_string(ckm::kSixelMegapixelsMin) + "-" +
                                    std::to_string(ckm::kSixelMegapixelsMax) + ").",
                           .kind = w::FieldKind::Note});
    descriptor.fields.push_back(w::FieldDescriptor{
        .label = "  Ending a session kills nothing unless you tick that dialog's box.",
        .kind = w::FieldKind::Note});

    // What a reattach does with a window too big for the terminal the reader
    // has come back on (WP-30). No note of its own: the label says when it
    // happens and that moving comes first, which is the whole setting, and a
    // row spent here is a row the fields above would have to give up.
    //
    // `&R`, not `&S`: `&Start` above and the `&Save` button already hold S, and
    // two claims on one mnemonic is a key that does whichever the walk reaches
    // first.
    const std::size_t fit_field = descriptor.fields.size();
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "&Resize oversized windows on reattach if moving is not enough",
                           .kind = w::FieldKind::Check,
                           .initial_checked = options_.settings.resize_windows_to_fit});

    // The clock and the theme: chrome rather than terminal behaviour, which is
    // why they are last. The settings above wait for a new terminal — their own
    // labels say so — while these two change as the dialog closes.
    //
    // Combo boxes rather than radio groups, although three visible
    // alternatives would otherwise be a radio group's job: two of those cost
    // eight rows, and this dialog has to keep fitting a 24-row terminal, which
    // is a real size for a multiplexer to be run in.
    const std::size_t clock_field = descriptor.fields.size();
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "Cloc&k in the menu bar:",
                           .kind = w::FieldKind::Combo,
                           .options = choice_labels(kClockChoices),
                           .initial_selection = choice_index(kClockChoices, options_.settings.clock)});
    const std::size_t theme_field = descriptor.fields.size();
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "Colour &theme:",
                           .kind = w::FieldKind::Combo,
                           .options = choice_labels(kThemeChoices),
                           .initial_selection = choice_index(kThemeChoices, options_.settings.theme)});
    // Roles rather than a bool: Save is the default button and validates,
    // Cancel dismisses without validating or carrying values — which is what
    // Esc does, and saying so here is how the two stay the same thing.
    descriptor.buttons.push_back(w::ButtonDescriptor{"&Save", w::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back(w::ButtonDescriptor{"&Cancel", w::ButtonRole::Dismiss, nullptr});
    descriptor.minimum_window_size = ckv::Size{72, 22};
    descriptor.button_alignment = u::Alignment::End;
    descriptor.anchor_buttons_to_bottom = true;

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler(
        [this, login_field, sixel_field, grace_field, fit_field, clock_field, theme_field](
            w::DialogResult result) {
            if (!result.accepted || result.checked.size() <= fit_field ||
                result.values.size() <= sixel_field || result.selected.size() <= theme_field)
                return;
            // One report between them: a home directory that cannot be written
            // to fails every save, and being told so four times is being told
            // so once with three extra dialogs in the way.
            bool stored = apply_login_shell(result.checked[login_field]);
            // The field validator has already refused anything unreadable, so a
            // value that arrives here parses; the check is what makes that a
            // fact rather than an assumption.
            if (const std::optional<int> megapixels =
                    ckm::parse_sixel_megapixels(result.values[sixel_field]))
                stored = apply_sixel_max_megapixels(*megapixels) && stored;
            if (result.values.size() > grace_field)
                if (const std::optional<int> seconds =
                        ckm::parse_int_in_range(result.values[grace_field], 0, 600))
                    stored = apply_kill_grace_seconds(*seconds) && stored;
            stored = apply_resize_windows_to_fit(result.checked[fit_field]) && stored;
            stored = apply_clock_mode(choice_value(kClockChoices, result.selected[clock_field],
                                                   options_.settings.clock)) &&
                     stored;
            stored = apply_theme(choice_value(kThemeChoices, result.selected[theme_field],
                                              options_.settings.theme)) &&
                     stored;
            if (!stored) report_settings_not_saved();
        });
    retain_dialog(presentation);
}

void ClientApp::toggle_stats_readout(bool Settings::* field, std::string_view key) {
    options_.settings.*field = !(options_.settings.*field);
    // Persisted the moment it changes, through the same atomic write the
    // Settings dialog's checkboxes use — the preference survives the session
    // because the reader flipped it, not because they remembered a file. Live
    // either way; a save that cannot happen is said out loud (the configuration spec).
    if (!ckm::save_setting(ckm::platform::config_file_path(), "general", std::string(key),
                           ckm::bool_setting(options_.settings.*field)))
        report_settings_not_saved();
    apply_stats_toggles();
}

void ClientApp::refresh_menu_marks() {
    // A checkmark is a value in the bar's items rather than a predicate the bar
    // asks about, so a toggle that changed one has to hand the bar fresh items
    // — the same call `apply_stats_toggles` makes for the View menu.
    if (w::MenuBar* const bar = menu_bar()) bar->set_menus(build_menus());
}

void ClientApp::apply_stats_toggles() {
    const StatsToggles toggles = stats_toggles();
    // Every open terminal window at once, from whatever numbers are current —
    // and all-off restores today's frame, because an empty footer draws
    // nothing.
    for (const auto& [terminal, window] : terminal_windows_)
        refresh_stats_footer(*window, *terminal);
    // A checkmark is a value in the bar's items, so the bar gets fresh items.
    if (w::MenuBar* const bar = menu_bar()) bar->set_menus(build_menus());
    // The server samples only while somebody watches (WP-38); tell it when
    // the answer changes. A serverless client has nobody to tell and samples
    // for itself below, at the same one-second cadence.
    if (options_.watch_stats) options_.watch_stats(toggles.any());
    // The local sampler asks the platform before arming: a timer ticking
    // every second to take dead samples is exactly the waste the predicate
    // exists to refuse. The wire subscription above is deliberately not
    // gated — the server answers for its own platform, not for this one.
    if (platform::process_stats_supported() && toggles.any() && stats_timer_ == 0) {
        stats_timer_ =
            app_.start_timer(1'000'000'000, /*repeating=*/true, [this] { sample_local_stats(); });
        // "Within one sample period" means now for the first one: a reader
        // who just ticked the box is looking at the frame it lands on.
        sample_local_stats();
    } else if (!toggles.any() && stats_timer_ != 0) {
        app_.cancel_timer(stats_timer_);
        stats_timer_ = 0;
        local_cpu_.clear();
    }
}

void ClientApp::refresh_stats_footer(w::Window& window,
                                     const ckv::term::TerminalSubsession& terminal) {
    const auto found = latest_stats_.find(&terminal);
    window.set_footer(stats_footer(
        found == latest_stats_.end() ? proto::TermStats{} : found->second, stats_toggles()));
}

ckv::Rect ClientApp::view_content_area() const {
    const ckv::Rect area = desktop_->content_area();
    const ckv::Size extent = desktop_->extent();
    if (extent.width <= 0 || extent.height <= 0) return area;
    // The same arithmetic the library's own pan clamp uses: the view's height
    // is the desktop's bounds minus the docked chrome, and the pan offsets
    // within the content rows, based at the content area's own top.
    const ckv::widgets::View* const bottom_dock = desktop_->bottom_dock();
    const int bottom_rows =
        bottom_dock != nullptr ? std::max(1, bottom_dock->vertical_size_hint().preferred) : 0;
    const int view_width = std::min(area.width, desktop_->bounds().width);
    const int view_height =
        std::max(0, std::min(area.height, desktop_->bounds().height - area.y - bottom_rows));
    const ckv::Point pan = desktop_->pan();
    return ckv::Rect{pan.x, area.y + pan.y, view_width, view_height};
}

void ClientApp::set_session_desktop(ckv::Size world) {
    if (desktop_ == nullptr) return;
    // Zeros are "no server has said", and silence does not shrink a world —
    // the extent keeps whatever the session last stated until a session
    // states otherwise or this client leaves (forget_terminals clears it).
    if (world.width <= 0 || world.height <= 0) return;
    desktop_->set_extent(world);
    // The pan is re-clamped by set_extent itself; what is left is the reader:
    // if the active window fell outside a world that just changed shape,
    // follow it exactly as an activation would have.
    if (w::Window* const active = desktop_->active_window())
        desktop_->pan_to_show(active->bounds());
}

void ClientApp::receive_terminal_stats(const ckv::term::TerminalSubsession& terminal,
                                       const proto::TermStats& stats) {
    latest_stats_[&terminal] = stats;
    if (w::Window* const window = window_showing(&terminal))
        refresh_stats_footer(*window, terminal);
}

void ClientApp::sample_local_stats() {
    const StatsToggles toggles = stats_toggles();
    if (!toggles.any()) return;
    // At most one table per pass, and none at all for a purely remote client:
    // a mirror answers -1 through the seam (U5-b) and its numbers arrive over
    // the wire, so a client watching only mirrors does no sampling work here
    // — the same nobody-watching-nobody-sampling rule the server keeps.
    std::optional<platform::ProcessTable> table;
    const std::int64_t now = app_.clock().now_nanos();
    for (const auto& [terminal, window] : terminal_windows_) {
        const int root = terminal->process_id();
        if (root < 0) {
            // A mirror — or a local child that has gone, and only the latter
            // holds a baseline of ours and a readout to clear. Cleared once:
            // erasing the baseline is what makes the next pass skip it.
            const auto base = local_cpu_.find(terminal);
            if (base != local_cpu_.end()) {
                local_cpu_.erase(base);
                latest_stats_[terminal] = proto::TermStats{};
                refresh_stats_footer(*window, *terminal);
            }
            continue;
        }
        if (!table) table = platform::ProcessTable::snapshot();
        const platform::TreeSample sample = platform::sample_tree(*table, root);
        proto::TermStats stats;
        if (sample.process_count > 0) {
            stats.rss_bytes = sample.rss_bytes;
            stats.real_bytes = sample.real_bytes;
            stats.flags = static_cast<std::uint8_t>(proto::TermStatsFlag::Alive);
            if (sample.has_real)
                stats.flags |= static_cast<std::uint8_t>(proto::TermStatsFlag::HasReal);
            LocalCpuBaseline& base = local_cpu_[terminal];
            if (base.primed && now > base.at_nanos && sample.cpu_time_nanos >= base.cpu_nanos) {
                const std::uint64_t wall = static_cast<std::uint64_t>(now - base.at_nanos);
                stats.cpu_permille = static_cast<std::uint32_t>(
                    (sample.cpu_time_nanos - base.cpu_nanos) * 1000u / wall);
            }
            base.cpu_nanos = sample.cpu_time_nanos;
            base.at_nanos = now;
            base.primed = true;
        } else {
            local_cpu_.erase(terminal);
        }
        latest_stats_[terminal] = stats;
        refresh_stats_footer(*window, *terminal);
    }
}

bool ClientApp::apply_login_shell(bool login_shell) {
    if (options_.settings.login_shell == login_shell) return true;
    options_.settings.login_shell = login_shell;
    // Stored where the reader can also edit it by hand, and where it will be
    // found again next time (the configuration spec). A save that cannot
    // happen is said out loud rather than swallowed: the setting is live
    // either way for this session, but a reader who was not told would find
    // it reverted tomorrow with no idea why. Saying so is the caller's, so
    // that two settings changed together cost one explanation.
    return ckm::save_setting(ckm::platform::config_file_path(), "general", "login-shell",
                             ckm::bool_setting(login_shell));
}

bool ClientApp::apply_kill_grace_seconds(int seconds) {
    if (options_.settings.kill_grace_seconds == seconds) return true;
    options_.settings.kill_grace_seconds = seconds;
    return ckm::save_setting(ckm::platform::config_file_path(), "general", "kill-grace-seconds",
                             std::to_string(seconds));
}

bool ClientApp::apply_resize_windows_to_fit(bool resize) {
    if (options_.settings.resize_windows_to_fit == resize) return true;
    options_.settings.resize_windows_to_fit = resize;
    // Nothing on screen moves. This decides what the NEXT reattach does with a
    // window that no longer fits, and re-laying the desktop out the moment a
    // reader ticked a box would be the setting doing something it does not
    // claim to do (WP-30).
    return ckm::save_setting(ckm::platform::config_file_path(), "general",
                             "resize-windows-to-fit", ckm::bool_setting(resize));
}

bool ClientApp::apply_sixel_max_megapixels(int megapixels) {
    if (options_.settings.sixel_max_megapixels == megapixels) return true;
    options_.settings.sixel_max_megapixels = megapixels;
    return ckm::save_setting(ckm::platform::config_file_path(), "terminal", "sixel-max-megapixels",
                             std::to_string(megapixels));
}

bool ClientApp::apply_clock_mode(ClockMode mode) {
    if (options_.settings.clock == mode) return true;
    options_.settings.clock = mode;
    // Immediately, unlike the two settings above it: the clock is chrome the
    // reader is looking at while they choose, so the dialog closing onto an
    // unchanged bar would read as a setting that did not take.
    install_clock();
    return ckm::save_setting(ckm::platform::config_file_path(), "general", "clock",
                             ckm::clock_setting(mode));
}

bool ClientApp::apply_theme(Theme theme) {
    if (options_.settings.theme == theme) return true;
    options_.settings.theme = theme;
    switch (theme) {
        case Theme::Dark: set_theme(u::make_dark_theme(app_.roles(), roles_)); break;
        case Theme::Light: set_theme(u::make_light_theme(app_.roles(), roles_)); break;
        case Theme::Mono: set_theme(u::make_mono_theme(app_.roles(), roles_)); break;
    }
    // Stored, which is the whole reason this moved out of a View menu: a theme
    // chosen for one session and forgotten by the next is a theme a reader has
    // to choose every morning.
    return ckm::save_setting(ckm::platform::config_file_path(), "general", "theme",
                             ckm::theme_setting(theme));
}

void ClientApp::report_settings_not_saved() {
    if (desktop_ == nullptr) return;
    const std::filesystem::path path = ckm::platform::config_file_path();
    retain_dialog(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
        app_, *desktop_, roles_,
        w::MessageBoxDescriptor{
            w::MessageBoxKind::Warning, "Settings not saved",
            "The change applies to this session, but could not be written to\n" +
                (path.empty() ? std::string("a configuration file — no HOME is set.")
                              : path.string() + "\n\nIt will be back to its old value next time."),
            w::MessageBoxButtons::Ok})));
}

void ClientApp::report_config_warnings() {
    if (desktop_ == nullptr || options_.config_warnings.empty()) return;
    // Said once, at the start, naming the lines. A reader who mistyped a key
    // otherwise gets a ckmux that quietly ignores what they wrote — which
    // looks exactly like a ckmux that does not have the setting at all.
    //
    // The file is named once, in the sentence, and struck off the entries
    // underneath. Each warning carries its whole path so that a reader with a
    // `$CKMUX_CONFIG` beside their real file can tell two "ckmux.conf:4"s
    // apart (config.cpp) — right at a terminal, and wrong down the left margin
    // of a dialog that has to fit a 72-column window, where it spends on the
    // same path over and over the width each message needs to be readable.
    const std::string file = common_config_file(options_.config_warnings);
    std::string body = "ckmux started, but part of ";
    body += file.empty() ? std::string("your configuration") : file;
    body += " was not understood:\n";
    for (const std::string& warning : options_.config_warnings)
        body += "\n  " + without_prefix(warning, file);
    retain_dialog(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
        app_, *desktop_, roles_,
        w::MessageBoxDescriptor{w::MessageBoxKind::Warning, "Configuration", std::move(body),
                                w::MessageBoxButtons::Ok})));
}

void ClientApp::populate_help() {
    // Generated from the keymap table, never hand-copied: a rebinding changes
    // this page as a matter of course (the interface spec).
    std::string keys;
    for (const KeyBinding& binding : keymap_.bindings()) {
        if (binding.chord.empty()) continue;
        std::string label = binding_label(options_.settings.prefix, binding);
        label.resize(std::max<std::size_t>(label.size(), 8), ' ');
        keys += label + "  " + std::string(binding.hint) + "\n";
    }

    // The complete listing (WP-14): every command this ckmux has, chorded or
    // not, with the reader's own rebindings applied AND marked. Three groups,
    // because a reader asking "what can I press" and a reader asking "did my
    // configuration take" are asking different questions of the same table.
    //
    // Grouped by what REACHES a command rather than by binding context: the
    // config file has five context names and only `terminal` is honoured
    // today (the configuration spec refuses the rest out loud), so grouping by context would
    // print four empty headings and one full one — a shape that says the
    // opposite of what is true.
    std::string all_keys;
    std::string rebound;
    std::string menu_only;
    for (const KeyBinding& binding : keymap_.bindings()) {
        const bool moved = binding.chord != binding.default_chord;
        if (binding.chord.empty()) {
            std::string row = "  " + std::string(binding.hint);
            if (moved)
                row += "  — you unbound " + prefix_label(options_.settings.prefix) + " " +
                       binding.default_chord;
            menu_only += row + "\n";
            continue;
        }
        std::string label = binding_label(options_.settings.prefix, binding);
        label.resize(std::max<std::size_t>(label.size(), 8), ' ');
        const std::string row = label + "  " + std::string(binding.hint) + "\n";
        all_keys += row;
        if (moved) {
            // What their file changed, and what it changed FROM — the half a
            // listing usually leaves out, and the half a reader needs when a
            // key they have used for months has stopped working.
            std::string was = binding.default_chord.empty()
                                  ? std::string("nothing")
                                  : prefix_label(options_.settings.prefix) + " " +
                                        binding.default_chord;
            rebound += "  " + std::string(binding.hint) + ": now " +
                       binding_label(options_.settings.prefix, binding) + ", was " + was + "\n";
        }
    }
    const std::string prefix_text = prefix_label(options_.settings.prefix);

    // Prose is written as paragraphs and left to the viewer to wrap, which it
    // does to whatever width the reader has given the window. Only the key
    // table below keeps its own line breaks, because its columns are aligned.
    help_.add_topic("ckmux.terminal",
                    w::HelpTopic{"Terminal window",
                                 "Everything you type goes to the program running in this "
                                 "window. ckmux keeps only one key for itself: " +
                                     prefix_text + ".\n\nPress it, then one of:\n\n" + keys +
                                     "\nPress " + prefix_text + " twice to send a literal " +
                                     prefix_text + " to the program.",
                                 {{"ckmux.keys", "All keys"}, {"ckmux.prefix", "The prefix key"}}});
    help_.add_topic("ckmux.prefix", w::HelpTopic{"Prefix pending",
                                                 "ckmux is waiting for one key:\n\n" + keys +
                                                     "\nEsc cancels without doing anything.",
                                                 {{"ckmux.keys", "All keys"}}});
    help_.add_topic("ckmux.keys",
                    w::HelpTopic{"All keys",
                                 "Every ckmux command is also in the menu bar, and every "
                                 "menu entry shows the key that reaches it.\n\nPrefix: " +
                                     prefix_text + "\n\n" + keys +
                                     "\nInside a terminal, all other keys — function keys, Alt "
                                     "combinations, the mouse — belong to the program you are "
                                     "running, exactly as they would without ckmux.",
                                 {{"ckmux.terminal", "Terminal window"},
                                  {"ckmux.prefix", "The prefix key"}}});
    help_.add_topic(
        "ckmux.switcher",
        w::HelpTopic{
            "Window bar",
            "The row above the footer lists every open terminal by its caption. It is on "
            "screen while more than one terminal is open, and also whenever any terminal is "
            "put away — with one terminal hidden and none showing, this row is the only way "
            "back to it.\n\nEach entry carries the terminal's own window control as a mark, "
            "so a row and the window it stands for wear the same chrome: " +
                std::string(w::WindowSwitcherBar::status_glyph(
                    w::WindowSwitcherBar::Status::Visible)) +
                " for a terminal that is on the desktop, and " +
                std::string(w::WindowSwitcherBar::status_glyph(
                    w::WindowSwitcherBar::Status::Minimized)) +
                " — the mark on a window's minimize button — for one that has been put "
                "away.\n\nWhich one you are working in is shown the way the windows "
                "themselves show it: that entry's mark is lit in the control colour and its "
                "row is highlighted, exactly as the frame of the terminal you are in lights "
                "its own controls. The ones behind it draw the same mark plainly.\n\nClicking "
                "does what the mark says: the "
                "terminal you are in is put away, one behind comes forward and takes the "
                "keyboard, and one that was put away comes back in front. Right-click an entry "
                "for the rest — Minimize or Show, Maximize or Restore, Move / Resize, Rename…, "
                "Move to session…, and Close. Every one of them acts on the terminal whose "
                "entry you clicked, never on the one in front.\n\nThe ▼ at the far left hides "
                "the status bar and drops this row onto the last line of the screen, which is "
                "a row of terminal back. ▲ brings both back, and so does Window ▸ Status Bar.",
            {{"ckmux.terminal", "Terminal window"},
             {"ckmux.keys", "All keys"},
             {"ckmux.keys.all", "Every command"}}});

    // The complete listing behind Help ▸ All Keybindings… — and behind F1 on
    // any surface that has nothing more specific to say.
    std::string every = "Everything ckmux can do, and how it is reached. The prefix is " +
                        prefix_text + ".\n\nAfter the prefix:\n\n" + all_keys;
    if (!menu_only.empty())
        every += "\nIn the menus only — no key reaches these directly:\n\n" + menu_only;
    if (rebound.empty()) {
        every +=
            "\nEvery key above is ckmux's own default. Your configuration file can change any "
            "of them with `bind` and `unbind` lines, and what you change is listed here.";
    } else {
        // Marked, not silently applied: a reader whose key stopped working
        // needs to be told their own file is why, and told it here rather
        // than by reading the file again.
        every += "\nChanged by your configuration:\n\n" + rebound;
    }
    help_.add_topic("ckmux.keys.all",
                    w::HelpTopic{"Every command", std::move(every),
                                 {{"ckmux.keys", "All keys"},
                                  {"ckmux.terminal", "Terminal window"}}});

    // Copy mode. Its own keys are fixed rather than bindable (the interface spec), so
    // this page states them; the keymap has nothing to generate here.
    help_.add_topic(
        "ckmux.copy",
        w::HelpTopic{"Copy mode",
                     "You are reading this terminal's history rather than typing into it. The "
                     "window's title bar says COPY while you are.\n\n↑ ↓ PgUp PgDn Home End "
                     "move; Space or v starts a selection and moves extend it; Enter or y "
                     "copies what is selected and leaves; Esc or q leaves without copying.\n\n"
                     "What you copy goes to ckmux's own clipboard, which " +
                         prefix_text +
                         " ] pastes into any terminal here. The program you were "
                         "running is untouched and still there when you leave.",
                     {{"ckmux.terminal", "Terminal window"}, {"ckmux.keys", "All keys"}}});

    // The picker, which is also the first thing a new reader sees.
    help_.add_topic(
        "ckmux.picker",
        w::HelpTopic{"Sessions",
                     "A session is a set of terminals the server keeps running whether or not "
                     "anybody is watching them. This list is every session on this "
                     "server.\n\nEnter or [ Attach ] takes you to the highlighted one — even "
                     "one somebody else is watching, which is not refused: the newest client "
                     "wins and the other one is told so and handed this same list. [ New… ] "
                     "makes one, [ Rename… ] and [ End… ] act on the highlighted one, and "
                     "ending a session ends the programs in it.\n\nClosing this window "
                     "leaves ckmux running with nothing attached, which is a perfectly good "
                     "state: the commands that need a session simply go grey.",
                     {{"ckmux.keys", "All keys"}, {"ckmux.terminal", "Terminal window"}}});

    // Move/resize mode, which is modal and therefore worth saying out loud.
    help_.add_topic(
        "ckmux.move",
        w::HelpTopic{"Moving a window",
                     "The arrow keys move this window; Shift with them resizes it. Enter or "
                     "Esc finishes — Esc does not undo, because the window is already where "
                     "you put it.\n\nThe mouse does the same thing without a mode: drag the "
                     "title bar to move, drag an edge or a corner to resize.",
                     {{"ckmux.switcher", "Window bar"}, {"ckmux.keys", "All keys"}}});

    // And the surface WP-14 itself added, because a reader who has just been
    // told something they did not ask about may reasonably ask what it is.
    help_.add_topic(
        "ckmux.notice",
        w::HelpTopic{"Notices",
                     "A line over the desktop reporting something that happened without you "
                     "asking: a session taken over from another terminal, a session that "
                     "ended, a server that stopped.\n\nMost take themselves away after a few "
                     "seconds. The ones that matter — the ones that explain where your "
                     "terminals went — stay until you dismiss them, because the reader they "
                     "are for is the one who was not at the keyboard when it happened. Click "
                     "a line to dismiss it.\n\nNothing here ever takes the keyboard from the "
                     "program you are typing into. A notice is news, not a question; anything "
                     "ckmux needs an answer to is a dialog.",
                     {{"ckmux.picker", "Sessions"}, {"ckmux.terminal", "Terminal window"}}});
}

}  // namespace ckm::client
