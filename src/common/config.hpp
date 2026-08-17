// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The ckmux configuration file (plans/08-configuration.md).
//
// Two rules from that plan shape everything here. ckmux must be excellent
// with an EMPTY config, so every key has a built-in default and a missing
// file is not an error — it is the normal case. And a bad key never aborts:
// it warns, naming the line, and the rest of the file is still read, because
// a typo in one setting is no reason to refuse to start.
//
// Only the keys ckmux actually honours today are listed. The plan's full
// table lands with config v1 in M3; this is its first key and its file
// format, written now because a settings dialog that forgets what it was
// told is not a setting.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// Everything ckmux reads from the file, with the built-in defaults that
// apply when it says nothing. A struct rather than a lookup, so a caller
// cannot ask for a key that has no default.
struct Settings {
    // [general] login-shell. True — a login shell — is the default because
    // it is what tmux does, what login(1) does, and what every terminal
    // window on the machine does; a reader's PATH usually depends on it.
    bool login_shell = true;

    // [terminal] sixel-max-megapixels. The largest picture a program running
    // in a terminal may draw. A picture too large for the window it is drawn
    // in is cut off at the window's edge, so this is not a bound anyone meets
    // by having a big screen: it keeps an absurd geometry away from the
    // decoder's allocator, and sits far above a full screen (4K at 2x scaling
    // is about 33 megapixels). The default matches ckVision's own
    // `max_image_pixels`, so an unconfigured ckmux and an unconfigured
    // ckVision behave alike.
    int sixel_max_megapixels = 64;

    friend bool operator==(const Settings&, const Settings&) = default;
};

struct LoadedSettings {
    Settings settings;
    // One entry per problem, each naming its line: "ckmux.conf:4: unknown
    // key 'shel' in [general]". The caller decides where these go; nothing
    // here writes to a stream of its own.
    std::vector<std::string> warnings;
};

// Reads `path`. A file that does not exist yields the defaults and no
// warnings — that is what an unconfigured ckmux is.
LoadedSettings load_settings(const std::filesystem::path& path);

// Writes one key, leaving the rest of the file exactly as it was: comments,
// blank lines, key order and every unrelated key survive, because the file
// is the reader's and a dialog is only a visitor in it (plans/08). A
// matching key is rewritten where it stands, keeping any trailing comment; a
// missing key is added to its section; a missing section is appended. The
// file and its directory are created if absent.
//
// Returns false if the file could not be written — a full disk, a read-only
// home — so a dialog can say so rather than silently discarding the change.
bool save_setting(const std::filesystem::path& path, const std::string& section, const std::string& key,
                  const std::string& value);

// The spelling save_setting expects for a bool, and the one load_settings
// accepts: plans/08 says `true`/`false` and nothing else, so that a config
// file reads the same way everywhere.
std::string bool_setting(bool value);

}  // namespace ckm
