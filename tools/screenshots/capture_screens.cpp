// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The screenshots in README.md and docs/: the real ckmux binary, run in a
// ckVision pseudo-terminal exactly as a reader runs it in theirs, driven by
// keystrokes, with its screen decoded by ckVision's terminal emulator and
// written as SVG by scene::render_svg_snapshot. Nothing here draws a picture
// of ckmux; every figure is what the program actually put on a screen.
//
// Usage: capture_screens <ckmux-binary> <output-directory> [scene ...]
//
// The programs inside the windows are real too — bash, vim, tail, the
// ckmux CLI — working on a small fixture project this tool writes into a
// private HOME, so a figure shows nothing of the machine that made it. The
// server and client use a private socket and configuration, and the server
// is ended before the tool exits.
//
// POSIX only: the fixture is a bash/vim workspace, and that is what the
// figures are meant to show.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "cvision/core/frame_view.hpp"
#include "cvision/core/image.hpp"
#include "cvision/scene/svg_snapshot.hpp"
#include "cvision/term/terminal_subsession.hpp"

namespace {

namespace fs = std::filesystem;
using steady = std::chrono::steady_clock;

constexpr ckv::Size kScreen{120, 40};
constexpr ckv::PixelSize kCell{9, 18};

// ---------------------------------------------------------------------------
// The fixture: a HOME with a prompt, an editor configuration and a project.

void write_file(const fs::path& path, std::string_view text, bool executable = false) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
    if (executable)
        fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);
}

constexpr std::string_view kBashrc = R"(# capture_screens fixture
PS1='\[\e[1;32m\]dev\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
unset PROMPT_COMMAND
export LESS=-R
cd ~/orbit
)";

constexpr std::string_view kVimrc = R"(set nocompatible
syntax on
set number
set laststatus=2
set ruler
set title titlestring=editor
set background=dark
highlight LineNr ctermfg=242
highlight StatusLine ctermfg=16 ctermbg=110 cterm=NONE
)";

constexpr std::string_view kBuildScript = R"(#!/bin/bash
# A build and test run, as a CMake project prints one.
printf '\e]2;build\a\e[H\e[2J'
B=$'\e[1m'; G=$'\e[32m'; C=$'\e[36m'; M=$'\e[35m'; R=$'\e[0m'
echo "${B}-- orbit 2.4.0: Release, 8 jobs${R}"
n=0
for f in orbit/kepler orbit/propagator io/tle_reader net/telemetry app/main; do
  n=$((n + 19))
  printf '[%3d%%] %sBuilding CXX object src/%s.o%s\n' "$n" "$G" "$f" "$R"
  if [ "$f" = "orbit/propagator" ]; then
    echo "${B}propagator.cpp:212:9: ${M}warning:${R}${B} unused 'drag'${R}"
  fi
done
echo "[100%] ${G}${B}Linking CXX executable orbit${R}"
echo "${C}\$ ctest${R}"
i=0
for t in kepler propagator tle_reader telemetry; do
  i=$((i + 1))
  printf '  %d/4 Test #%d: %-11s ...  %sPassed%s  0.0%d s\n' "$i" "$i" "$t" "$G" "$R" "$i"
done
echo "${G}${B}100% tests passed${R}, 0 failed out of 4"
)";

constexpr std::string_view kPlotScript = R"sh(#!/bin/bash
printf '\e]2;plot\a\e[H\e[2J'
echo "ground track density, 24 h  (Sixel, drawn by this script)"
cat rings.six
echo
)sh";

constexpr std::string_view kFollowScript = R"(#!/bin/bash
printf '\e]2;api logs\a'
exec tail -n 40 -f logs/api.log
)";

