// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "client/client_app.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <utility>

#include "common/config.hpp"
#include "common/shell.hpp"
#include "platform/paths.hpp"

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

// The version string children see, and the About box shows. ckmux is
// pre-release; the number is deliberately the plan's own milestone number
// rather than a marketing version.
constexpr const char* kVersion = "0.0.1 (M1)";

w::TerminalView* terminal_view_of(w::Window* window) {
    if (window == nullptr) return nullptr;
    return dynamic_cast<w::TerminalView*>(window->content());
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
    // plans/04-terminal-emulation.md: xterm-256color first, with honest
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
}

ClientApp::~ClientApp() {
    if (which_key_timer_ != 0) app_.cancel_timer(which_key_timer_);
    if (title_timer_ != 0) app_.cancel_timer(title_timer_);
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

void ClientApp::build_chrome() {
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

    // No New Terminal here: a terminal is the Terminal menu's to make, and the
    // same item under two titles is a reader wondering which one they used.
    // This menu holds what happens to SESSIONS.
    w::MenuBarItem session_menu{
        "&Session",
        {item(commands::kNewSession), item(commands::kSessions),
         item(commands::kRenameSession), item(commands::kKillSession), separator,
         item(commands::kDetach), item(u::std_command_keys::kQuit)}};
    // Rule 2 of plans/06: everything is in the menu, with the chord that
    // reaches it beside it. Copy Mode, Paste and Send Prefix live here — the
    // three verbs a reader aims at the terminal itself rather than at the
    // desktop around it.
    w::MenuBarItem terminal_menu{
        "&Terminal",
        {item(commands::kNewTerminal), item(commands::kCloseTerminal),
         item(commands::kMoveTerminal), separator,
         item(u::std_command_keys::kNextWindow), item(u::std_command_keys::kPreviousWindow),
         separator, item(commands::kCopyMode), item(commands::kPaste), separator,
         item(commands::kSendPrefix)}};
    // The window list is a list of WINDOWS — it lives with the other commands
    // that arrange them, not under Terminal, where it sat because M1 had no
    // Window menu yet to put it in.
    w::MenuBarItem window_menu{"&Window",
                               {item(u::std_command_keys::kWindowList), separator,
                                item(commands::kMoveResize), item(u::std_command_keys::kZoom), separator,
                                item(u::std_command_keys::kTile), item(u::std_command_keys::kCascade)}};
    // plans/06-ui-design.md: settings dialogs collect under a top-level
    // Settings menu rather than hiding one per feature area — and the colour
    // theme is a setting, which is why there is no View menu holding three
    // themes of its own. It was one: three menu items that changed the theme
    // for the session and left the file saying something else, so a reader who
    // liked Light had to find it again every morning.
    w::MenuBarItem settings_menu{"&Settings", {item(commands::kSettings)}};
    // The terminal report sits where cksetup keeps it: in Help, beside the
    // other "what is this thing doing" answers. It reports the OUTER host —
    // what ckmux itself detected, Sixel included — which is the report a
    // reader wants when a picture drawn inside a ckmux terminal fails to
    // appear.
    w::MenuBarItem help_menu{"&Help",
                             {item(commands::kKeyReference),
                              item(u::std_command_keys::kTerminalReport), separator,
                              item(commands::kAbout)}};

    // The theme the constructor already put on the application, handed
    // straight back: the shell assigns whatever it is given, so naming one
    // here would quietly overrule `[general] theme` — which is how a reader
    // who chose Light got Dark every morning.
    w::ApplicationShell shell(app_, {.theme = app_.theme(),
                                     .menus = {std::move(session_menu), std::move(terminal_menu),
                                               std::move(window_menu), std::move(settings_menu),
                                               std::move(help_menu)},
                                     .status_items = {},
                                     // ckmux composes every footer item from
                                     // the current context, so there is
                                     // nothing to hand over here — but the
                                     // bar is permanent chrome and must exist
                                     // before the first frame.
                                     .always_dock_status_line = true});
    desktop_ = &shell.desktop();
    footer_ = shell.status_line();
    install_clock();
}

ckv::widgets::MenuBar* ClientApp::menu_bar() {
    if (desktop_ == nullptr) return nullptr;
    return dynamic_cast<w::MenuBar*>(desktop_->top_dock());
}

void ClientApp::install_clock() {
    // The clock goes at the right end of the menu bar (plans/06): it is where
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
    for (const SessionRow& row : rows) {
        std::string line = row.name.empty() ? "session " + std::to_string(row.id) : row.name;
        line += row.terminals == 1 ? " — 1 terminal"
                                   : " — " + std::to_string(row.terminals) + " terminals";
        // Three states, and a reader picking a row needs to tell them apart:
        // the one they are already watching, one somebody else is watching —
        // where attaching takes it from them — and one nobody holds.
        if (row.id == attached_session_ && attached_session_ != 0)
            line += "  (this client)";
        else if (row.attached)
            line += "  (in use — attaching takes it over)";
        choice.options.push_back(std::move(line));
    }
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

    // Attaching to a different session is a detach and an attach, which is what
    // "switch" means here (plans/03): the windows this client is showing belong
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
        if (options_.attach_to_session) options_.attach_to_session(attach_choice_[static_cast<std::size_t>(index)]);
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
    // The numbering starts over with the session: "Terminal 1" is the first
    // terminal of what a reader is looking at now.
    next_terminal_number_ = 1;
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
    titles_.erase(window);
    (void)desktop_->remove_window(window);
    forgetting_terminals_ = false;
    focus_active_terminal();
    refresh_footer();
}

void ClientApp::remember_sessions(std::vector<SessionRow> rows) {
    last_sessions_ = std::move(rows);
    if (attached_session_ != 0) attached_session_name_ = session_name(attached_session_);
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
    // opened when the server says the terminal exists (plans/03). Locally it is
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
    const ckv::Rect area = desktop_->content_area();
    const int offset = static_cast<int>((number - 1) % 6) * 2;
    window->set_bounds(ckv::Rect{area.x + 2 + offset, area.y + 1 + offset,
                                 std::max(20, area.width - 8 - offset),
                                 std::max(6, area.height - 4 - offset)});

    const ckm::ShellLaunch shell = ckm::shell_launch(options_.settings.shell, options_.settings.login_shell);
    ckv::term::TerminalLaunchSpec launch =
        ckv::term::TerminalLaunchSpec::program(shell.executable, shell.arguments);
    launch.argv0 = shell.argv0;
    launch.profile = ckv::term::embedded_xterm_sixel_profile();
    // Let the child name its own window (OSC 0 and OSC 2). This is what makes
    // a caption say "vim: plans/02-architecture.md" instead of "Terminal 2".
    // The emulator neutralises control bytes in that text before it reaches
    // us, and its length is bounded there too.
    launch.profile.osc_policy = ckv::core::TerminalOscPolicy::StoreMetadata;
    // What the reader's configuration says this terminal can do (plans/08).
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
    // to be worth claiming (plans/12 WP-5).
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
        titles_.erase(terminal_window);
        w::schedule_self_detach(*terminal_window, app_);
        // Focus and the footer both follow whatever the desktop activates
        // next, once the detach has actually happened.
        app_.post([this] {
            focus_active_terminal();
            refresh_footer();
        });
    };
    titles_[terminal_window] = TerminalTitle{fallback_title, {}, number};
    desktop_->add_window(std::move(window));
    app_.set_focus(terminal_view);
    refresh_footer();
    return terminal_window;
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
            // goes with its last terminal (kill-empty-session, plans/03).
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

void ClientApp::refresh_terminal_titles() {
    if (desktop_ == nullptr) return;
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
        // While its history is being read the caption belongs to copy mode,
        // which writes the badge and the scroll position into it. The child's
        // title is still recorded, so leaving copy mode restores whatever the
        // program asked for in the meantime rather than a stale name.
        if (window == copy_mode_window_) {
            entry->second.child = wanted;
            refresh_copy_mode_caption();
            continue;
        }
        if (wanted == entry->second.child) continue;  // nothing changed
        entry->second.child = wanted;
        // An empty title is how a program hands the caption back — on exit,
        // or when it simply stops claiming one — so the window returns to the
        // name ckmux gave it rather than keeping stale text forever.
        window->set_title(wanted.empty() ? entry->second.fallback : wanted);
    }
}

