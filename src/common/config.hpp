// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The ckmux configuration file (the configuration spec).
//
// Two rules from that plan shape everything here. ckmux must be excellent
// with an EMPTY config, so every key has a built-in default and a missing
// file is not an error — it is the normal case. And a bad key never aborts:
// it warns, naming the line, and the rest of the file is still read, because
// a typo in one setting is no reason to refuse to start.
//
// Config v1 is the whole key table of that plan. A key is *read and
// validated* here whether or not the milestone that acts on it has arrived:
// `scrollback` reaches a terminal today, `max-fps` waits for a server that
// does not exist yet. That is deliberate. The alternative — warn "unknown
// key" at a key the plan documents — teaches a reader that the file they were
// handed is wrong, and the one after it (accept silently, do nothing) is the
// live-looking-and-dead failure this project pins tests against. `Settings`
// carries every value; `keys_not_honoured_yet()` names the ones that do not
// reach anything yet, and `check-config` prints it (the work queue WP-11).
//
// The format has exactly one escape: `\#` is a literal '#' rather than the
// start of a comment, so that a `[terminal] clipboard` command may contain
// one. Every other backslash stands for itself — the values that need the
// escape are shell commands, and a format that rewrote `\\` or `\n` would
// break more of them than it saved.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/keymap.hpp"
#include "cvision/core/key.hpp"

namespace ckm {

// What [terminal] sixel-max-megapixels will accept. One megapixel is smaller
// than any terminal worth drawing into and is the floor rather than zero,
// because zero is not "unlimited" — it is "no pictures", which the reader
// already has a clearer way to ask for. The ceiling is absurdity insurance:
// 4096 megapixels is a hundred times the largest display anyone can buy.
inline constexpr int kSixelMegapixelsMin = 1;
inline constexpr int kSixelMegapixelsMax = 4096;

// The one reading of that value, shared by the config parser and the settings
// dialog's validator — two readings would eventually disagree, and the reader
// would meet a number the dialog accepted and the file then rejected.
// Digits only: a unit suffix would invite `64M` to mean megabytes.
std::optional<int> parse_sixel_megapixels(std::string_view text);

// The bounds the file's numbers are held to. Each one is a range a reader
// could plausibly want all of, with an end that is absurdity insurance rather
// than a judgement about taste.
inline constexpr int kScrollbackMin = 0;          // 0 = remember nothing
inline constexpr int kScrollbackMax = 1'000'000;  // ~2 GB of cells at 80 columns
// How many terminals one session may hold (the session model: "config, default 64").
// The floor is 1 rather than 0: a session that can hold no terminals is a
// session nothing can be done with, and a reader who wants that closes the
// session. The ceiling is absurdity insurance — every terminal is a PTY, a
// child process and a scrollback, so a machine runs out of descriptors long
// before this.
inline constexpr int kMaxTerminalsMin = 1;
inline constexpr int kMaxTerminalsMax = 4'096;
inline constexpr int kMaxFpsMin = 1;
inline constexpr int kMaxFpsMax = 240;
// Byte sizes accept a K/M/G suffix, since that is how the plan's own example
// file writes them (`256K`, `1M`). The ceiling is one gigabyte: a spool is
// held in memory and shown to a person.
inline constexpr std::size_t kByteSizeMax = 1024ULL * 1024ULL * 1024ULL;

enum class Theme : unsigned char { Dark, Light, Mono };
// What the menu bar's clock shows. Three states rather than a bool, because
// "with seconds" and "without" are as different to a reader as either is to
// no clock: a second hand is either information or motion in the corner of
// their eye, and which one it is depends on the person, not on the setting
// being on.
enum class ClockMode : unsigned char { Seconds, Minutes, Off };
enum class ExitPolicy : unsigned char { Close, Hold, HoldOnError };
enum class SixelMode : unsigned char { Auto, Off };
enum class PrinterMode : unsigned char { Ask, Capture, Off };
enum class PrinterSaveFormat : unsigned char { Text, Ansi };
// [general] desktop-size — what happens to a SESSION's virtual desktop when a
// client of another size attaches to it (WP-40, the session model "Two clients at
// once").
//
// `Fixed` is the default and the honest one: the desktop is set by the first
// client to attach and changed only when a reader asks. `FitSmallest` shrinks
// to a newcomer and — deliberately — never grows back when they leave, because
// a session whose geometry oscillates with people's attach cycles is worse
// than one that is merely too small. `FitLatest` adopts whoever attached last,
// which is what tmux does by default and is offered for readers who want it.
enum class DesktopSizePolicy : unsigned char { Fixed, FitSmallest, FitLatest };

// One entry of [terminal] clipboard, in the order the file listed them: the
// copy targets are tried in turn, so the order is the setting.
struct ClipboardTarget {
    enum class Kind : unsigned char { Osc52, Pbcopy, Exec } kind = Kind::Osc52;
    // The command for `exec:<command>`, empty otherwise.
    std::string command;

