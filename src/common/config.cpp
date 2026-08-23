// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/config.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace ckm {
namespace {

std::string_view trim(std::string_view text) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    return text;
}

// Where a comment starts in `text`, or npos. A '#' written `\#` is a literal
// '#' rather than the start of a comment — the one escape this format has
// (the configuration spec), so that `exec:my-copy --tag=\#1` is a command and not half of
// one. Every other backslash stands for itself, including one before another
// backslash: the values that need the escape are shell commands, and a format
// that rewrote `\\` or `\n` would break more commands than it saved.
std::size_t find_comment(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '#') {
            ++i;  // that '#' is spoken for
            continue;
        }
        if (text[i] == '#') return i;
    }
    return std::string_view::npos;
}

// What the reader meant, once that one escape is resolved.
std::string unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '#') {
            out += '#';
            ++i;
            continue;
        }
        out += text[i];
    }
    return out;
}

// The inverse, for the values a dialog writes back: a '#' in a value is
// written `\#`, so that what save_setting writes is what load_settings reads.
// Nothing a dialog writes today contains one; the round trip should not have
// to depend on that staying true.
std::string escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '#') out += '\\';
        out += c;
    }
    return out;
}

// A line's meaning, decided once so the reader and the writer cannot
// disagree about what a line is.
struct ParsedLine {
    enum class Kind { Blank, Comment, Section, Assignment, Directive, Malformed } kind = Kind::Blank;
    std::string_view section;  // Section
    std::string_view key;      // Assignment
    std::string_view value;    // Assignment, comment already stripped
    std::string_view trailing_comment;  // Assignment, including its '#'
    std::string_view directive;  // Directive: the whole line, comment stripped
};

// The words of a directive line, which is the one line shape that is not
// `key = value`: `bind <context> <chord> <action>`. Unescaped word by word,
// because '#' is a chord a reader may want to bind and `\#` is how they write
// it.
std::vector<std::string> split_words(std::string_view text) {
    std::vector<std::string> words;
    std::size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) ++position;
        const std::size_t start = position;
        while (position < text.size() && text[position] != ' ' && text[position] != '\t') ++position;
        if (position > start) words.push_back(unescape(text.substr(start, position - start)));
    }
    return words;
}

bool starts_with_word(std::string_view body, std::string_view word) {
    return body.size() > word.size() && body.compare(0, word.size(), word) == 0 &&
           (body[word.size()] == ' ' || body[word.size()] == '\t');
}

ParsedLine parse_line(std::string_view line) {
    ParsedLine parsed;
    const std::string_view body = trim(line);
    if (body.empty()) return parsed;
    if (body.front() == '#') {
        parsed.kind = ParsedLine::Kind::Comment;
        return parsed;
    }
    // `bind`/`unbind` are directives, not assignments: they repeat, they are
    // positional, and there is no key to rewrite. Recognised before the '='
    // search so that a malformed one is reported as a bad directive rather
    // than as a line that forgot its equals sign.
    if (starts_with_word(body, "bind") || starts_with_word(body, "unbind")) {
        parsed.kind = ParsedLine::Kind::Directive;
        std::string_view directive = body;
        if (const std::size_t hash = find_comment(directive); hash != std::string_view::npos)
            directive = directive.substr(0, hash);
        parsed.directive = trim(directive);
        return parsed;
    }
    if (body.front() == '[') {
        const std::size_t close = body.find(']');
        if (close == std::string_view::npos) {
            parsed.kind = ParsedLine::Kind::Malformed;
            return parsed;
        }
        parsed.kind = ParsedLine::Kind::Section;
        parsed.section = trim(body.substr(1, close - 1));
        return parsed;
    }
    const std::size_t equals = body.find('=');
    if (equals == std::string_view::npos) {
        parsed.kind = ParsedLine::Kind::Malformed;
        return parsed;
    }
    parsed.kind = ParsedLine::Kind::Assignment;
    parsed.key = trim(body.substr(0, equals));
    std::string_view value = body.substr(equals + 1);
    // A '#' after a value is a comment, as in the plan's own annotated
    // example file; `\#` is a '#' the value keeps. The value is left escaped
    // here — what the reader wrote is what a message has to quote back at
    // them — and resolved where it is used.
    if (const std::size_t hash = find_comment(value); hash != std::string_view::npos) {
        parsed.trailing_comment = trim(value.substr(hash));
        value = value.substr(0, hash);
    }
    parsed.value = trim(value);
    return parsed;
}

