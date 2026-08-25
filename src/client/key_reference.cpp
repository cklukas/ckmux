// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/key_reference.hpp"

#include <string>
#include <string_view>

#include "client/commands.hpp"
#include "common/config.hpp"
#include "common/keymap.hpp"

namespace ckm::client {
namespace {

std::string plain_title(std::string_view title) {
    std::string result;
    result.reserve(title.size());
    for (std::size_t index = 0; index < title.size(); ++index) {
        if (title[index] == '&') continue;
        if (title.substr(index).starts_with("…")) {
            result += "...";
            index += std::string_view("…").size() - 1U;
            continue;
        }
        result += title[index];
    }
    return result;
}

std::string markdown_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        if (character == '|' || character == '`') result += '\\';
        result += character;
    }
    return result;
}

std::string roff_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        if (character == '\\') {
            result += "\\\\";
        } else if (character == '-') {
            result += "\\-";
        } else if (character == '"') {
            result += "\\(dq";
        } else {
            result += character;
        }
    }
    return result;
}

Keymap documented_defaults(Settings& settings) {
    Keymap keymap;
    keymap.set_default_chord(Action::SendPrefix, chord_spelling(settings.prefix));
    return keymap;
}

std::string markdown_appendix(const Keymap& keymap, const Settings& settings) {
    std::string result =
        "## Default key appendix\n\n"
        "This table is generated from the same registry that drives dispatch, the\n"
        "menus, footer, which-key popup, and in-application help. `Prefix` means `" +
        markdown_text(prefix_label(settings.prefix)) +
        "` by\n"
        "default. A `bind` or `unbind` line can change any row with an action name.\n\n"
        "| Default | Command | `bind` action |\n"
        "|---|---|---|\n";

    for (const KeyBinding& binding : keymap.bindings()) {
        const std::string reached =
            binding.chord.empty() ? std::string("Menu only")
                                  : binding_label(settings.prefix, binding);
        result += "| `" + markdown_text(reached) + "` | " +
                  markdown_text(plain_title(binding.title)) + " | ";
        if (binding.action.has_value())
            result += "`" + markdown_text(action_name(*binding.action)) + "`";
        else
            result += "—";
        result += " |\n";
    }
    return result;
}

std::string roff_appendix(const Keymap& keymap, const Settings& settings) {
    std::string result =
        ".SH DEFAULT KEYS\n"
        "This section is generated from the same registry that drives key dispatch,\n"
        "menus, the footer, the which-key popup, and interactive help.\n"
        "The default prefix is \\fB" +
        roff_text(prefix_label(settings.prefix)) +
        "\\fR. Commands marked \\fBMenu only\\fR have no\n"
        "default chord.\n";

    for (const KeyBinding& binding : keymap.bindings()) {
        const std::string reached =
            binding.chord.empty() ? std::string("Menu only")
                                  : binding_label(settings.prefix, binding);
        result += ".TP\n.B \"" + roff_text(reached) + "\"\n" +
                  roff_text(plain_title(binding.title));
        if (binding.action.has_value())
            result += " (configuration action \\fB" +
                      roff_text(action_name(*binding.action)) + "\\fR)";
        result += "\n";
    }
    return result;
}

}  // namespace

std::string render_default_key_appendix(KeyAppendixFormat format) {
    Settings settings;
    const Keymap keymap = documented_defaults(settings);
    if (format == KeyAppendixFormat::Markdown) return markdown_appendix(keymap, settings);
    return roff_appendix(keymap, settings);
}

}  // namespace ckm::client
