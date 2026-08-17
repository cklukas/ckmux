// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "common/config.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

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
    enum class Kind { Blank, Comment, Section, Assignment, Malformed } kind = Kind::Blank;
    std::string_view section;  // Section
    std::string_view key;      // Assignment
    std::string_view value;    // Assignment, comment already stripped
    std::string_view trailing_comment;  // Assignment, including its '#'
};

ParsedLine parse_line(std::string_view line) {
    ParsedLine parsed;
    const std::string_view body = trim(line);
    if (body.empty()) return parsed;
    if (body.front() == '#') {
        parsed.kind = ParsedLine::Kind::Comment;
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

}  // namespace

std::string bool_setting(bool value) { return value ? "true" : "false"; }

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
                if (section == "general" && parsed.key == "login-shell") {
                    if (parsed.value == "true") loaded.settings.login_shell = true;
                    else if (parsed.value == "false") loaded.settings.login_shell = false;
                    else
                        loaded.warnings.push_back(
                            warning(path, line_number,
                                    "login-shell wants true or false, not '" + std::string(parsed.value) +
                                        "' — keeping " + bool_setting(loaded.settings.login_shell)));
                } else if (section == "terminal" && parsed.key == "sixel-max-megapixels") {
                    if (const std::optional<int> megapixels = parse_sixel_megapixels(parsed.value))
                        loaded.settings.sixel_max_megapixels = *megapixels;
                    else
                        loaded.warnings.push_back(
                            warning(path, line_number,
                                    "sixel-max-megapixels wants a whole number of megapixels between " +
                                        std::to_string(kSixelMegapixelsMin) + " and " +
                                        std::to_string(kSixelMegapixelsMax) + ", not '" +
                                        std::string(parsed.value) + "' — keeping " +
                                        std::to_string(loaded.settings.sixel_max_megapixels)));
                } else {
                    // Kept, never fatal: the plan's file lists keys ckmux
                    // will honour in later milestones, and a reader who has
                    // written one ahead of time should not be stopped.
                    loaded.warnings.push_back(
                        warning(path, line_number,
                                "unknown key '" + std::string(parsed.key) + "'" +
                                    (section.empty() ? " outside any section" : " in [" + section + "]")));
                }
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