// What became of an attempt to read the file. `Absent` and `Unreadable` are
// deliberately different answers: the first is an unconfigured ckmux, which is
// the ordinary case and silent, and the second is a file the reader wrote and
// ckmux ignored, which is never silent (m-conf).
enum class ReadOutcome : unsigned char { Read, Absent, Unreadable };

ReadOutcome read_lines(const std::filesystem::path& path, std::vector<std::string>& lines,
                       std::string& reason) {
    errno = 0;
    std::ifstream in(path);
    if (!in) {
        // errno is taken before anything else can overwrite it: the stream
        // says only "it failed", and "Permission denied" is the whole
        // difference between a file the reader must fix and one they never
        // made.
        const int failure = errno;
        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::status(path, error);
        // `status` reports not_found for exactly the two errors that mean the
        // file is not there (ENOENT, ENOTDIR). Anything else — including a
        // status it could not obtain at all — means something is there.
        if (status.type() == std::filesystem::file_type::not_found) return ReadOutcome::Absent;
        reason = failure == 0 ? std::string("it could not be opened")
                              : std::error_code(failure, std::generic_category()).message();
        return ReadOutcome::Unreadable;
    }
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    // A read that stopped on an I/O error rather than at the end of the file
    // has half a configuration in hand, and half a configuration silently
    // applied is worse than none.
    if (in.bad()) {
        reason = "it could not be read to the end";
        return ReadOutcome::Unreadable;
    }
    return ReadOutcome::Read;
}

// Force what has been written to `path` out to the disk itself. Opened
// read-only purely for the descriptor: fsync is about the file, not about the
// access mode of the handle it is asked through.
bool flush_file(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) return false;
    const int flushed = ::fsync(descriptor);
    ::close(descriptor);
    return flushed == 0;
}

// The same for the directory entry, which is a different thing on disk from
// the file it names. Best effort by contract: its only caller has already
// renamed the new file into place, so there is no failure left to report and
// nothing to undo.
void flush_directory(const std::filesystem::path& directory) {
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) return;
    (void)::fsync(descriptor);
    ::close(descriptor);
}

// Warnings name the file in full. The basename alone is ambiguous the moment
// a reader has a `$CKMUX_CONFIG` for testing beside their real one — and it is
// not a path they can paste into an editor (m-conf).
std::string warning(const std::filesystem::path& path, std::size_t line_number, const std::string& text) {
    std::ostringstream out;
    out << path.string() << ':' << line_number << ": " << text;
    return out.str();
}

// The same, for something wrong with the file rather than with a line in it.
std::string file_warning(const std::filesystem::path& path, const std::string& text) {
    return path.string() + ": " + text;
}

// Every section name the file may contain, in the order the configuration spec's example
// file writes them. A header that is not one of these is a typo, and one
// warning at the header beats one at every key underneath it (m-conf).
constexpr std::array<std::string_view, 5> kSections{"general", "terminal", "printer", "render",
                                                    "keys"};

bool section_is_known(std::string_view name) {
    for (const std::string_view candidate : kSections)
        if (candidate == name) return true;
    return false;
}