std::string api_log() {
    const char* const reset = "\x1b[0m";
    struct Line {
        const char* level;
        const char* color;
        const char* text;
    };
    const Line lines[] = {
        {"INFO ", "\x1b[32m", "listening on 0.0.0.0:8443 (tls)"},
        {"INFO ", "\x1b[32m", "GET  /v1/orbits            200   4 ms"},
        {"INFO ", "\x1b[32m", "GET  /v1/orbits/iss/pass   200  11 ms"},
        {"WARN ", "\x1b[33m", "tle cache stale (age 26h) - refreshing"},
        {"INFO ", "\x1b[32m", "tle refresh: 4211 objects"},
        {"INFO ", "\x1b[32m", "POST /v1/propagate         202   7 ms"},
        {"ERROR", "\x1b[31;1m", "ws telemetry: peer reset (client 10.0.4.17)"},
        {"INFO ", "\x1b[32m", "ws telemetry: reconnected"},
        {"INFO ", "\x1b[32m", "GET  /v1/orbits/hst        200   5 ms"},
        {"INFO ", "\x1b[32m", "GET  /healthz              200   0 ms"},
        {"INFO ", "\x1b[32m", "POST /v1/propagate         202   9 ms"},
        {"WARN ", "\x1b[33m", "slow query: passes(iss, 7d) 312 ms"},
        {"INFO ", "\x1b[32m", "GET  /v1/orbits            200   3 ms"},
    };
    std::string text;
    int second = 2;
    for (const Line& line : lines) {
        char stamp[32];
        std::snprintf(stamp, sizeof stamp, "09:14:%02d", second);
        second += 3;
        text += std::string("\x1b[2m") + stamp + reset + " " + line.color + line.level + reset +
                " " + line.text + "\n";
    }
    return text;
}

constexpr std::string_view kSource = R"(// Two-body propagation for the orbit service.
#include "orbit/propagator.hpp"

#include <cmath>

namespace orbit {

// Solves Kepler's equation M = E - e sin E by Newton iteration.
double eccentric_anomaly(double mean, double e) {
    double estimate = e < 0.8 ? mean : M_PI;
    for (int step = 0; step < 16; ++step) {
        const double f = estimate - e * std::sin(estimate) - mean;
        const double slope = 1.0 - e * std::cos(estimate);
        estimate -= f / slope;
        if (std::abs(f) < 1e-12) break;
    }
    return estimate;
}

State propagate(const Elements& el, Seconds dt) {
    const double n = std::sqrt(kMu / std::pow(el.a, 3));
    const double mean = el.mean_anomaly + n * dt.count();
    const double ecc = eccentric_anomaly(mean, el.e);
    return to_state(el, ecc);
}

}  // namespace orbit
)";

// A small picture for the graphics scene, as Sixel: concentric rings of
// colour, the kind of thing a plotting tool sends to a capable terminal.
std::string sixel_picture() {
    constexpr int width = 432;
    constexpr int height = 180;
    constexpr int colors = 16;
    std::string out = "\x1bPq\"1;1;" + std::to_string(width) + ";" + std::to_string(height);
    for (int c = 0; c < colors; ++c) {
        // A hue sweep in Sixel's percentage RGB.
        const double t = static_cast<double>(c) / colors;
        const int r = static_cast<int>(50 + 50 * std::max(0.0, 1.0 - std::abs(t * 3.0 - 0.0)));
        const int g = static_cast<int>(20 + 70 * std::max(0.0, 1.0 - std::abs(t * 3.0 - 1.5)));
        const int b = static_cast<int>(30 + 70 * std::max(0.0, 1.0 - std::abs(t * 3.0 - 3.0)));
        out += "#" + std::to_string(c) + ";2;" + std::to_string(std::min(r, 100)) + ";" +
               std::to_string(std::min(g, 100)) + ";" + std::to_string(std::min(b, 100));
    }
    const auto color_at = [&](int x, int y) {
        const double dx = (x - width / 2.0) / (width / 2.0);
        const double dy = (y - height / 2.0) / (height / 2.0);
        const double d = std::sqrt(dx * dx + dy * dy);
        return static_cast<int>(d * 24.0) % colors;
    };
    for (int band = 0; band < height; band += 6) {
        for (int c = 0; c < colors; ++c) {
            std::string row;
            bool any = false;
            for (int x = 0; x < width; ++x) {
                int bits = 0;
                for (int k = 0; k < 6 && band + k < height; ++k)
                    if (color_at(x, band + k) == c) bits |= 1 << k;
                any = any || bits != 0;
                row += static_cast<char>(63 + bits);
            }
            if (any) out += "#" + std::to_string(c) + row + "$";
        }
        out += "-";
    }
    out += "\x1b\\";
    return out;
}

// ---------------------------------------------------------------------------
// One ckmux client on a decoded screen.

struct Fixture {
    fs::path root;
    fs::path home;
    fs::path socket;
    fs::path binary;
    fs::path dark_config;
    fs::path light_config;
};

