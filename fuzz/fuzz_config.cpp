// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The configuration file (the testing plan §6, the configuration spec). The file is the reader's,
// which means it is arbitrary bytes: half-written, pasted from a blog, saved
// by an editor that added a BOM, or hostile. The configuration spec states what must happen
// to any of it — a bad key warns and the rest of the file is still read, a
// missing file is not an error, and nothing aborts — so this driver holds the
// parser to exactly that, over every byte string libFuzzer can invent.
//
// `load_settings` takes a path rather than a buffer, so each input is written
// to one scratch file per process and read back. That is the real entry point;
// fuzzing a private helper instead would leave the file handling — the part
// that meets the filesystem — untested.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "common/config.hpp"
#include "fuzz_common.hpp"

namespace {

// One file per process, so parallel fuzzer workers cannot read each other's
// input. Built once: a failure here is an unusable environment, not a finding.
const std::filesystem::path& scratch_path() {
    static const std::filesystem::path path = [] {
        std::filesystem::path candidate = std::filesystem::temp_directory_path();
        candidate /= "ckmux_fuzz_config_" + std::to_string(static_cast<long>(::getpid())) + ".conf";
        return candidate;
    }();
    return path;
}

void require_settings_are_sayable(const ckm::Settings& settings) {
    // Every number the file can carry comes back inside the range the configuration spec
    // documents, whatever the file said. A parser that lets an out-of-range
    // value through hands it to code that was written against the range —
    // a scrollback of four billion lines, a max-fps of zero.
    ckm::fuzz::require(settings.scrollback >= ckm::kScrollbackMin &&
                       settings.scrollback <= ckm::kScrollbackMax);
    ckm::fuzz::require(settings.max_fps >= ckm::kMaxFpsMin && settings.max_fps <= ckm::kMaxFpsMax);
    ckm::fuzz::require(settings.sixel_max_megapixels >= ckm::kSixelMegapixelsMin &&
                       settings.sixel_max_megapixels <= ckm::kSixelMegapixelsMax);
    ckm::fuzz::require(settings.kill_grace_seconds >= 0 && settings.kill_grace_seconds <= 600);
    ckm::fuzz::require(settings.printer_ask_cache_bytes <= ckm::kByteSizeMax);
    ckm::fuzz::require(settings.printer_spool_limit_bytes <= ckm::kByteSizeMax);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);

    {
        std::ofstream out(scratch_path(), std::ios::binary | std::ios::trunc);
        if (!out) return 0;
        out.write(input.data(), static_cast<std::streamsize>(input.size()));
    }

    ckm::LoadedSettings loaded;
    try {
        loaded = ckm::load_settings(scratch_path());
    } catch (...) {
        // The configuration spec is explicit that a bad file warns rather than failing, and
        // The engineering standard is explicit that exceptions never cross a boundary. A
        // throw out of here is therefore a finding, not an outcome.
        ckm::fuzz::invariant_failure();
    }

    require_settings_are_sayable(loaded.settings);
    for (const std::string& warning : loaded.warnings) {
        // A warning a reader cannot act on is noise. The configuration spec requires the
        // file and the line, in full, so this checks the half a fuzzer can:
        // the path is there, and the text is not empty.
        ckm::fuzz::require(!warning.empty());
        ckm::fuzz::require(warning.find(scratch_path().string()) != std::string::npos);
    }

    std::error_code ignored;
    std::filesystem::remove(scratch_path(), ignored);

    // The value grammars, driven directly on the same bytes. They are public
    // because the dialogs and `check-config` share them with the file parser
    // (config.hpp), so they are reachable with input the file never shaped —
    // and a range that only the file path enforces would be a range the
    // dialogs do not have.
    if (const std::optional<std::size_t> bytes = ckm::parse_byte_size(input))
        ckm::fuzz::require(*bytes <= ckm::kByteSizeMax);
    if (const std::optional<int> megapixels = ckm::parse_sixel_megapixels(input))
        ckm::fuzz::require(*megapixels >= ckm::kSixelMegapixelsMin &&
                           *megapixels <= ckm::kSixelMegapixelsMax);
    if (const std::optional<int> value = ckm::parse_int_in_range(input, ckm::kScrollbackMin,
                                                                ckm::kScrollbackMax))
        ckm::fuzz::require(*value >= ckm::kScrollbackMin && *value <= ckm::kScrollbackMax);
    (void)ckm::parse_bool(input);
    if (const std::optional<std::vector<ckm::ClipboardTarget>> targets =
            ckm::parse_clipboard_targets(input)) {
        for (const ckm::ClipboardTarget& target : *targets)
            // `exec:` with nothing after it is a copy target that runs
            // nothing; accepting it would put a silent no-op in the chain.
            if (target.kind == ckm::ClipboardTarget::Kind::Exec)
                ckm::fuzz::require(!target.command.empty());
    }
    return 0;
}