std::string section_name_list() {
    std::string out;
    for (const std::string_view name : kSections) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

// "key wants one of a, b, c, not 'x' — keeping y". Every rejection says all
// four things: what was asked for, what was written, that the line was not
// obeyed, and what is in force instead. A message missing the last one leaves
// a reader who wrote `theme = drak` unsure whether they now have light.
std::string rejected(std::string_view key, std::string_view wanted, std::string_view written,
                     std::string_view kept) {
    std::string out(key);
    out += " wants ";
    out += wanted;
    out += ", not '";
    out += written;
    out += "' — keeping ";
    out += kept;
    return out;
}

// An enum key: the spelling table, so the message can list the alternatives
// without a second copy of them.
template <typename Value>
struct EnumChoice {
    std::string_view name;
    Value value;
};

template <typename Value, std::size_t N>
std::optional<Value> parse_enum(std::string_view text, const std::array<EnumChoice<Value>, N>& table) {
    for (const EnumChoice<Value>& choice : table)
        if (choice.name == text) return choice.value;
    return std::nullopt;
}

template <typename Value, std::size_t N>
std::string enum_names(const std::array<EnumChoice<Value>, N>& table) {
    std::string out;
    for (const EnumChoice<Value>& choice : table) {
        if (!out.empty()) out += " | ";
        out += choice.name;
    }
    return out;
}

template <typename Value, std::size_t N>
std::string_view enum_spelling(Value value, const std::array<EnumChoice<Value>, N>& table) {
    for (const EnumChoice<Value>& choice : table)
        if (choice.value == value) return choice.name;
    return {};
}

constexpr std::array<EnumChoice<Theme>, 3> kThemes{
    {{"dark", Theme::Dark}, {"light", Theme::Light}, {"mono", Theme::Mono}}};
// `minutes` is the clock without a second hand — named for what it shows
// rather than for what it leaves out, so the three spellings read as three
// answers to one question.
constexpr std::array<EnumChoice<ClockMode>, 3> kClockModes{
    {{"seconds", ClockMode::Seconds}, {"minutes", ClockMode::Minutes}, {"off", ClockMode::Off}}};
constexpr std::array<EnumChoice<ExitPolicy>, 3> kExitPolicies{{{"close", ExitPolicy::Close},
                                                               {"hold", ExitPolicy::Hold},
                                                               {"hold-on-error", ExitPolicy::HoldOnError}}};
constexpr std::array<EnumChoice<SixelMode>, 2> kSixelModes{
    {{"auto", SixelMode::Auto}, {"off", SixelMode::Off}}};
constexpr std::array<EnumChoice<PrinterMode>, 3> kPrinterModes{
    {{"ask", PrinterMode::Ask}, {"capture", PrinterMode::Capture}, {"off", PrinterMode::Off}}};
constexpr std::array<EnumChoice<DesktopSizePolicy>, 3> kDesktopSizePolicies{
    {{"fixed", DesktopSizePolicy::Fixed},
     {"fit-smallest", DesktopSizePolicy::FitSmallest},
     {"fit-latest", DesktopSizePolicy::FitLatest}}};
constexpr std::array<EnumChoice<PrinterSaveFormat>, 2> kSaveFormats{
    {{"txt", PrinterSaveFormat::Text}, {"ansi", PrinterSaveFormat::Ansi}}};

// What became of one assignment. `Unknown` is the only outcome the caller has
// to think twice about: the key may not exist at all, or it may exist in a
// different section, and a reader who wrote the second one is helped by being
// told where it lives rather than that it does not.
enum class AssignmentOutcome : unsigned char { Applied, Refused, Unknown };

struct AssignmentResult {
    AssignmentOutcome outcome = AssignmentOutcome::Applied;
    std::string message;  // empty when the line was obeyed
};

// One assignment, applied to `settings`. Written as one function over
// (section, key) pairs so that adding a key is one case rather than an edit
// in three places — and so that "which section knows this key?" can be
// answered by asking this function rather than from a second table.
AssignmentResult apply_assignment(std::string_view section, std::string_view key,
                                  std::string_view value, Settings& settings) {
    // What the file literally wrote, kept for the message so a reader can find
    // the text again in their own file; and what it means, once the format's
    // one escape (`\#`) is resolved.
    const std::string written(value);
    const std::string plain = unescape(value);
    value = plain;

    const auto refuse = [](std::string message) {
        return AssignmentResult{AssignmentOutcome::Refused, std::move(message)};
    };
    const auto applied = [] { return AssignmentResult{AssignmentOutcome::Applied, {}}; };

    const auto boolean = [&](bool& target) -> AssignmentResult {
        if (const std::optional<bool> parsed = parse_bool(value)) {
            target = *parsed;
            return applied();
        }
        return refuse(rejected(key, "true or false", written, bool_setting(target)));
    };
    const auto integer = [&](int& target, int minimum, int maximum) -> AssignmentResult {
        if (const std::optional<int> parsed = parse_int_in_range(value, minimum, maximum)) {
            target = *parsed;
            return applied();
        }
        return refuse(rejected(key,
                               "a whole number between " + std::to_string(minimum) + " and " +
                                   std::to_string(maximum),
                               written, std::to_string(target)));
    };
    const auto bytes = [&](std::size_t& target) -> AssignmentResult {
        if (const std::optional<std::size_t> parsed = parse_byte_size(value)) {
            target = *parsed;
            return applied();
        }
        return refuse(
            rejected(key, "a byte size such as 256K or 1M", written, std::to_string(target) + " bytes"));
    };
    const auto enumerated = [&]<typename Value, std::size_t N>(
                                Value& target,
                                const std::array<EnumChoice<Value>, N>& table) -> AssignmentResult {
        if (const std::optional<Value> parsed = parse_enum(value, table)) {
            target = *parsed;
            return applied();
        }
        return refuse(rejected(key, enum_names(table), written, enum_spelling(target, table)));
    };

    if (section == "general") {
        if (key == "prefix") {
            if (const std::optional<ckv::KeyChord> chord = parse_chord(value)) {
                settings.prefix = *chord;
                return applied();
            }
            return refuse(rejected(key, "a chord such as ^B, ^A, F12 or Esc", written,
                                   chord_spelling(settings.prefix)));
        }
        if (key == "shell") {
            // Not validated against the filesystem. A shell that is not there
            // is a launch failure with its own message, and a config file
            // read on a machine where the path is about to exist — a home
            // directory shared across machines — should not be second-guessed
            // here. `$SHELL` is the documented way to say "the default".
            settings.shell = (value == "$SHELL") ? std::string{} : plain;
            return applied();
        }
        if (key == "login-shell") return boolean(settings.login_shell);
        if (key == "scrollback") return integer(settings.scrollback, kScrollbackMin, kScrollbackMax);
        if (key == "max-terminals")
            return integer(settings.max_terminals, kMaxTerminalsMin, kMaxTerminalsMax);
        if (key == "theme") return enumerated(settings.theme, kThemes);
        if (key == "on-exit") return enumerated(settings.on_exit, kExitPolicies);
        if (key == "audible-bell") return boolean(settings.audible_bell);
        if (key == "confirm-kill") return boolean(settings.confirm_kill);
        if (key == "kill-empty-session") return boolean(settings.kill_empty_session);
        // [general], where the configuration spec documents it, where this struct's own
        // comment says it is, and where `Settings ▸ Settings…` writes it. It
        // used to be parsed under [terminal] only, so the dialog's save was
        // read back as an unknown key and the reader was blamed for a line
        // ckmux had written itself (M-T1). Written under [terminal] it is now
        // refused out loud, naming [general] — silent acceptance would leave
        // two live copies of one setting, and the dialog would go on writing
        // to the other one.
        if (key == "kill-grace-seconds") {
            if (const std::optional<int> seconds = parse_int_in_range(value, 0, 600)) {
                settings.kill_grace_seconds = *seconds;
                return applied();
            }
            return refuse(rejected(key, "a whole number of seconds between 0 and 600", written,
                                   std::to_string(settings.kill_grace_seconds)));
        }
        if (key == "clock") return enumerated(settings.clock, kClockModes);
        // [general], where the configuration spec documents it and where `Settings ▸
        // General…` writes it — the one place a key may be read from, since
        // M-T1 was a key parsed under a section the dialog did not write to and
        // a reader blamed for a line ckmux had written itself.
        if (key == "resize-windows-to-fit") return boolean(settings.resize_windows_to_fit);
        // A session's desktop, not this client's screen — the two are
        // different numbers and only one of them reflows anybody's windows
        // (WP-40).
        if (key == "desktop-size")
            return enumerated(settings.desktop_size, kDesktopSizePolicies);
        // The View menu's readouts (WP-39), written back by the menu items
        // themselves through `save_setting` exactly as the Settings dialog's
        // checkboxes are — so the preference survives the session because the
        // reader flipped it, not because they remembered to edit a file.
        if (key == "show-cpu") return boolean(settings.show_cpu);
        if (key == "show-memory-rss") return boolean(settings.show_memory_rss);
        if (key == "show-memory-real") return boolean(settings.show_memory_real);
    } else if (section == "terminal") {
        if (key == "term") {
            settings.term = plain.empty() ? std::string("auto") : plain;
            return applied();
        }
        if (key == "mouse") return boolean(settings.mouse);
        if (key == "alternate-scroll") return boolean(settings.alternate_scroll);
        if (key == "sixel") return enumerated(settings.sixel, kSixelModes);
        if (key == "sixel-max-megapixels") {
            if (const std::optional<int> megapixels = parse_sixel_megapixels(value)) {
                settings.sixel_max_megapixels = *megapixels;
                return applied();
            }
            return refuse(rejected(key,
                                   "a whole number of megapixels between " +
                                       std::to_string(kSixelMegapixelsMin) + " and " +
                                       std::to_string(kSixelMegapixelsMax),
                                   written, std::to_string(settings.sixel_max_megapixels)));
        }
        if (key == "osc52") return boolean(settings.osc52);
        if (key == "clipboard") {
            if (std::optional<std::vector<ClipboardTarget>> targets = parse_clipboard_targets(value)) {
                settings.clipboard = *std::move(targets);
                return applied();
            }
            return refuse(rejected(key, "a comma-separated list of osc52, pbcopy or exec:<command>",
                                   written, "the built-in order"));
        }
    } else if (section == "printer") {
        if (key == "mode") return enumerated(settings.printer_mode, kPrinterModes);
        if (key == "ask-cache") return bytes(settings.printer_ask_cache_bytes);
        if (key == "spool-limit") return bytes(settings.printer_spool_limit_bytes);
        if (key == "save-format") return enumerated(settings.printer_save_format, kSaveFormats);
        if (key == "save-folder") {
            settings.printer_save_folder = plain;
            return applied();
        }
        if (key == "save-ask-name") return boolean(settings.printer_save_ask_name);
    } else if (section == "render") {
        if (key == "max-fps") return integer(settings.max_fps, kMaxFpsMin, kMaxFpsMax);
    }

    // Kept, never fatal: a typo in one setting is no reason to refuse to
    // start, and a key from a newer ckmux should not stop an older one.
    return AssignmentResult{AssignmentOutcome::Unknown,
                            "unknown key '" + std::string(key) + "'" +
                                (section.empty() ? " outside any section"
                                                 : " in [" + std::string(section) + "]")};
}

// Which section does know this key, if any. Asked by offering the key to each
// other section rather than answered from a second table, because a second
// table is a thing that drifts away from the first one. Only ever reached
// after a key was not recognised, so a handful of default `Settings` is a cost
// paid on a path that is already an error.
std::optional<std::string_view> section_that_knows(std::string_view key, std::string_view written_in) {
    for (const std::string_view candidate : kSections) {
        if (candidate == written_in) continue;
        Settings scratch;
        if (apply_assignment(candidate, key, {}, scratch).outcome != AssignmentOutcome::Unknown)
            return candidate;
    }
    return std::nullopt;
}

// A key that exists, under a header it does not belong to — a likelier mistake
// than a typo, and one nobody can fix without being told where the key lives.
// The four things again: what was wanted (that section), what was written
// (this one), that the line was not obeyed, and what is in force instead.
std::string misplaced(std::string_view key, std::string_view owner, std::string_view written_in) {
    return std::string(key) + " belongs in [" + std::string(owner) + "], not [" +
           std::string(written_in) + "] — not obeyed; [" + std::string(owner) + "]'s value stands";
}

// `bind <context> <chord> <action>` or `unbind <context> <chord>`. Positional
// and repeatable; order matters, so directives are appended rather than
// merged, and the keymap resolves them in file order.
std::optional<std::string> apply_directive(std::string_view line, Settings& settings) {
    const std::vector<std::string> words = split_words(line);
    // parse_line only calls a line a directive when it starts with the word
    // `bind` or `unbind`, so there is always a first word — but this function
    // reads it, and a function that reads words[0] should be the one that says
    // so rather than the one that trusts a caller two screens away.
    if (words.empty()) return "not a bind or unbind line";
    const bool unbinding = words[0] == "unbind";
    const std::size_t expected = unbinding ? 3U : 4U;
    if (words.size() != expected)
        return std::string(words[0]) + " wants " +
               (unbinding ? "a context and a chord: unbind <context> <chord>"
                          : "a context, a chord and an action: bind <context> <chord> <action>");

    const std::optional<KeyContext> context = parse_key_context(words[1]);
    if (!context)
        return "no such context '" + std::string(words[1]) + "' — try one of " + key_context_name_list();

    const std::optional<ckv::KeyChord> chord = parse_chord(words[2]);
    if (!chord)
        return "'" + std::string(words[2]) +
               "' is not a chord — try a character, C-x, A-x, F1-F12, or a named key such as Enter, "
               "Esc, Space or Up";

    BindDirective directive;
    directive.context = *context;
    // Stored as the canonical spelling rather than as the reader wrote it:
    // dispatch looks a chord up by its spelling, and `A-x` and `M-x` are the
    // same key written two ways.
    directive.chord = chord_spelling(*chord);
    if (!unbinding) {
        const std::optional<Action> action = parse_action(words[3]);
        if (!action)
            return "no such action '" + std::string(words[3]) + "' — try one of " + action_name_list();
        directive.action = *action;
    }
    settings.binds.push_back(std::move(directive));
    return std::nullopt;
}

}  // namespace