void ClientApp::focus_active_terminal() {
    if (w::TerminalView* const view = terminal_view_of(active_terminal())) app_.set_focus(view);
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
    // plans/06 promises "1-9 focus window" and a digit that does nothing at all
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
    // from here (plans/06's context table).
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
    const Context current = context();
    std::vector<w::StatusLineItem> items;
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
    overlay_->set_bounds(overlay_->preferred_bounds(desktop_->content_area()));
    app_.set_focus(overlay_);
    which_key_timer_ = app_.start_timer(options_.which_key_delay_nanos, /*repeating=*/false, [this] {
        which_key_timer_ = 0;
        if (overlay_ == nullptr) return;
        overlay_->expand();
        overlay_->set_bounds(overlay_->preferred_bounds(desktop_->content_area()));
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

void ClientApp::show_about() {
    if (desktop_ == nullptr) return;
    retain_dialog(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
        app_, *desktop_, roles_,
        w::MessageBoxDescriptor{w::MessageBoxKind::Info, "About ckmux",
                                std::string("ckmux ") + kVersion +
                                    "\nA terminal multiplexer with a visible interface.\n\n"
                                    "Built on ckVision.",
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
        if (title != titles_.end())
            copy_mode_window_->set_title(title->second.child.empty() ? title->second.fallback
                                                                     : title->second.child);
    }
    copy_mode_window_ = nullptr;
    copy_mode_window_alive_.reset();
    focus_active_terminal();
    refresh_footer();
}

void ClientApp::refresh_copy_mode_caption() {
    // The badge and the position, on the window's own caption, because that is
    // where a reader looks to find out which window they are in and how far
    // back (plans/06). Restored on exit from the same store the title poll
    // reads, so a caption cannot be left saying COPY.
    if (copy_mode_ == nullptr || copy_mode_window_ == nullptr) return;
    // Nothing is written through a window that has been destroyed. This runs on
    // the title timer, so "the window went while copy mode was up" is not a
    // hypothetical race — it is a tenth of a second wide, every tenth of a
    // second, for as long as a reader stays in copy mode.
    if (copy_mode_window_alive_.expired()) return;
    const auto title = titles_.find(copy_mode_window_);
    const std::string base = title == titles_.end() ? copy_mode_window_->title()
                             : title->second.child.empty() ? title->second.fallback
                                                            : title->second.child;
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
    view->session().send_input(
        paste_bytes(internal_clipboard_, view->session().snapshot().bracketed_paste_enabled));
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

    // The picture limit. Its unit is in the label because a bare number in a
    // field is a question ("of what?"), and the note underneath answers the
    // one a reader actually has: why a limit exists at all, and what happens
    // when a terminal is over it.
    const std::size_t sixel_field = descriptor.fields.size();
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "&Largest picture a program may draw (megapixels):",
                           .initial_text = std::to_string(options_.settings.sixel_max_megapixels),
                           .validate = [](const std::string& text) {
                               return ckm::parse_sixel_megapixels(text).has_value();
                           },
                           .kind = w::FieldKind::Text});
    descriptor.fields.push_back(w::FieldDescriptor{
        .label = "  Pictures larger than the window are cut off at its edge, so this only",
        .kind = w::FieldKind::Note});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  stops an absurd one. Between " + std::to_string(ckm::kSixelMegapixelsMin) +
                                    " and " + std::to_string(ckm::kSixelMegapixelsMax) +
                                    "; new terminals only.",
                           .kind = w::FieldKind::Note});

    // How long a session's programs are given to end. The note says the thing a
    // reader would otherwise find out the hard way: that nothing is killed at
    // all unless they ask for it at the moment they end a session.
    const std::size_t grace_field = descriptor.fields.size();
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "&Give programs this long to quit (seconds):",
                           .initial_text = std::to_string(options_.settings.kill_grace_seconds),
                           .validate = [](const std::string& text) {
                               return ckm::parse_int_in_range(text, 0, 600).has_value();
                           },
                           .kind = w::FieldKind::Text});
    descriptor.fields.push_back(w::FieldDescriptor{
        .label = "  When you end a session. Nothing is killed unless you tick that box.",
        .kind = w::FieldKind::Note});

    // The clock and the theme: chrome rather than terminal behaviour, which is
    // why they are last, and why the note above says only the two settings
    // over it wait for a new terminal. These two change as the dialog closes.
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
        [this, login_field, sixel_field, grace_field, clock_field, theme_field](
            w::DialogResult result) {
            if (!result.accepted || result.checked.size() <= login_field ||
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

bool ClientApp::apply_login_shell(bool login_shell) {
    if (options_.settings.login_shell == login_shell) return true;
    options_.settings.login_shell = login_shell;
    // Stored where the reader can also edit it by hand, and where it will be
    // found again next time (plans/08-configuration.md). A save that cannot
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
    // this page as a matter of course (plans/06-ui-design.md).
    std::string keys;
    for (const KeyBinding& binding : keymap_.bindings()) {
        if (binding.chord.empty()) continue;
        std::string label = binding_label(options_.settings.prefix, binding);
        label.resize(std::max<std::size_t>(label.size(), 8), ' ');
        keys += label + "  " + std::string(binding.hint) + "\n";
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
}

}  // namespace ckm::client
