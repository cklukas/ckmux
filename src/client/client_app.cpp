// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "client/client_app.hpp"

#include <algorithm>
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
    if (options_.shell.empty()) options_.shell = ckm::resolve_shell();
    roles_ = u::intern_standard_roles(app_.roles());
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
    // Only ckmux's own ids get registered: ckVision's reserved standard
    // commands already carry fixed titles and default chords, and its Desktop
    // installs the window-management handlers. Referencing them is the
    // sanctioned path; re-registering them is rejected by design.
    const auto register_own = [&](u::CommandId id, std::string_view title, std::string category,
                                  std::function<void()> handler) {
        registry.add(u::CommandDescriptor{.id = id,
                                          .title = std::string(title),
                                          .category = std::move(category),
                                          .context = {},
                                          .chord = {},
                                          .handler = std::move(handler)});
    };

    for (const KeyBinding& binding : bindings()) {
        if (binding.command < commands::kBase) continue;  // a ckVision standard command
        switch (binding.command) {
            case commands::kNewTerminal:
                register_own(binding.command, binding.title, "Terminal", [this] { new_terminal(); });
                break;
            case commands::kCloseTerminal:
                register_own(binding.command, binding.title, "Terminal",
                             [this] { close_terminal(active_terminal()); });
                break;
            case commands::kMoveResize:
                register_own(binding.command, binding.title, "Window", [this] {
                    if (w::Window* const window = desktop_->active_window()) window->enter_move_mode();
                });
                break;
            case commands::kKeyReference:
                register_own(binding.command, binding.title, "Help", [this] { show_key_reference(); });
                break;
            case commands::kAbout:
                register_own(binding.command, binding.title, "Help", [this] { show_about(); });
                break;
            case commands::kSettings:
                register_own(binding.command, binding.title, "Settings", [this] { show_settings(); });
                break;
            case commands::kDetach:
            case commands::kSessions:
                // Registered so the menu, the footer, and the key reference
                // all show the real shape of ckmux from the first milestone —
                // and disabled, so they say plainly that the server does not
                // exist yet (M2) rather than pretending or hiding.
                register_own(binding.command, binding.title, "Session", [] {});
                registry.set_enabled_predicate(binding.command, [] { return false; });
                break;
            case commands::kFocusByNumber:
                // Display-only: the digits carry a value, so they are
                // dispatched directly rather than through one handler.
                register_own(binding.command, binding.title, "Terminal", [] {});
                registry.set_enabled_predicate(binding.command, [] { return false; });
                break;
            default:
                break;
        }
    }

    register_own(commands::kSendPrefix, "Send Prefix to Program", "Terminal", [this] {
        if (w::TerminalView* const view = terminal_view_of(active_terminal())) {
            std::string bytes;
            const ckv::KeyChord& p = options_.prefix;
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

    register_own(commands::kThemeDark, "&Dark", "View",
                 [this] { set_theme(u::make_dark_theme(app_.roles(), roles_)); });
    register_own(commands::kThemeLight, "&Light", "View",
                 [this] { set_theme(u::make_light_theme(app_.roles(), roles_)); });
    register_own(commands::kThemeMono, "&Monochrome", "View",
                 [this] { set_theme(u::make_mono_theme(app_.roles(), roles_)); });
}

void ClientApp::build_chrome() {
    // Every menu entry shows the key that reaches it from a terminal — the
    // prefix chord, not whatever single chord the registry holds. The two
    // differ for ckVision's standard commands, and the prefix one is the one
    // that works where a ckmux reader spends their time.
    const auto presentation = [this](const KeyBinding& binding) {
        return w::CommandPresentation{binding.command, std::string(binding.title),
                                      binding_label(options_.prefix, binding)};
    };
    const auto item = [&](u::CommandId command) {
        for (const KeyBinding& binding : bindings())
            if (binding.command == command) return w::MenuItem{presentation(binding)};
        return w::MenuItem{w::CommandPresentation{command}};
    };
    const w::MenuItem separator{"", u::kInvalidCommand, {}, true};

    w::MenuBarItem session_menu{
        "&Session",
        {item(commands::kNewTerminal), separator, item(commands::kDetach), item(commands::kSessions),
         separator, item(u::std_commands::kQuit)}};
    w::MenuBarItem terminal_menu{
        "&Terminal",
        {item(commands::kNewTerminal), item(commands::kCloseTerminal), separator,
         item(u::std_commands::kNextWindow), item(u::std_commands::kPreviousWindow), separator,
         item(u::std_commands::kWindowList)}};
    w::MenuBarItem window_menu{"&Window",
                               {item(commands::kMoveResize), item(u::std_commands::kZoom), separator,
                                item(u::std_commands::kTile), item(u::std_commands::kCascade)}};
    w::MenuBarItem view_menu{"&View",
                             {w::MenuItem{w::CommandPresentation{commands::kThemeDark}},
                              w::MenuItem{w::CommandPresentation{commands::kThemeLight}},
                              w::MenuItem{w::CommandPresentation{commands::kThemeMono}}}};
    // plans/06-ui-design.md: settings dialogs collect under a top-level
    // Settings menu rather than hiding one per feature area.
    w::MenuBarItem settings_menu{"&Settings", {item(commands::kSettings)}};
    w::MenuBarItem help_menu{"&Help",
                             {item(commands::kKeyReference), separator, item(commands::kAbout)}};

    w::ApplicationShell shell(app_, {.theme = u::make_dark_theme(app_.roles(), roles_),
                                     .menus = {std::move(session_menu), std::move(terminal_menu),
                                               std::move(window_menu), std::move(view_menu),
                                               std::move(settings_menu), std::move(help_menu)},
                                     .status_items = {},
                                     // ckmux composes every footer item from
                                     // the current context, so there is
                                     // nothing to hand over here — but the
                                     // bar is permanent chrome and must exist
                                     // before the first frame.
                                     .always_dock_status_line = true});
    desktop_ = &shell.desktop();
    footer_ = shell.status_line();
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

    const ckm::ShellLaunch shell = ckm::shell_launch(options_.shell, options_.login_shell);
    ckv::term::TerminalLaunchSpec launch =
        ckv::term::TerminalLaunchSpec::program(shell.executable, shell.arguments);
    launch.argv0 = shell.argv0;
    launch.profile = ckv::term::embedded_xterm_sixel_profile();
    // Let the child name its own window (OSC 0 and OSC 2). This is what makes
    // a caption say "vim: plans/02-architecture.md" instead of "Terminal 2".
    // The emulator neutralises control bytes in that text before it reaches
    // us, and its length is bounded there too.
    launch.profile.osc_policy = ckv::core::TerminalOscPolicy::StoreMetadata;
    launch.environment = options_.environment;
    // A shell opens where a shell opens. Left at the spec's default a
    // terminal starts in "/", which is nobody's working directory.
    if (!options_.working_directory.empty()) launch.working_directory = options_.working_directory;
    // The largest picture this terminal will hold, which the reader can raise
    // (Settings ▸ [terminal] sixel-max-megapixels). It bounds the picture, not
    // the window: a picture too large for the window is cut off at its edge.
    ckv::term::TerminalSubsessionOptions subsession;
    subsession.max_image_pixels =
        static_cast<std::size_t>(options_.sixel_max_megapixels) * 1024U * 1024U;
    ckv::term::TerminalSubsession& session =
        app_.launch_terminal_subsession(std::move(launch), subsession);

    auto view = std::make_unique<w::TerminalView>(session);
    w::TerminalView* const terminal_view = view.get();
    view->set_bounds(window->content_rect());
    view->set_help_context_key("ckmux.terminal");
    // The prefix rides on ckVision's parent-escape hook: the one key the
    // terminal view does not hand to the child.
    view->set_parent_escape(options_.prefix);
    view->on_parent_escape = [this] { arm_prefix(); };
    view->on_selection_copy = [this](std::string text) { app_.set_clipboard_text(std::move(text)); };
    window->set_content(std::move(view));

    w::Window* const terminal_window = window.get();
    ckv::term::TerminalSubsession* const terminal_session = &session;
    window->close_request = [this, terminal_window] {
        if (!child_is_live(terminal_window)) return true;
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
    const Context current = context();
    std::vector<w::StatusLineItem> items;
    for (const KeyBinding* binding : footer_bindings(current)) {
        // In the prefix context the reader has already pressed the prefix, so
        // the bare key is what they need to see next; otherwise the full
        // "^B c" is the reachable spelling. Either way the key is stated
        // rather than derived: several of these commands are ckVision's own,
        // whose registered chords (F6, F10) are consumed by the child
        // program while a terminal has focus and so would advertise a key
        // that never arrives.
        const std::string key = current == Context::Prefix
                                    ? std::string(binding->chord)
                                    : binding_label(options_.prefix, *binding);
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
    auto overlay = std::make_unique<PrefixOverlay>(options_.prefix, roles_);
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
        if (chord == chord_spelling(ckv::KeyEvent{options_.prefix})) {
            app_.execute_command(commands::kSendPrefix);
        } else if (const int index = digit_chord(chord); index != 0) {
            desktop_->select_by_number(index);
            focus_active_terminal();
        } else if (const KeyBinding* const binding = find_binding(chord)) {
            app_.execute_command(binding->command);
        }
    }
    refresh_footer();
}

void ClientApp::set_theme(u::Theme theme) {
    app_.theme() = std::move(theme);
    app_.invalidate_all();
}

void ClientApp::show_key_reference() { app_.execute_command(u::std_commands::kHelp); }

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
                           .initial_checked = options_.login_shell});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  Runs your login startup files (~/.zprofile, ~/.profile), so a",
                           .kind = w::FieldKind::Note});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  terminal here has the same PATH as one you open from the Dock.",
                           .kind = w::FieldKind::Note});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  Unticked, only ~/.zshrc and its kind run. tmux ticks this.",
                           .kind = w::FieldKind::Note});
    descriptor.fields.push_back(w::FieldDescriptor{.kind = w::FieldKind::Note});

    // The picture limit. Its unit is in the label because a bare number in a
    // field is a question ("of what?"), and the note underneath answers the
    // one a reader actually has: why a limit exists at all, and what happens
    // when a terminal is over it.
    const std::size_t sixel_field = descriptor.fields.size();
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "&Largest picture a program may draw (megapixels):",
                           .initial_text = std::to_string(options_.sixel_max_megapixels),
                           .validate = [](const std::string& text) {
                               return ckm::parse_sixel_megapixels(text).has_value();
                           },
                           .kind = w::FieldKind::Text});
    descriptor.fields.push_back(w::FieldDescriptor{
        .label = "  Pictures larger than the window are cut off at its edge, so this",
        .kind = w::FieldKind::Note});
    descriptor.fields.push_back(w::FieldDescriptor{
        .label = "  only stops an absurd one: 64 is far more than a full screen at",
        .kind = w::FieldKind::Note});
    descriptor.fields.push_back(
        w::FieldDescriptor{.label = "  4K and 2x scaling, which is about 33. Between " +
                                    std::to_string(ckm::kSixelMegapixelsMin) + " and " +
                                    std::to_string(ckm::kSixelMegapixelsMax) + ". A picture past",
                           .kind = w::FieldKind::Note});
    descriptor.fields.push_back(w::FieldDescriptor{
        .label = "  it is refused, and the terminal says which picture and which limit.",
        .kind = w::FieldKind::Note});

    // A blank line: what the settings do and when they start applying are two
    // different thoughts, and the second is the one a reader comes back for.
    descriptor.fields.push_back(w::FieldDescriptor{.kind = w::FieldKind::Note});
    descriptor.fields.push_back(w::FieldDescriptor{
        .label = "  Both take effect for terminals opened from now on.", .kind = w::FieldKind::Note});
    descriptor.buttons.push_back(w::ButtonDescriptor{"&Save", true, nullptr});
    descriptor.buttons.push_back(w::ButtonDescriptor{"&Cancel", false, nullptr});
    descriptor.minimum_window_size = ckv::Size{72, 21};
    descriptor.button_alignment = u::Alignment::End;
    descriptor.anchor_buttons_to_bottom = true;

    auto presentation = std::make_shared<w::DescriptorDialogPresentation>(
        w::present_dialog(std::move(descriptor), app_, *desktop_, roles_));
    presentation->set_completion_handler([this, login_field, sixel_field](w::DialogResult result) {
        if (!result.accepted || result.checked.size() <= login_field ||
            result.values.size() <= sixel_field)
            return;
        // One report between them: a home directory that cannot be written to
        // fails both saves, and being told so twice is being told so once with
        // an extra dialog in the way.
        bool stored = apply_login_shell(result.checked[login_field]);
        // The field validator has already refused anything unreadable, so a
        // value that arrives here parses; the check is what makes that a fact
        // rather than an assumption.
        if (const std::optional<int> megapixels = ckm::parse_sixel_megapixels(result.values[sixel_field]))
            stored = apply_sixel_max_megapixels(*megapixels) && stored;
        if (!stored) report_settings_not_saved();
    });
    pending_dialogs_.push_back(std::move(presentation));
}

bool ClientApp::apply_login_shell(bool login_shell) {
    if (options_.login_shell == login_shell) return true;
    options_.login_shell = login_shell;
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
    if (options_.sixel_max_megapixels == megapixels) return true;
    options_.sixel_max_megapixels = megapixels;
    return ckm::save_setting(ckm::platform::config_file_path(), "terminal", "sixel-max-megapixels",
                             std::to_string(megapixels));
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
    for (const KeyBinding& binding : bindings()) {
        if (binding.chord.empty()) continue;
        std::string label = binding_label(options_.prefix, binding);
        label.resize(std::max<std::size_t>(label.size(), 8), ' ');
        keys += label + "  " + std::string(binding.hint) + "\n";
    }
    const std::string prefix_text = prefix_label(options_.prefix);

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