std::string bool_setting(bool value) { return value ? "true" : "false"; }

std::string clock_setting(ClockMode value) { return std::string(enum_spelling(value, kClockModes)); }

std::string theme_setting(Theme value) { return std::string(enum_spelling(value, kThemes)); }

std::optional<bool> parse_bool(std::string_view text) {
    const std::string_view body = trim(text);
    if (body == "true") return true;
    if (body == "false") return false;
    // Deliberately not yes/no/on/off/1/0. The configuration spec says true and false, so a
    // config file reads the same way everywhere; a second spelling is a
    // second thing to remember and a second thing to get wrong.
    return std::nullopt;
}

std::optional<int> parse_int_in_range(std::string_view text, int minimum, int maximum) {
    const std::string_view body = trim(text);
    if (body.empty()) return std::nullopt;
    int value = 0;
    const auto result = std::from_chars(body.data(), body.data() + body.size(), value);
    if (result.ec != std::errc{} || result.ptr != body.data() + body.size()) return std::nullopt;
    if (value < minimum || value > maximum) return std::nullopt;
    return value;
}

std::optional<std::size_t> parse_byte_size(std::string_view text) {
    std::string_view body = trim(text);
    if (body.empty()) return std::nullopt;
    std::size_t multiplier = 1;
    switch (body.back()) {
        case 'K': multiplier = 1024ULL; break;
        case 'M': multiplier = 1024ULL * 1024ULL; break;
        case 'G': multiplier = 1024ULL * 1024ULL * 1024ULL; break;
        default: break;
    }
    // Upper case only, and only these three. A lower-case `k` in a file that
    // also contains `64` for megapixels would invite the reading that some
    // numbers carry units and others carry a different unit silently.
    if (multiplier != 1) body.remove_suffix(1);
    if (body.empty()) return std::nullopt;
    unsigned long long value = 0;
    const auto result = std::from_chars(body.data(), body.data() + body.size(), value);
    if (result.ec != std::errc{} || result.ptr != body.data() + body.size()) return std::nullopt;
    if (value > kByteSizeMax / multiplier) return std::nullopt;
    return static_cast<std::size_t>(value * multiplier);
}

