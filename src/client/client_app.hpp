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
#include "cvision/widgets/desktop.hpp"
#include "cvision/widgets/help_viewer.hpp"
#include "cvision/widgets/status_line.hpp"
#include "cvision/widgets/window.hpp"

namespace ckm::client {

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
    void refresh_footer();
    ckv::widgets::Window* open_terminal(std::string title);
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
    void show_about();
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
