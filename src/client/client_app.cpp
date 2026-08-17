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
        pending_dialogs_.push_back(std::make_shared<w::HelpViewerPresentation>(
            w::present_help_viewer(help_, key, app_, *desktop_, roles_)));
    });
    footer_->set_hint_provider([](const std::string& key) -> std::string {
        if (key == "ckmux.prefix") return "choose a key, or Esc to cancel";
        if (key == "ckmux.terminal") return "keys go to the program in this window";
        return {};
    });
    new_terminal();
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
            declare(binding.key, binding.title, "Terminal", [this] { new_terminal(); });
        } else if (binding.key == commands::kCloseTerminal) {
            declare(binding.key, binding.title, "Terminal", [this] { close_terminal(active_terminal()); });
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
        } else if (binding.key == commands::kDetach || binding.key == commands::kSessions) {
            // Declared so the menu, the footer, and the key reference all show
            // the real shape of ckmux from the first milestone — and disabled,
            // so they say plainly that the server does not exist yet (M2)
            // rather than pretending or hiding.
            registry.set_enabled_predicate(declare(binding.key, binding.title, "Session", [] {}),
                                           [] { return false; });
        } else if (binding.key == commands::kFocusByNumber) {
            // Display-only: the digits carry a value, so they are dispatched
            // directly rather than through one handler.
            registry.set_enabled_predicate(declare(binding.key, binding.title, "Terminal", [] {}),
                                           [] { return false; });
        }
    }

    declare(commands::kSendPrefix, "Send Prefix to Program", "Terminal", [this] {
        if (w::TerminalView* const view = terminal_view_of(active_terminal())) {
            std::string bytes;
            const ckv::KeyChord& p = options_.settings.prefix;
            if (p.key == ckv::Key::Char && !p.text.empty() &&
                has_modifier(p.modifiers, ckv::Modifier::Ctrl)) {
                const char letter = p.text[0];
                const char upper = (letter >= 'a' && letter <= 'z')
                                       ? static_cast<char>(letter - 'a' + 'A')
                                       : letter;
                bytes.push_back(static_cast<char>(upper - 'A' + 1));
            }
            if (!bytes.empty()) view->session().send_input(bytes);
        }
    });

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

    w::MenuBarItem session_menu{
        "&Session",
        {item(commands::kNewTerminal), separator, item(commands::kDetach), item(commands::kSessions),
         separator, item(u::std_command_keys::kQuit)}};
    w::MenuBarItem terminal_menu{
        "&Terminal",
        {item(commands::kNewTerminal), item(commands::kCloseTerminal), separator,
         item(u::std_command_keys::kNextWindow), item(u::std_command_keys::kPreviousWindow), separator,
         item(u::std_command_keys::kWindowList)}};
    w::MenuBarItem window_menu{"&Window",
                               {item(commands::kMoveResize), item(u::std_command_keys::kZoom), separator,
                                item(u::std_command_keys::kTile), item(u::std_command_keys::kCascade)}};
    // plans/06-ui-design.md: settings dialogs collect under a top-level
    // Settings menu rather than hiding one per feature area — and the colour
    // theme is a setting, which is why there is no View menu holding three
    // themes of its own. It was one: three menu items that changed the theme
    // for the session and left the file saying something else, so a reader who
    // liked Light had to find it again every morning.
    w::MenuBarItem settings_menu{"&Settings", {item(commands::kSettings)}};
    w::MenuBarItem help_menu{"&Help",
                             {item(commands::kKeyReference), separator, item(commands::kAbout)}};

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

ckv::widgets::Window* ClientApp::new_terminal() {
    return open_terminal("Terminal " + std::to_string(next_terminal_number_++));
}

ckv::widgets::Window* ClientApp::open_terminal(std::string title) {
    const std::string fallback_title = title;
    auto window = std::make_unique<w::Window>(std::move(title));
    // Cascade from the top-left so a second terminal never lands exactly on
    // the first. Full placement policy is a session-model concern (M2).
    const ckv::Rect area = desktop_->content_area();
    const int offset = static_cast<int>((next_terminal_number_ - 2) % 6) * 2;
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
    window->set_content(std::move(view));

    w::Window* const terminal_window = window.get();
    ckv::term::TerminalSubsession* const terminal_session = &session;
    window->close_request = [this, terminal_window] {
        if (!child_is_live(terminal_window)) return true;
        // [general] confirm-kill = false means the reader has said they do
        // not want to be asked. Honoured here rather than by suppressing the
        // dialog later, so "no confirmation" really is no round trip.
        if (!options_.settings.confirm_kill) return true;
        if (confirmed_close_ == terminal_window) {
            confirmed_close_ = nullptr;
            return true;
        }
        confirm_then_close(terminal_window);
        return false;  // vetoed for now; the confirmation decides
    };
    window->on_closed = [this, terminal_window, terminal_session] {
        terminal_session->close();
        titles_.erase(terminal_window);
        w::schedule_self_detach(*terminal_window, app_);
        // Focus and the footer both follow whatever the desktop activates
        // next, once the detach has actually happened.
        app_.post([this] {
            focus_active_terminal();
            refresh_footer();
        });
    };
    titles_[terminal_window] = TerminalTitle{fallback_title, {}};
    desktop_->add_window(std::move(window));
    app_.set_focus(terminal_view);
    refresh_footer();
    return terminal_window;
}