std::optional<std::vector<ClipboardTarget>> parse_clipboard_targets(std::string_view text) {
    std::vector<ClipboardTarget> targets;
    std::string_view rest = trim(text);
    if (rest.empty()) return std::nullopt;
    while (true) {
        const std::size_t comma = rest.find(',');
        const std::string_view entry =
            trim(comma == std::string_view::npos ? rest : rest.substr(0, comma));
        if (entry == "osc52") {
            targets.push_back(ClipboardTarget{ClipboardTarget::Kind::Osc52, {}});
        } else if (entry == "pbcopy") {
            targets.push_back(ClipboardTarget{ClipboardTarget::Kind::Pbcopy, {}});
        } else if (entry.starts_with("exec:")) {
            const std::string_view command = trim(entry.substr(5));
            // `exec:` with nothing after it is a target that cannot run; it is
            // rejected here rather than discovered at the first copy.
            if (command.empty()) return std::nullopt;
            targets.push_back(ClipboardTarget{ClipboardTarget::Kind::Exec, std::string(command)});
        } else {
            return std::nullopt;
        }
        if (comma == std::string_view::npos) break;
        rest = rest.substr(comma + 1);
    }
    return targets;
}

std::vector<std::string> keys_not_honoured_yet() {
    // Read and validated, but nothing acts on them yet — each one waits for
    // the package named beside it in the work queue. Listed so
    // `check-config` can answer "I set this, why did nothing happen?" without
    // a reader having to find the roadmap.
    //
    // The list lies in both directions if it is not maintained, and it did:
    // it named four keys that had since been wired up (m-conf). A key belongs
    // here only while NOTHING reads it. As of this audit, each line below was
    // checked against the code, and these four left because something does:
    //   [general] kill-empty-session  server.cpp forget_session, both call
    //                                 sites, plus the close dialog's warning
    //   [printer] mode                terminals.cpp launch profile (`off`
    //                                 denies capture)
    //   [printer] spool-limit         terminals.cpp max_printer_spool_bytes
    //   [render]  max-fps             server.cpp flush_tick period
    // And one more left with WP-13, for the same reason and found the same
    // way — by `check-config` printing it after the code had moved on:
    //   [general] on-exit             server.cpp's self-exit path reads it to
    //                                 decide whether an exited terminal is
    //                                 removed or held, and sets TermClosed.hold
    // A key some of whose values do nothing yet is not listed here — the list
    // is about keys, and `[printer] mode = ask` capturing without asking until
    // PRINT-1…6 lands is the configuration spec's to explain, not a key nothing reads.
    return {
        "[printer] ask-cache (PRINT-1…6)",
        "[printer] save-format (PRINT-1…6)",
        "[printer] save-folder (PRINT-1…6)",
        "[printer] save-ask-name (PRINT-1…6)",
    };
}

