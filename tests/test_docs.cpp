// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-24: the installed man page and browser key appendix are built from the
// same Keymap that dispatches the keys. A generated file merely existing is
// weak evidence; these cases prove every registry row reached both formats
// and that the checked-in Pages artifact is exactly the build's output.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "client/commands.hpp"
#include "client/key_reference.hpp"
#include "common/config.hpp"
#include "common/keymap.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string generated_man() {
#if defined(CKMUX_GENERATED_MAN_PATH)
    return read_file(CKMUX_GENERATED_MAN_PATH);
#else
    return {};
#endif
}

std::string generated_keys() {
#if defined(CKMUX_GENERATED_KEYS_PATH)
    return read_file(CKMUX_GENERATED_KEYS_PATH);
#else
    return {};
#endif
}

std::string checked_in_keys() {
#if defined(CKMUX_SOURCE_KEYS_PATH)
    return read_file(CKMUX_SOURCE_KEYS_PATH);
#else
    return {};
#endif
}

std::string roff_hyphens(std::string_view text) {
    std::string result;
    for (const char character : text)
        result += character == '-' ? std::string("\\-") : std::string(1, character);
    return result;
}

}  // namespace

CK_TEST(the_man_page_has_the_sections_a_command_reference_owes) {
    const std::string man = generated_man();
    CK_CHECK(!man.empty());
    for (const char* section : {".TH CKMUX 1", ".SH NAME", ".SH SYNOPSIS",
                                ".SH DESCRIPTION", ".SH COMMANDS", ".SH ATTACH OPTIONS",
                                ".SH DEFAULT KEYS", ".SH CONFIGURATION", ".SH ENVIRONMENT",
                                ".SH FILES", ".SH EXIT STATUS", ".SH SECURITY", ".SH SEE ALSO"})
        CK_CHECK(man.find(section) != std::string::npos);
    for (const char* command : {"ls", "new", "attach", "kill\\-session", "check\\-config",
                                "kill\\-server", "\\-\\-share", "\\-\\-watch",
                                "\\-\\-adopt\\-size"})
        CK_CHECK(man.find(command) != std::string::npos);
}

CK_TEST(every_default_registry_row_reaches_both_generated_appendices) {
    const ckm::Settings defaults;
    ckm::client::Keymap keymap;
    keymap.set_default_chord(ckm::Action::SendPrefix, ckm::chord_spelling(defaults.prefix));

    const std::string markdown =
        ckm::client::render_default_key_appendix(ckm::client::KeyAppendixFormat::Markdown);
    const std::string roff =
        ckm::client::render_default_key_appendix(ckm::client::KeyAppendixFormat::Roff);
    for (const ckm::client::KeyBinding& binding : keymap.bindings()) {
        const std::string reached =
            binding.chord.empty() ? std::string("Menu only")
                                  : ckm::client::binding_label(defaults.prefix, binding);
        CK_CHECK(markdown.find(reached) != std::string::npos);
        CK_CHECK(roff.find(roff_hyphens(reached)) != std::string::npos);
        if (binding.action.has_value()) {
            const std::string action(ckm::action_name(*binding.action));
            CK_CHECK(!action.empty());
            CK_CHECK(markdown.find(action) != std::string::npos);
            // Roff escapes the hyphens in action names.
            CK_CHECK(roff.find(roff_hyphens(action)) != std::string::npos);
        }
    }
}

CK_TEST(the_pages_key_reference_is_the_build_generated_artifact) {
    const std::string built = generated_keys();
    const std::string source = checked_in_keys();
    CK_CHECK(!built.empty());
    CK_CHECK(source == built);
    CK_CHECK(built.find("generated from the same registry") != std::string::npos);
    CK_CHECK(built.find("## Copy mode and paste") != std::string::npos);
}