void ClientApp::confirm_then_close(w::Window* window) {
    if (window == nullptr || desktop_ == nullptr) return;
    auto presentation = std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
        app_, *desktop_, roles_,
        w::MessageBoxDescriptor{w::MessageBoxKind::Confirm, "Close terminal",
                                "\"" + window->title() +
                                    "\" is still running a program.\nClose it anyway?",
                                w::MessageBoxButtons::YesNo}));
    presentation->set_completion_handler([this, window](w::MessageBoxResult result) {
        if (result != w::MessageBoxResult::Yes) return;
        confirmed_close_ = window;
        window->close();
    });
    pending_dialogs_.push_back(std::move(presentation));
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
        const std::string wanted = view->session().snapshot().title;
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
        if (chord == chord_spelling(ckv::KeyEvent{options_.settings.prefix})) {
            if (const std::optional<u::CommandId> send_prefix =
                    app_.commands().id_for(commands::kSendPrefix))
                app_.execute_command(*send_prefix);
        } else if (const int index = digit_chord(chord); index != 0) {
            desktop_->select_by_number(index);
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
    pending_dialogs_.push_back(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
        app_, *desktop_, roles_,
        w::MessageBoxDescriptor{w::MessageBoxKind::Info, "About ckmux",
                                std::string("ckmux ") + kVersion +
                                    "\nA terminal multiplexer with a visible interface.\n\n"
                                    "Built on ckVision.",
                                w::MessageBoxButtons::Ok})));
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
    if (copy_mode_window_ != nullptr) {
        const auto title = titles_.find(copy_mode_window_);
        if (title != titles_.end())
            copy_mode_window_->set_title(title->second.child.empty() ? title->second.fallback
                                                                     : title->second.child);
    }
    copy_mode_window_ = nullptr;
    focus_active_terminal();
    refresh_footer();
}

void ClientApp::refresh_copy_mode_caption() {
    // The badge and the position, on the window's own caption, because that is
    // where a reader looks to find out which window they are in and how far
    // back (plans/06). Restored on exit from the same store the title poll
    // reads, so a caption cannot be left saying COPY.
    if (copy_mode_ == nullptr || copy_mode_window_ == nullptr) return;
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
    for (const ClipboardTarget& target : options_.settings.clipboard) {
        switch (target.kind) {
            case ClipboardTarget::Kind::Osc52:
                // The outer terminal, through ckVision's clipboard writer —
                // which emits OSC 52 and therefore works over a connection.
                app_.set_clipboard_text(text);
                break;
            case ClipboardTarget::Kind::Pbcopy:
                if (options_.clipboard_writer) options_.clipboard_writer("pbcopy", text);
                break;
            case ClipboardTarget::Kind::Exec:
                if (options_.clipboard_writer) options_.clipboard_writer(target.command, text);
                break;
        }
    }
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
        w::FieldDescriptor{.label = "  Runs your login startup files (~/.zprofile, ~/.profile), so a",
                           .kind = w::FieldKind::Note});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  terminal here has the same PATH as one you open from the Dock.",
                           .kind = w::FieldKind::Note});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  Unticked, only ~/.zshrc and its kind run. tmux ticks this.",
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
    descriptor.fields.push_back(w::FieldDescriptor{
        .label = "  stops an absurd one: 64 is far more than a full screen at 4K and 2x",
        .kind = w::FieldKind::Note});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  scaling. Between " + std::to_string(ckm::kSixelMegapixelsMin) +
                                    " and " + std::to_string(ckm::kSixelMegapixelsMax) +
                                    "; a larger one is refused, with the reason.",
                           .kind = w::FieldKind::Note});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  Both take effect for terminals opened from now on.",
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
        [this, login_field, sixel_field, clock_field, theme_field](w::DialogResult result) {
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
            stored = apply_clock_mode(choice_value(kClockChoices, result.selected[clock_field],
                                                   options_.settings.clock)) &&
                     stored;
            stored = apply_theme(choice_value(kThemeChoices, result.selected[theme_field],
                                              options_.settings.theme)) &&
                     stored;
            if (!stored) report_settings_not_saved();
        });
    pending_dialogs_.push_back(std::move(presentation));
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
    pending_dialogs_.push_back(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
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
    std::string body = "ckmux started, but part of your configuration was not understood:\n";
    for (const std::string& warning : options_.config_warnings) body += "\n  " + warning;
    pending_dialogs_.push_back(std::make_shared<w::MessageBoxPresentation>(w::present_message_box(
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