std::vector<std::pair<std::string, std::string>> environment(const Fixture& fixture,
                                                             const fs::path& config) {
    return {{"TERM", "xterm-256color"},
            {"HOME", fixture.home.string()},
            {"SHELL", "/bin/bash"},
            {"PATH", "/usr/bin:/bin:/usr/sbin:/sbin"},
            {"LANG", "en_US.UTF-8"},
            {"LC_ALL", "en_US.UTF-8"},
            {"CKMUX_SOCKET", fixture.socket.string()},
            {"CKMUX_CONFIG", config.string()},
            // macOS's bash otherwise opens every shell with a notice about zsh.
            {"BASH_SILENCE_DEPRECATION_WARNING", "1"}};
}

class Screen {
public:
    Screen(const Fixture& fixture, const fs::path& config, ckv::Size cells = kScreen) {
        auto spec = ckv::term::TerminalLaunchSpec::program(fixture.binary.string(), {});
        spec.working_directory = fixture.home.string();
        spec.environment = environment(fixture, config);
        spec.profile = ckv::term::embedded_xterm_sixel_profile();
        spec.profile.cells = cells;
        spec.profile.cell_pixels = kCell;
        spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
        ckv::term::TerminalSubsessionOptions options;
        options.max_output_bytes = 4u << 20u;
        options.max_parser_work_per_step = 1u << 20u;
        session_ = ckv::term::launch_terminal_subsession(std::move(spec), options);
    }

    bool running() const {
        return session_->process_id() > 0 &&
               session_->state() != ckv::core::TerminalSubsessionState::Failed &&
               session_->state() != ckv::core::TerminalSubsessionState::Exited;
    }