std::optional<int> parse_sixel_megapixels(std::string_view text) {
    const std::string_view body = trim(text);
    if (body.empty()) return std::nullopt;
    int value = 0;
    const auto result = std::from_chars(body.data(), body.data() + body.size(), value);
    // The whole field has to be the number. "64 or so" is not 64, and a
    // partial parse is how a typo turns into a setting nobody chose.
    if (result.ec != std::errc{} || result.ptr != body.data() + body.size()) return std::nullopt;
    if (value < kSixelMegapixelsMin || value > kSixelMegapixelsMax) return std::nullopt;
    return value;
}

LoadedSettings load_settings(const std::filesystem::path& path) {
    LoadedSettings loaded;
    if (path.empty()) return loaded;
    std::vector<std::string> lines;
    std::string reason;
    switch (read_lines(path, lines, reason)) {
        case ReadOutcome::Absent:
            // Not a warning. An unconfigured ckmux is the ordinary case, and
            // telling a reader about a file they never created would be noise.
            return loaded;
        case ReadOutcome::Unreadable:
            // The other half of that sentence, and the half that used to be
            // silent: a file the reader DID create, which ckmux then ignored
            // wholesale while behaving as though they had configured nothing
            // (m-conf). Named in full, with the reason, and with what is in
            // force instead.
            loaded.warnings.push_back(
                file_warning(path, "could not be read (" + reason + ") — keeping the built-in settings"));
            return loaded;
        case ReadOutcome::Read:
            break;
    }

    std::string section;
    // Before the first header there is no section rather than an unknown one:
    // an assignment there is reported as a key outside any section, which is
    // what it is.
    bool section_known = true;
    // Which (section, key) pairs the file has already set, so that a second
    // one says so instead of quietly winning. Duplicates are legal — the last
    // line wins, as it always has — but a reader whose dialog save "did
    // nothing" was usually looking at the wrong one of two lines.
    std::set<std::pair<std::string, std::string>> assigned;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const ParsedLine parsed = parse_line(lines[i]);
        const std::size_t line_number = i + 1;
        switch (parsed.kind) {
            case ParsedLine::Kind::Blank:
            case ParsedLine::Kind::Comment:
                break;
            case ParsedLine::Kind::Section: {
                section = std::string(parsed.section);
                section_known = section_is_known(section);
                // One warning at the header rather than one at every key
                // under it: a reader who typed `[generall]` has made a single
                // mistake and should be handed a single sentence (m-conf).
                if (!section_known)
                    loaded.warnings.push_back(
                        warning(path, line_number,
                                "no such section '[" + section + "]' — try one of " + section_name_list() +
                                    "; nothing under this header was read"));
                break;
            }
            case ParsedLine::Kind::Malformed:
                loaded.warnings.push_back(warning(path, line_number, "not a section header or key = value"));
                break;
            case ParsedLine::Kind::Assignment: {
                // Already covered, once, by the warning at the header.
                if (!section_known) break;
                const AssignmentResult result =
                    apply_assignment(section, parsed.key, parsed.value, loaded.settings);
                switch (result.outcome) {
                    case AssignmentOutcome::Applied:
                        if (!assigned.emplace(section, std::string(parsed.key)).second)
                            loaded.warnings.push_back(
                                warning(path, line_number,
                                        std::string(parsed.key) + " is set again here — the earlier line in [" +
                                            section + "] no longer applies"));
                        break;
                    case AssignmentOutcome::Refused:
                        loaded.warnings.push_back(warning(path, line_number, result.message));
                        break;
                    case AssignmentOutcome::Unknown:
                        if (const std::optional<std::string_view> owner =
                                section_that_knows(parsed.key, section))
                            loaded.warnings.push_back(
                                warning(path, line_number, misplaced(parsed.key, *owner, section)));
                        else
                            loaded.warnings.push_back(warning(path, line_number, result.message));
                        break;
                }
                break;
            }
            case ParsedLine::Kind::Directive:
                // Skipped under an unknown header too, so that the warning
                // there means exactly what it says: nothing under it was read.
                if (!section_known) break;
                if (const std::optional<std::string> problem =
                        apply_directive(parsed.directive, loaded.settings))
                    loaded.warnings.push_back(warning(path, line_number, *problem));
                break;
        }
    }
    return loaded;
}

