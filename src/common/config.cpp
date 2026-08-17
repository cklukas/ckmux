// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "common/config.hpp"

#include <array>
#include <charconv>
#include <fstream>
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
// `key = value`: `bind <context> <chord> <action>`.
std::vector<std::string_view> split_words(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) ++position;
        const std::size_t start = position;
        while (position < text.size() && text[position] != ' ' && text[position] != '\t') ++position;
        if (position > start) words.push_back(text.substr(start, position - start));
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
        if (const std::size_t hash = directive.find('#'); hash != std::string_view::npos)
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
    // example file. A value that needs one is not a value ckmux has.
    if (const std::size_t hash = value.find('#'); hash != std::string_view::npos) {
        parsed.trailing_comment = trim(value.substr(hash));
        value = value.substr(0, hash);
    }
    parsed.value = trim(value);
    return parsed;
}

std::vector<std::string> read_lines(const std::filesystem::path& path, bool& existed) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    existed = in.good();
    if (!existed) return lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

std::string warning(const std::filesystem::path& path, std::size_t line_number, const std::string& text) {
    std::ostringstream out;
    out << path.filename().string() << ':' << line_number << ": " << text;
    return out.str();
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
constexpr std::array<EnumChoice<PrinterSaveFormat>, 2> kSaveFormats{
    {{"txt", PrinterSaveFormat::Text}, {"ansi", PrinterSaveFormat::Ansi}}};

// One assignment, applied to `settings`. Returns what to warn about, or
// nothing when the line was obeyed. Written as one function over
// (section, key) pairs so that adding a key is one case rather than an edit
// in three places.
std::optional<std::string> apply_assignment(std::string_view section, std::string_view key,
                                            std::string_view value, Settings& settings) {
    const std::string written(value);

    const auto boolean = [&](bool& target) -> std::optional<std::string> {
        if (const std::optional<bool> parsed = parse_bool(value)) {
            target = *parsed;
            return std::nullopt;
        }
        return rejected(key, "true or false", written, bool_setting(target));
    };
    const auto integer = [&](int& target, int minimum, int maximum) -> std::optional<std::string> {
        if (const std::optional<int> parsed = parse_int_in_range(value, minimum, maximum)) {
            target = *parsed;
            return std::nullopt;
        }
        return rejected(key,
                        "a whole number between " + std::to_string(minimum) + " and " +
                            std::to_string(maximum),
                        written, std::to_string(target));
    };
    const auto bytes = [&](std::size_t& target) -> std::optional<std::string> {
        if (const std::optional<std::size_t> parsed = parse_byte_size(value)) {
            target = *parsed;
            return std::nullopt;
        }
        return rejected(key, "a byte size such as 256K or 1M", written, std::to_string(target) + " bytes");
    };
    const auto enumerated = [&]<typename Value, std::size_t N>(
                                Value& target,
                                const std::array<EnumChoice<Value>, N>& table) -> std::optional<std::string> {
        if (const std::optional<Value> parsed = parse_enum(value, table)) {
            target = *parsed;
            return std::nullopt;
        }
        return rejected(key, enum_names(table), written, enum_spelling(target, table));
    };

    if (section == "general") {
        if (key == "prefix") {
            if (const std::optional<ckv::KeyChord> chord = parse_chord(value)) {
                settings.prefix = *chord;
                return std::nullopt;
            }
            return rejected(key, "a chord such as C-b, C-a, F12 or Esc", written,
                            chord_spelling(settings.prefix));
        }
        if (key == "shell") {
            // Not validated against the filesystem. A shell that is not there
            // is a launch failure with its own message, and a config file
            // read on a machine where the path is about to exist — a home
            // directory shared across machines — should not be second-guessed
            // here. `$SHELL` is the documented way to say "the default".
            settings.shell = (value == "$SHELL") ? std::string{} : written;
            return std::nullopt;
        }
        if (key == "login-shell") return boolean(settings.login_shell);
        if (key == "scrollback") return integer(settings.scrollback, kScrollbackMin, kScrollbackMax);
        if (key == "theme") return enumerated(settings.theme, kThemes);
        if (key == "on-exit") return enumerated(settings.on_exit, kExitPolicies);
        if (key == "confirm-kill") return boolean(settings.confirm_kill);
        if (key == "kill-empty-session") return boolean(settings.kill_empty_session);
        if (key == "clock") return enumerated(settings.clock, kClockModes);
    } else if (section == "terminal") {
        if (key == "term") {
            settings.term = written.empty() ? std::string("auto") : written;
            return std::nullopt;
        }
        if (key == "kill-grace-seconds") {
            if (const std::optional<int> seconds = parse_int_in_range(value, 0, 600)) {
                settings.kill_grace_seconds = *seconds;
                return std::nullopt;
            }
            return rejected(key, "a whole number of seconds between 0 and 600", written,
                            std::to_string(settings.kill_grace_seconds));
        }
        if (key == "mouse") return boolean(settings.mouse);
        if (key == "alternate-scroll") return boolean(settings.alternate_scroll);
        if (key == "sixel") return enumerated(settings.sixel, kSixelModes);
        if (key == "sixel-max-megapixels") {
            if (const std::optional<int> megapixels = parse_sixel_megapixels(value)) {
                settings.sixel_max_megapixels = *megapixels;
                return std::nullopt;
            }
            return rejected(key,
                            "a whole number of megapixels between " + std::to_string(kSixelMegapixelsMin) +
                                " and " + std::to_string(kSixelMegapixelsMax),
                            written, std::to_string(settings.sixel_max_megapixels));
        }
        if (key == "osc52") return boolean(settings.osc52);
        if (key == "clipboard") {
            if (std::optional<std::vector<ClipboardTarget>> targets = parse_clipboard_targets(value)) {
                settings.clipboard = *std::move(targets);
                return std::nullopt;
            }
            return rejected(key, "a comma-separated list of osc52, pbcopy or exec:<command>", written,
                            "the built-in order");
        }
    } else if (section == "printer") {
        if (key == "mode") return enumerated(settings.printer_mode, kPrinterModes);
        if (key == "ask-cache") return bytes(settings.printer_ask_cache_bytes);
        if (key == "spool-limit") return bytes(settings.printer_spool_limit_bytes);
        if (key == "save-format") return enumerated(settings.printer_save_format, kSaveFormats);
        if (key == "save-folder") {
            settings.printer_save_folder = written;
            return std::nullopt;
        }
        if (key == "save-ask-name") return boolean(settings.printer_save_ask_name);
    } else if (section == "render") {
        if (key == "max-fps") return integer(settings.max_fps, kMaxFpsMin, kMaxFpsMax);
    }

    // Kept, never fatal: a typo in one setting is no reason to refuse to
    // start, and a key from a newer ckmux should not stop an older one.
    return "unknown key '" + std::string(key) + "'" +
           (section.empty() ? " outside any section" : " in [" + std::string(section) + "]");
}

// `bind <context> <chord> <action>` or `unbind <context> <chord>`. Positional
// and repeatable; order matters, so directives are appended rather than
// merged, and the keymap resolves them in file order.
std::optional<std::string> apply_directive(std::string_view line, Settings& settings) {
    const std::vector<std::string_view> words = split_words(line);
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
    // Deliberately not yes/no/on/off/1/0. plans/08 says true and false, so a
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
    // the package named beside it in plans/12-work-packages.md. Listed so
    // `check-config` can answer "I set this, why did nothing happen?" without
    // a reader having to find the roadmap.
    return {
        "[general] on-exit (WP-13)",
        "[general] kill-empty-session (WP-13)",
        "[printer] mode (U1-c, PRINT-1…6)",
        "[printer] ask-cache (U1-c, PRINT-1…6)",
        "[printer] spool-limit (U1-c, PRINT-1…6)",
        "[printer] save-format (U1-c, PRINT-1…6)",
        "[printer] save-folder (U1-c, PRINT-1…6)",
        "[printer] save-ask-name (U1-c, PRINT-1…6)",
        "[render] max-fps (WP-2)",
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
    bool existed = false;
    const std::vector<std::string> lines = read_lines(path, existed);
    // Not a warning. An unconfigured ckmux is the ordinary case, and telling
    // a reader about a file they never created would be noise.
    if (!existed) return loaded;

    std::string section;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const ParsedLine parsed = parse_line(lines[i]);
        const std::size_t line_number = i + 1;
        switch (parsed.kind) {
            case ParsedLine::Kind::Blank:
            case ParsedLine::Kind::Comment:
                break;
            case ParsedLine::Kind::Section:
                section = std::string(parsed.section);
                break;
            case ParsedLine::Kind::Malformed:
                loaded.warnings.push_back(warning(path, line_number, "not a section header or key = value"));
                break;
            case ParsedLine::Kind::Assignment:
                if (const std::optional<std::string> problem =
                        apply_assignment(section, parsed.key, parsed.value, loaded.settings))
                    loaded.warnings.push_back(warning(path, line_number, *problem));
                break;
            case ParsedLine::Kind::Directive:
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
    bool existed = false;
    std::vector<std::string> lines = read_lines(path, existed);

    // Where the key belongs, found in one pass: the line to rewrite if it is
    // already there, or else the point in its section to insert at.
    //
    // That point is after the section's last non-blank line, not immediately
    // before the next section header. The blank line between two sections is
    // the reader's paragraph break, and a new key dropped below it would
    // read as belonging to the section that follows.
    std::size_t rewrite_at = lines.size() + 1;
    std::size_t insert_at = lines.size() + 1;
    bool in_section = false;
    bool section_seen = false;
    for (std::size_t i = 0; i < lines.size() && rewrite_at > lines.size(); ++i) {
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
            rewrite_at = i;
            // Keep whatever the reader wrote beside it: an inline comment is
            // a note to themselves about this very setting, and a dialog
            // that erased it would be taking something it was not offered.
            std::string replacement = key + " = " + value;
            if (!parsed.trailing_comment.empty())
                replacement += "  " + std::string(parsed.trailing_comment);
            lines[i] = std::move(replacement);
        }
    }

    if (rewrite_at > lines.size()) {
        if (section_seen && insert_at <= lines.size()) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), key + " = " + value);
        } else {
            // No such section anywhere: append one, after a blank line if
            // the file already has content that does not end in one.
            if (!lines.empty() && !trim(lines.back()).empty()) lines.emplace_back();
            lines.push_back("[" + section + "]");
            lines.push_back(key + " = " + value);
        }
    }

    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
    }
    // Written beside the target and renamed over it, so an interrupted save
    // leaves the reader's existing configuration intact rather than a half
    // file where their settings used to be.
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return false;
        for (const std::string& line : lines) out << line << '\n';
        out.flush();
        if (!out) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

}  // namespace ckm