    friend bool operator==(const ClipboardTarget&, const ClipboardTarget&) = default;
};

// The one reading of each value's grammar, shared by the config parser, the
// settings dialogs and `check-config`. Two readings would eventually
// disagree, and a reader would meet a value one accepted and the other
// rejected.
std::optional<bool> parse_bool(std::string_view text);
std::optional<int> parse_int_in_range(std::string_view text, int minimum, int maximum);
std::optional<std::size_t> parse_byte_size(std::string_view text);
std::optional<std::vector<ClipboardTarget>> parse_clipboard_targets(std::string_view text);

// Everything ckmux reads from the file, with the built-in defaults that
// apply when it says nothing. A struct rather than a lookup, so a caller
// cannot ask for a key that has no default. Defaults are
// The configuration spec's annotated example, key for key: that file
// claims to show the defaults, so it is the specification of this struct.
struct Settings {
    // --- [general] ------------------------------------------------------
    // The one key ckmux steals from the programs running inside it. C-b, as
    // tmux's is, because the readers this is for already have it in their
    // fingers.
    ckv::KeyChord prefix{ckv::Key::Char, ckv::Modifier::Ctrl, "b"};
    // What New Terminal runs. Empty means $SHELL, resolved at launch — the
    // file says `$SHELL` and means "whatever the reader's shell is", not the
    // literal five characters.
    std::string shell;
    // [general] login-shell. True — a login shell — is the default because
    // it is what tmux does, what login(1) does, and what every terminal
    // window on the machine does; a reader's PATH usually depends on it.
    bool login_shell = true;
    // Lines of history per terminal. 0 is "remember nothing", which is a real
    // request and not merely the smallest number (ckVision's
    // `max_scrollback_lines`, U0-c).
    int scrollback = 10'000;
    // The most terminals one session may hold (the session model's new-terminal row:
    // "Session at terminal limit (config, default 64) → error"). A limit
    // rather than none because a session is a thing a reader manages by hand,
    // and a runaway script asking for terminals in a loop would otherwise take
    // the machine's descriptors with it.
    int max_terminals = 64;
    // Whether a child's bell reaches the reader's own terminal as a bell
    // (the interface spec: "audible bell to the host optional via config"). The
    // VISUAL half — the window's border flash and the footer flag — is not
    // configurable and is always on: it is how a reader learns that a window
    // they are not looking at wants them, which is the whole point of a bell
    // in a multiplexer. What a reader may reasonably not want is the noise,
    // especially with several terminals running, so only that is a setting.
    bool audible_bell = false;
    Theme theme = Theme::Dark;
    // What happens to a window when its program exits. hold-on-error keeps
    // the window when the exit status is non-zero, which is the one case
    // where the text on screen is the only evidence of what went wrong.
    ExitPolicy on_exit = ExitPolicy::HoldOnError;
    // Whether closing or killing something with a live program in it asks
    // first. A terminal holding an editor with unsaved work looks exactly
    // like one holding an idle shell.
    bool confirm_kill = true;
    // [general] kill-grace-seconds. How long every program in a session is
    // given to end on its own when a reader kills it, before the escalation
    // they ticked (or did not) applies. Seconds, because that is the unit a
    // reader thinks in for "wait a moment"; zero means do not wait at all.
    int kill_grace_seconds = 5;
    bool kill_empty_session = true;
    // The clock at the right end of the menu bar, and the calendar that drops
    // out of it when clicked. Seconds by default: the bar's right end is
    // otherwise empty, and a clock a reader glances at while a build runs is
    // more use with a second hand than without. `minutes` and `off` are the
    // other two answers (Settings ▸ General…).
    ClockMode clock = ClockMode::Seconds;
    // [general] desktop-size. See DesktopSizePolicy: a client's own screen
    // never silently reflows a session's windows, because reflowing them
    // SIGWINCHes every child in it — for every reader watching, not just the
    // one who attached.
    DesktopSizePolicy desktop_size = DesktopSizePolicy::Fixed;
    // [general] resize-windows-to-fit. What happens to a window that is still
    // too large for the desktop AFTER a reattach has moved it as far up and
    // left as it can (WP-30).
    //
    // Off, and deliberately: a reader's window stays the size they set it to
    // unless they asked otherwise. Reattaching on a smaller terminal then
    // leaves such a window at its real size with a corner hanging off the
    // edge, which is honest — the window IS that big — and reattaching on the
    // larger one again puts it back exactly as it was. On, the window is
    // shrunk to the largest size that fits, which is a change to the
    // arrangement the reader made and never something to do without being
    // asked. The MOVE is not optional and happens either way; this decides only
    // whether a second, resizing step follows it.
    bool resize_windows_to_fit = false;
    // [general] show-cpu / show-memory-rss / show-memory-real. The View
    // menu's three readouts (WP-39): what the processes under each terminal
    // cost, on every terminal window's frame footer, sampled about once a
    // second. All off by default — a readout is chrome a reader opts into.
    // "Real" is the platform's own what-does-it-actually-cost metric (macOS
    // phys_footprint, PSS once WP-22 fills Linux in), beside RSS because
    // summing RSS over a process tree bills every shared page once per
    // process that maps it. The sample interval is deliberately not a key
    // (the configuration spec, "explicitly not configurable in v1").
    bool show_cpu = false;
    bool show_memory_rss = false;
    bool show_memory_real = false;