    void settle(int ms) {
        const steady::time_point until = steady::now() + std::chrono::milliseconds(ms);
        while (steady::now() < until) {
            (void)session_->drain(1u << 20u);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void keys(std::string_view bytes, int settle_ms = 350) {
        session_->send_input(bytes);
        settle(settle_ms);
    }

    // A command line typed into whichever shell has the focus.
    void type(std::string_view line, int settle_ms = 600) {
        keys(std::string(line) + "\r", settle_ms);
    }

    std::string text() {
        std::string out;
        for (const ckv::Cell& cell : session_->snapshot().cell_buffer)
            if (!cell.is_continuation()) out += cell.grapheme();
        return out;
    }

    bool sees(std::string_view needle, int budget_ms = 8000) {
        const steady::time_point deadline = steady::now() + std::chrono::milliseconds(budget_ms);
        for (;;) {
            (void)session_->drain(1u << 20u);
            if (text().find(needle) != std::string::npos) return true;
            if (steady::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Whether a Sixel picture has reached the decoded screen. Graphics are
    // paced separately from text, so the caption can arrive well before it.
    bool sees_picture(int budget_ms = 8000) {
        const steady::time_point deadline = steady::now() + std::chrono::milliseconds(budget_ms);
        for (;;) {
            (void)session_->drain(1u << 20u);
            if (!session_->snapshot().rasters.empty()) return true;
            if (steady::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // The decoded screen, with any Sixel pictures composed into one raster
    // plane at the cell metric the client was told its host has.
    std::string svg() const {
        const ckv::core::TerminalSnapshot shot = session_->snapshot();
        const ckv::FrameView frame(shot.cell_buffer.data(), shot.cells);
        ckv::scene::SvgSnapshotOptions options;
        options.cell_pixels = kCell;
        if (shot.rasters.empty()) return ckv::scene::render_svg_snapshot(frame, options);
        ckv::Image plane(ckv::PixelSize{shot.cells.width * kCell.width,
                                        shot.cells.height * kCell.height});
        for (const ckv::core::TerminalRaster& raster : shot.rasters) {
            if (raster.image == nullptr) continue;
            const ckv::Image& image = *raster.image;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const int col = x / kCell.width;
                    const int row = y / kCell.height;
                    if (raster.live_cells != nullptr &&
                        (*raster.live_cells)[static_cast<std::size_t>(
                            row * raster.cell_extent.width + col)] == 0)
                        continue;
                    const int px = raster.anchor.x * kCell.width + x;
                    const int py = raster.anchor.y * kCell.height + y;
                    if (px < 0 || py < 0 || px >= plane.width() || py >= plane.height()) continue;
                    const ckv::Image::Rgba pixel = image.pixel(x, y);
                    if (pixel.a != 0) plane.set_pixel(px, py, pixel);
                }
            }
        }
        return ckv::scene::render_svg_snapshot(frame, options, &plane);
    }

    void quit() { session_->close(); }

private:
    std::unique_ptr<ckv::term::TerminalSubsession> session_;
};

// A ckmux CLI command, run to completion on its own pseudo-terminal.
std::string run_cli(const Fixture& fixture, std::vector<std::string> arguments) {
    auto spec = ckv::term::TerminalLaunchSpec::program(fixture.binary.string(), std::move(arguments));
    spec.working_directory = fixture.home.string();
    spec.environment = environment(fixture, fixture.dark_config);
    spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
    auto session = ckv::term::launch_terminal_subsession(std::move(spec));
    const steady::time_point deadline = steady::now() + std::chrono::seconds(10);
    while (session->state() != ckv::core::TerminalSubsessionState::Exited &&
           session->state() != ckv::core::TerminalSubsessionState::Failed &&
           steady::now() < deadline) {
        (void)session->drain(1u << 16u);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::string text;
    for (const ckv::Cell& cell : session->snapshot().cell_buffer)
        if (!cell.is_continuation()) text += cell.grapheme();
    return text;
}

// ---------------------------------------------------------------------------
// The scenes. Each one leaves the session as the next one expects it.

constexpr std::string_view kPrefix = "\x02";
constexpr std::string_view kEscape = "\x1b";
constexpr std::string_view kUp = "\x1b[A";

struct Capture {
    fs::path out_dir;
    std::vector<std::string> only;
    int written = 0;
    int failures = 0;

    bool wanted(std::string_view name) const {
        return only.empty() || std::find(only.begin(), only.end(), name) != only.end();
    }

    void save(Screen& screen, std::string_view name) {
        if (!wanted(name)) return;
        const fs::path path = out_dir / (std::string(name) + ".svg");
        std::ofstream(path, std::ios::binary) << screen.svg();
        ++written;
        std::fprintf(stderr, "wrote %s\n", path.c_str());
    }

    bool expect(bool ok, std::string_view what) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "capture_screens: %.*s did not appear\n",
                         static_cast<int>(what.size()), what.data());
        }
        return ok;
    }
};

void capture_all(const Fixture& fixture, Capture& capture) {
    Screen screen(fixture, fixture.dark_config);
    if (!capture.expect(screen.running(), "the ckmux client")) return;
    capture.expect(screen.sees("orbit$"), "the first shell prompt");
    screen.settle(300);
    capture.save(screen, "first-start");

    // Name the session. The attached client makes it rather than
    // `ckmux new -s`, whose first terminal is never offered Sixel.
    screen.keys(std::string(kPrefix) + "R", 700);
    screen.keys("orbit\r", 700);

    // Three windows, tiled, then a program in each: a build, an editor and
    // a log tail.
    screen.keys(std::string(kPrefix) + "c", 800);
    screen.keys(std::string(kPrefix) + "c", 800);
    capture.expect(screen.sees("Terminal 3"), "a third terminal");
    screen.keys(std::string(kPrefix) + "g", 900);
    screen.keys(std::string(kPrefix) + "1", 400);
    screen.type("./build.sh", 800);
    capture.expect(screen.sees("tests passed"), "the build output");
    screen.keys(std::string(kPrefix) + "3", 400);
    screen.type("./follow-logs.sh", 1000);
    capture.expect(screen.sees("slow query"), "the log tail");
    screen.keys(std::string(kPrefix) + "2", 400);
    screen.type("vim src/propagator.cpp", 1500);
    screen.keys("9G", 600);
    capture.save(screen, "desktop");

    // The prefix popup: everything ^B can do next, in the place you look.
    screen.keys(kPrefix, 700);
    capture.save(screen, "prefix-popup");
    screen.keys(kEscape, 500);

    // The menu bar, open on the Window menu by its mnemonic.
    screen.keys(std::string(kPrefix) + "m", 500);
    screen.keys("w", 700);
    capture.save(screen, "menu-window");
    screen.keys(kEscape, 200);
    screen.keys(kEscape, 400);

    // Zoom the editor to the whole desktop.
    screen.keys(std::string(kPrefix) + "z", 800);
    capture.save(screen, "zoom-editor");
    screen.keys(std::string(kPrefix) + "z", 800);

    // Copy mode in the build window: a line selection over the test results.
    screen.keys(std::string(kPrefix) + "1", 400);
    screen.keys(std::string(kPrefix) + "[", 600);
    for (int i = 0; i < 2; ++i) screen.keys(kUp, 120);
    screen.keys("V", 200);
    for (int i = 0; i < 4; ++i) screen.keys(kUp, 120);
    screen.settle(400);
    capture.save(screen, "copy-mode");
    screen.keys(kEscape, 600);

    // The key reference.
    screen.keys(std::string(kPrefix) + "?", 900);
    capture.save(screen, "key-reference");
    screen.keys(kEscape, 600);

    // Graphics: a Sixel picture drawn by a program in a window.
    screen.type("./plot.sh", 1500);
    capture.expect(screen.sees("ground track"), "the plot caption");
    capture.expect(screen.sees_picture(), "the Sixel picture");
    screen.settle(500);
    capture.save(screen, "sixel");
    // Back to the build, so the later figures do not each carry the picture.
    screen.type("./build.sh", 800);
    capture.expect(screen.sees("tests passed"), "the build output again");

    // More sessions, made from the command line, and the picker.
    (void)run_cli(fixture, {"new", "-s", "docs"});
    (void)run_cli(fixture, {"new", "-s", "ops", "./follow-logs.sh"});
    screen.keys(std::string(kPrefix) + "s", 1000);
    capture.expect(screen.sees("docs"), "the session picker");
    capture.save(screen, "sessions");
    screen.keys(kEscape, 600);

    // Leave it all running and come back with another theme: the programs
    // never noticed the client was gone.
    screen.keys(std::string(kPrefix) + "q", 800);
    screen.settle(500);
    screen.quit();
    Screen light(fixture, fixture.light_config);
    capture.expect(light.sees("Sessions", 10000), "the session picker on return");
    light.settle(600);
    capture.save(light, "picker-light");
    light.keys("\r", 1500);
    capture.expect(light.sees("api logs", 10000), "the reattached desktop");
    light.settle(1000);
    capture.save(light, "reattach-light");
    light.keys(std::string(kPrefix) + "q", 500);
    light.quit();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <ckmux-binary> <output-directory> [scene ...]\n", argv[0]);
        return 2;
    }
    Fixture fixture;
    fixture.binary = fs::absolute(argv[1]);
    Capture capture;
    capture.out_dir = argv[2];
    for (int i = 3; i < argc; ++i) capture.only.emplace_back(argv[i]);
    fs::create_directories(capture.out_dir);

    // A private root under TMPDIR, short enough for a Unix socket path.
    const char* const tmp = std::getenv("TMPDIR");
    std::string pattern = (fs::path(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") /
                           "ckmux-shots-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
        std::perror("mkdtemp");
        return 1;
    }
    fixture.root = pattern;
    fixture.home = fixture.root / "home";
    fixture.socket = fixture.root / "s.sock";
    const fs::path project = fixture.home / "orbit";
    write_file(fixture.home / ".bashrc", kBashrc);
    write_file(fixture.home / ".bash_profile", "source ~/.bashrc\n");
    write_file(fixture.home / ".vimrc", kVimrc);
    write_file(project / "build.sh", kBuildScript, true);
    write_file(project / "follow-logs.sh", kFollowScript, true);
    write_file(project / "plot.sh", kPlotScript, true);
    write_file(project / "logs" / "api.log", api_log());
    write_file(project / "src" / "propagator.cpp", kSource);
    write_file(project / "rings.six", sixel_picture());

    const std::string common =
        "[general]\nshell = /bin/bash\nlogin-shell = false\nclock = off\nshow-cpu = true\n";
    fixture.dark_config = fixture.root / "dark.conf";
    fixture.light_config = fixture.root / "light.conf";
    write_file(fixture.dark_config, common + "theme = dark\n");
    write_file(fixture.light_config, common + "theme = light\n");

    capture_all(fixture, capture);
    (void)run_cli(fixture, {"kill-server"});

    std::error_code ignored;
    fs::remove_all(fixture.root, ignored);
    std::fprintf(stderr, "capture_screens: %d figure(s), %d failure(s)\n", capture.written,
                 capture.failures);
    return capture.failures == 0 ? 0 : 1;
}