bool save_setting(const std::filesystem::path& path, const std::string& section, const std::string& key,
                  const std::string& value) {
    if (path.empty()) return false;
    std::vector<std::string> lines;
    std::string reason;
    // A file that is there and unreadable is not an empty file. Reading it as
    // one would rewrite the reader's whole configuration as the single key
    // this call was asked to store — the worst thing this function could do,
    // and it would do it silently.
    if (read_lines(path, lines, reason) == ReadOutcome::Unreadable) return false;

    // The written line, and the place for it if the file has none yet.
    //
    // That place is after the section's last non-blank line, not immediately
    // before the next section header. The blank line between two sections is
    // the reader's paragraph break, and a new key dropped below it would
    // read as belonging to the section that follows.
    const std::string assignment = key + " = " + escape(value);
    std::size_t rewrites = 0;
    std::size_t insert_at = lines.size() + 1;
    bool in_section = false;
    bool section_seen = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const ParsedLine parsed = parse_line(lines[i]);
        if (parsed.kind == ParsedLine::Kind::Section) {
            in_section = parsed.section == section;
            if (in_section) {
                section_seen = true;
                insert_at = i + 1;  // an empty section takes the key directly
            }
            continue;
        }
        if (!in_section) continue;
        if (parsed.kind != ParsedLine::Kind::Blank) insert_at = i + 1;
        if (parsed.kind == ParsedLine::Kind::Assignment && parsed.key == key) {
            // Every occurrence, not only the first. A file that names one key
            // twice is read back last-line-wins, so rewriting only the first
            // left the dialog's Save with no visible effect and no way for the
            // reader to see why (m-conf). The reader's second line stays where
            // it is rather than being deleted — it is theirs, load_settings
            // already says out loud that the key is set twice, and a save is
            // not the moment to edit somebody's file for them.
            ++rewrites;
            // Keep whatever the reader wrote beside it: an inline comment is
            // a note to themselves about this very setting, and a dialog
            // that erased it would be taking something it was not offered.
            std::string replacement = assignment;
            if (!parsed.trailing_comment.empty())
                replacement += "  " + std::string(parsed.trailing_comment);
            lines[i] = std::move(replacement);
        }
    }

    if (rewrites == 0) {
        if (section_seen && insert_at <= lines.size()) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), assignment);
        } else {
            // No such section anywhere: append one, after a blank line if
            // the file already has content that does not end in one.
            if (!lines.empty() && !trim(lines.back()).empty()) lines.emplace_back();
            lines.push_back("[" + section + "]");
            lines.push_back(assignment);
        }
    }

    std::error_code error;
    const std::filesystem::path directory =
        path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    if (path.has_parent_path()) {
        std::filesystem::create_directories(directory, error);
        if (error) return false;
    }
    // Written beside the target and renamed over it, so an interrupted save
    // leaves the reader's existing configuration intact rather than a half
    // file where their settings used to be. The scratch name carries this
    // process's id: two ckmux clients saving at once would otherwise share one
    // scratch file, and the loser of that race would rename half of the
    // winner's bytes over the reader's configuration.
    const std::filesystem::path temporary =
        path.string() + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return false;
        for (const std::string& line : lines) out << line << '\n';
        out.flush();
        if (!out) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    // A rename is atomic but not durable. After a power loss the directory can
    // still name the old file, or name the new one with none of its bytes
    // behind it — and a settings dialog that reports success and loses the
    // setting is the defect this whole function exists to avoid. So both
    // halves are flushed: the contents before the rename, where a failure can
    // still be reported and the reader's file is still intact, and the
    // directory entry after it, where nothing is left to undo and the flush is
    // therefore best effort. (fsync, not F_FULLFSYNC: this is a text file a
    // person edits, not a database, and the portable barrier is the one whose
    // cost is proportionate.)
    if (!flush_file(temporary)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    flush_directory(directory);
    return true;
}

}  // namespace ckm