    // --- [terminal] -----------------------------------------------------
    // $TERM inside terminals. `auto` lets ckmux choose what it can honestly
    // claim (the terminal-emulation spec); anything else is used verbatim, because a reader who
    // names a terminfo entry has a reason.
    std::string term = "auto";
    bool mouse = true;
    bool alternate_scroll = true;
    SixelMode sixel = SixelMode::Auto;

    // [terminal] sixel-max-megapixels. The largest picture a program running
    // in a terminal may draw. A picture too large for the window it is drawn
    // in is cut off at the window's edge, so this is not a bound anyone meets
    // by having a big screen: it keeps an absurd geometry away from the
    // decoder's allocator, and sits far above a full screen (4K at 2x scaling
    // is about 33 megapixels). The default matches ckVision's own
    // `max_image_pixels`, so an unconfigured ckmux and an unconfigured
    // ckVision behave alike.
    int sixel_max_megapixels = 64;

    // Whether a program inside a terminal may put text on the system
    // clipboard with OSC 52. On, and capped upstream at 64 KiB: it is how
    // copying out of a program running under ckmux works at all.
    bool osc52 = true;
    // Where a copy goes, in order. The default is the plan's: the outer
    // terminal first, a local helper after it.
    std::vector<ClipboardTarget> clipboard{ClipboardTarget{ClipboardTarget::Kind::Osc52, {}},
                                           ClipboardTarget{ClipboardTarget::Kind::Pbcopy, {}}};

    // --- [printer] (WP-PRINT) -------------------------------------------
    PrinterMode printer_mode = PrinterMode::Ask;
    std::size_t printer_ask_cache_bytes = 256U * 1024U;
    std::size_t printer_spool_limit_bytes = 1024U * 1024U;
    PrinterSaveFormat printer_save_format = PrinterSaveFormat::Text;
    std::string printer_save_folder = "~/Documents";
    bool printer_save_ask_name = true;

    // --- [render] -------------------------------------------------------
    int max_fps = 30;

    // --- [keys] ---------------------------------------------------------
    // The `bind` and `unbind` lines, in file order, because order decides:
    // two binds of one chord mean the later one, and an unbind after a bind
    // means neither.
    std::vector<BindDirective> binds;

    friend bool operator==(const Settings&, const Settings&) = default;
};

// Which keys do NOT yet reach anything, as key names with the package each
// one waits for ("[general] on-exit (WP-13)"). The honest answer to "I set
// this — why did nothing happen?", and what `check-config` prints.
//
// A key leaves this list by being wired up, in the commit that wires it up.
// The list is as capable of lying in one direction as the other, and has: it
// named four keys that had since been honoured, so a reader was told nothing
// would happen and then something did (m-conf). Verify against the code, not
// against this list, before adding to it.
std::vector<std::string> keys_not_honoured_yet();

struct LoadedSettings {
    Settings settings;
    // One entry per problem, each naming its file and line in full:
    // "/home/me/.config/ckmux/ckmux.conf:4: unknown key 'shel' in [general]".
    // The whole path rather than the basename, because a reader with a
    // `$CKMUX_CONFIG` beside their real file cannot tell two "ckmux.conf:4"s
    // apart, and cannot paste one into an editor. The caller decides where
    // these go; nothing here writes to a stream of its own.
    std::vector<std::string> warnings;
};

// Reads `path`. A file that does not exist yields the defaults and no
// warnings — that is what an unconfigured ckmux is. A file that exists and
// cannot be read is the opposite case and warns, naming the path and the
// reason: it is a file the reader wrote, and behaving as though they had
// configured nothing would be ckmux ignoring them in silence.
LoadedSettings load_settings(const std::filesystem::path& path);

// Writes one key, leaving the rest of the file exactly as it was: comments,
// blank lines, key order and every unrelated key survive, because the file
// is the reader's and a dialog is only a visitor in it (the configuration spec). A
// matching key is rewritten where it stands, keeping any trailing comment; a
// missing key is added to its section; a missing section is appended. The
// file and its directory are created if absent.
//
// A key its section holds more than once is rewritten at *every* occurrence.
// The file is read back last-line-wins, so rewriting only the first would
// leave the dialog's Save with no effect the reader can see. Nothing is
// deleted: their duplicate line is theirs, and load_settings says out loud
// that the key is set twice.
//
// The write is atomic and durable: a scratch file (named per process, so two
// clients saving at once cannot share one), flushed to disk, renamed over the
// target, and the directory entry flushed after it. "Saved" therefore survives
// a power loss, which is the only kind of promise a settings dialog can make.
//
// Returns false if the file could not be written — a full disk, a read-only
// home — so a dialog can say so rather than silently discarding the change.
// Also false, and without writing anything, when the file exists and cannot
// be read: an unreadable file is not an empty one, and treating it as empty
// would replace the reader's whole configuration with this one key.
bool save_setting(const std::filesystem::path& path, const std::string& section, const std::string& key,
                  const std::string& value);

// The spelling save_setting expects for a bool, and the one load_settings
// accepts: the configuration spec says `true`/`false` and nothing else, so that a config
// file reads the same way everywhere.
std::string bool_setting(bool value);
// The same, for the two enums a dialog can change. Written here rather than
// spelled out at each call site: the parser's own table is the authority on
// what a value is called, and a dialog that invented its own spelling would
// write a file the parser then warns about.
std::string clock_setting(ClockMode value);
std::string theme_setting(Theme value);

}  // namespace ckm
