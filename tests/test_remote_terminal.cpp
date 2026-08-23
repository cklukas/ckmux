// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WP-5: a terminal in another process, driven by the M1 user interface.
//
// The tests here are deliberately end-to-end within the client: a real
// `ckv::ui::Application`, real `TerminalView`s, the real `ClientApp` chrome, and
// on the other side a real server-side `DiffEngine` reading a real emulator. The
// only thing missing is the socket, and the reason to leave it out is that WP-2
// already tests sockets: what is on trial here is whether the UI can drive a
// terminal it does not own.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "client/client_app.hpp"
#include "client/mirror.hpp"
#include "client/remote_terminal.hpp"
#include "common/proto.hpp"
#include "server/diff_engine.hpp"

#include "cvision/term/headless_terminal.hpp"
#include "cvision/term/virtual_display.hpp"
#include "cvision/term/terminal_emulator.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/ui/application.hpp"
#include "cvision/widgets/terminal_view.hpp"

namespace {

using ckm::client::RemoteTerminalSubsession;
using ckm::client::TerminalMirror;

// A server, minus the socket: an emulator with a child's output in it and the
// diff engine that turns what it holds into messages. Both are the real ones.
struct FakeServer {
    ckv::term::TerminalCapabilityProfile profile = [] {
        ckv::term::TerminalCapabilityProfile value = ckv::term::embedded_xterm_sixel_profile();
        value.cells = ckv::Size{40, 8};
        value.cell_pixels = ckv::Size{9, 18};
        value.osc_policy = ckv::core::TerminalOscPolicy::StoreMetadata;
        // What a real server's terminals are opened with when the reader's
        // `[terminal] osc52` is on and the printer is not off (server/
        // terminals.cpp): the child may put text on the clipboard and may
        // capture print output, and the server decides what to do with both.
        value.clipboard_policy = ckv::core::TerminalClipboardPolicy::AllowWrite;
        value.printer_policy = ckv::core::TerminalPrinterPolicy::Capture;
        return value;
    }();
    ckv::term::TerminalSubsessionOptions options = [] {
        ckv::term::TerminalSubsessionOptions value;
        value.max_scrollback_lines = 200;
        value.max_output_bytes = 1u << 20u;
        value.max_parser_work_per_step = 128u << 10u;
        return value;
    }();
    ckv::term::TerminalEmulator terminal{profile, options};
    ckm::server::DiffEngine engine;
    std::uint64_t id = 7;

    ckm::proto::TerminalState snapshot() {
        ckm::proto::TerminalState state = engine.snapshot(id, terminal);
        // The history is a second call, exactly as it is on the real path: how
        // much of it fits is not known until every terminal's screen has been
        // measured, so the screen goes first and the histories share what is
        // left (R1's snapshot budget). One terminal here, so it gets all of it.
        (void)engine.fill_history(id, state, terminal, ckm::proto::kSnapshotPayloadBudget);
        terminal.clear_damage();
        return state;
    }

    // The same snapshot a server states for a terminal whose child has ended
    // and which it is keeping anyway. The exit fields are the SERVER's rather
    // than the differ's — it is the server that knows a terminal still in a
    // session with a dead child is a held one (server.cpp's `send_snapshot`) —
    // so they are filled here the same way, and the reattach cases below drive
    // the wire instead of a mirror's memory.
    ckm::proto::TerminalState snapshot_of_a_held_terminal(int status) {
        ckm::proto::TerminalState state = snapshot();
        state.exited = 1;
        state.exit_status = status;
        state.hold = 1;
        return state;
    }

    // The clipboard message a server sends when the child asked for one: the
    // terminal it came from, and the text the emulator decoded and sanitized.
    ckm::proto::ClipboardSet clipboard_write() {
        ckm::proto::ClipboardSet clipboard;
        clipboard.term = id;
        clipboard.text = terminal.snapshot().clipboard_text;
        return clipboard;
    }

    // What the child printed, as the messages a client would receive.
    std::vector<ckm::proto::GridDelta> tick() {
        std::vector<ckm::proto::GridDelta> deltas;
        ckm::server::DiffEngine::TerminalTick flushed = engine.flush(id, terminal);
        if (flushed.delta.has_value()) deltas.push_back(std::move(*flushed.delta));
        return deltas;
    }

    void child_printed(std::string_view bytes) { terminal.feed_output(bytes); }
};

std::string mirror_text(const TerminalMirror& mirror) {
    std::string text;
    for (const ckv::Cell& cell : mirror.grid())
        if (!cell.is_continuation()) text += cell.grapheme();
    return text;
}

std::string surface_text(const ckv::scene::Surface& surface) {
    std::string text;
    for (int y = 0; y < surface.size().height; ++y)
        for (int x = 0; x < surface.size().width; ++x) {
            const ckv::Cell& cell = surface.at(ckv::Point{x, y});
            if (!cell.is_continuation()) text += cell.grapheme();
        }
    return text;
}

}  // namespace

CK_TEST(a_mirror_holds_what_the_server_holds_after_a_snapshot_and_its_deltas) {
    FakeServer server;
    // `\a` and not `\x07`: a hex escape eats as many hex digits as it can, so
    // "\x07first" is the single character 0x7F followed by "irst" — the BEL
    // disappears, the OSC never terminates, and it swallows the line instead.
    server.child_printed("\x1b]0;a caption\afirst line\r\nsecond line\r\n");

    TerminalMirror mirror;
    mirror.set_history_limit(200);
    mirror.adopt(server.snapshot());

    server.child_printed("third line\r\n");
    for (const ckm::proto::GridDelta& delta : server.tick()) CK_CHECK(mirror.apply(delta));

    // Cell for cell against the terminal the server is holding — the only check
    // that means anything here, because a mirror that merely looks plausible is a
    // mirror that will diverge on the next delta.
    const std::span<const ckv::Cell> theirs = server.terminal.cells();
    const std::span<const ckv::Cell> ours = mirror.grid();
    CK_CHECK(ours.size() == theirs.size());
    bool identical = ours.size() == theirs.size();
    for (std::size_t index = 0; identical && index < ours.size(); ++index)
        if (ours[index].grapheme() != theirs[index].grapheme() ||
            !(ours[index].style() == theirs[index].style()))
            identical = false;
    CK_CHECK(identical);
    CK_CHECK(mirror.title() == "a caption");
    CK_CHECK(!mirror.needs_snapshot());
    CK_CHECK(mirror.gaps() == 0U);
}

CK_TEST(a_delta_that_skips_a_number_is_refused_and_asks_for_a_snapshot) {
    // The gap rule (the protocol spec). A client that guessed would show a screen no
    // program ever drew; a client that resnapshots loses nothing but a round
    // trip. What this pins is that the mirror does not quietly apply it.
    FakeServer server;
    server.child_printed("before\r\n");
    TerminalMirror mirror;
    mirror.adopt(server.snapshot());

    server.child_printed("after\r\n");
    std::vector<ckm::proto::GridDelta> deltas = server.tick();
    CK_CHECK(deltas.size() == 1U);
    if (deltas.empty()) return;

    ckm::proto::GridDelta lost = deltas.front();
    lost.seq += 1;  // as though the delta before it never arrived
    CK_CHECK(!mirror.apply(lost));
    CK_CHECK(mirror.needs_snapshot());
    CK_CHECK(mirror.gaps() == 1U);
    // And the mirror did not take half of it.
    CK_CHECK(mirror_text(mirror).find("after") == std::string::npos);
}

CK_TEST(a_view_over_a_remote_terminal_shows_what_the_child_printed) {
    // `TerminalView`, unmodified, over mirror state. This is the claim the whole
    // package rests on: the widget cannot tell the difference, because everything
    // it asks is behind the seam.
    FakeServer server;
    std::vector<ckm::proto::Message> sent;
    RemoteTerminalSubsession remote(server.id, server.profile,
                                    [&sent](const ckm::proto::Message& message) {
                                        sent.push_back(message);
                                    });
    remote.mirror().set_history_limit(200);

    server.child_printed("hello from the other side\r\n");
    remote.mirror().adopt(server.snapshot());

    ckv::ui::RoleRegistry registry;
    ckv::ui::StandardRoles roles = ckv::ui::intern_standard_roles(registry);
    ckv::ui::Theme theme = ckv::ui::make_classic_theme(registry, roles);
    ckv::widgets::TerminalView view(remote);
    view.set_context(ckv::ui::Context{&theme, &registry, nullptr});
    view.set_bounds(ckv::Rect{0, 0, 40, 8});

    ckv::scene::Surface surface(ckv::Size{40, 8}, ckv::Cell{});
    ckv::scene::Painter painter(surface, ckv::Rect{0, 0, 40, 8});
    view.draw(painter);
    CK_CHECK(surface_text(surface).find("hello from the other side") != std::string::npos);

    // Typing goes out as opaque bytes, encoded here against mirrored mode state
    // — which is the other half of the acceptance criterion.
    //
    // The view has already sent something before this: giving it bounds tells
    // the session how big it is, which for a remote terminal is a `ClientResize`
    // on its way to the server. That is the right behaviour and worth noticing —
    // a window resize propagates with no extra code — so what is checked is the
    // message typing produced, not the only message there has ever been.
    sent.clear();
    CK_CHECK(view.on_text(ckv::TextEvent{"ls -l"}));
    CK_CHECK(sent.size() == 1U);
    if (!sent.empty()) {
        const auto* input = std::get_if<ckm::proto::Input>(&sent.front());
        CK_CHECK(input != nullptr);
        if (input != nullptr) {
            CK_CHECK(input->term == server.id);
            CK_CHECK(input->bytes == "ls -l");
        }
    }

    // And the view never asked for the whole terminal: on a mirror a snapshot
    // copies the grid and the history, and this is the read that happens on every
    // frame and every keystroke (ckVision L-53).
    CK_CHECK(remote.stray_output_calls() == 0U);
}

CK_TEST(two_simultaneous_placed_pictures_on_one_mirror_get_distinct_scene_ids) {
    // The client-side twin of ckVision's own terminal_emulator fix: a mirror
    // rebuilding TerminalRaster entries from ImagePlace messages carried the
    // identical bug the local emulator did, since every raster it built took
    // this terminal's bare identity verbatim, unchanged from picture to
    // picture. A child that places a second picture before the server (or a
    // test) removes the first is ordinary, and it used to crash the instant
    // both reached the same Surface — Surface::add_raster_region's own
    // uniqueness contract, violated by two regions sharing one id.
    FakeServer server;
    RemoteTerminalSubsession remote(server.id, server.profile,
                                    [](const ckm::proto::Message&) {});
    remote.mirror().set_history_limit(200);
    remote.mirror().adopt(server.snapshot());
    remote.mirror().set_raster_identity(99);

    auto image = std::make_shared<ckv::Image>(4, 6);
    remote.mirror().place_image(1, image, ckv::Point{0, 0}, ckv::Size{1, 1});
    remote.mirror().place_image(2, image, ckv::Point{4, 0}, ckv::Size{1, 1});
    CK_CHECK(remote.mirror().rasters().size() == 2U);
    CK_CHECK(remote.mirror().rasters()[0].id == 99);
    CK_CHECK(remote.mirror().rasters()[1].id == 100);

    ckv::ui::RoleRegistry registry;
    ckv::ui::StandardRoles roles = ckv::ui::intern_standard_roles(registry);
    ckv::ui::Theme theme = ckv::ui::make_classic_theme(registry, roles);
    ckv::widgets::TerminalView view(remote);
    view.set_context(ckv::ui::Context{&theme, &registry, nullptr});
    view.set_bounds(ckv::Rect{0, 0, 40, 8});

    ckv::scene::Surface surface(ckv::Size{40, 8}, ckv::Cell{});
    ckv::scene::Painter painter(surface, ckv::Rect{0, 0, 40, 8});
    view.draw(painter);  // used to abort here
    CK_CHECK(surface.raster_regions().size() == 2U);
}

CK_TEST(a_heal_keeps_the_pictures_a_mirror_holds_until_they_are_restated) {
    // A snapshot names the picture ids its watchers hold (TerminalState::
    // images), and the mirror keeps exactly those across adopt: their
    // restatement arrives under the same stable ids, megabytes behind the
    // grid, and clearing them first was the gray a reader stared at for most
    // of every heal under load (field report, 2026-08-19, ckgrapher). What
    // the snapshot does not name goes — its Remove was dropped with the rest
    // of the backlog the heal is healing.
    FakeServer server;
    server.child_printed("\x1b[2;3H\x1bPq#0;2;100;0;0!8~-!8~\x1b\\");
    const auto tick = server.engine.flush(server.id, server.terminal);
    const auto* begin = tick.images.empty()
                            ? nullptr
                            : std::get_if<ckm::proto::ImageAddBegin>(&tick.images.front());
    CK_CHECK(begin != nullptr);
    if (begin == nullptr) return;

    TerminalMirror mirror;
    mirror.set_history_limit(200);
    mirror.set_raster_identity(99);
    auto pixels = std::make_shared<ckv::Image>(8, 12);
    mirror.place_image(begin->id, pixels, ckv::Point{2, 1}, ckv::Size{1, 1});
    CK_CHECK(mirror.rasters().size() == 1U);

    // The heal: the snapshot lists the id, so the raster survives adoption
    // with its pixels intact.
    mirror.adopt(server.snapshot());
    CK_CHECK(mirror.rasters().size() == 1U);
    CK_CHECK(!mirror.rasters().empty() && mirror.rasters().front().image == pixels);

    // The picture goes away child-side; the next snapshot no longer names it,
    // and adoption drops it.
    server.child_printed("\x1b[2J");
    (void)server.engine.flush(server.id, server.terminal);
    mirror.adopt(server.snapshot());
    CK_CHECK(mirror.rasters().empty());
}

CK_TEST(the_child_dialect_the_view_encodes_is_the_one_the_server_reported) {
    // Keys are encoded client-side against mirrored mode state (the terminal-emulation spec), so the
    // mode bits have to survive the wire with their meaning intact. A cursor key
    // is the cheapest place to see it: DECCKM turns `ESC [ A` into `ESC O A`, and
    // getting that wrong means arrows do nothing in vim and everybody blames the
    // multiplexer.
    FakeServer server;
    std::vector<ckm::proto::Message> sent;
    RemoteTerminalSubsession remote(server.id, server.profile,
                                    [&sent](const ckm::proto::Message& message) {
                                        sent.push_back(message);
                                    });
    server.child_printed("ready");
    remote.mirror().adopt(server.snapshot());

    ckv::ui::RoleRegistry registry;
    ckv::ui::StandardRoles roles = ckv::ui::intern_standard_roles(registry);
    ckv::ui::Theme theme = ckv::ui::make_classic_theme(registry, roles);
    ckv::widgets::TerminalView view(remote);
    view.set_context(ckv::ui::Context{&theme, &registry, nullptr});
    view.set_bounds(ckv::Rect{0, 0, 40, 8});

    ckv::KeyEvent up;
    up.chord.key = ckv::Key::Up;
    CK_CHECK(view.on_key(up));
    CK_CHECK(!sent.empty());
    if (!sent.empty()) {
        const auto* input = std::get_if<ckm::proto::Input>(&sent.back());
        CK_CHECK(input != nullptr);
        if (input != nullptr) CK_CHECK(input->bytes == "\x1b[A");
    }

    // The child turns on application cursor keys; the server reports the mode;
    // the same keystroke now encodes the other way.
    sent.clear();
    server.child_printed("\x1b[?1h");
    for (const ckm::proto::GridDelta& delta : server.tick()) CK_CHECK(remote.mirror().apply(delta));
    CK_CHECK(remote.status().application_cursor_keys);
    CK_CHECK(view.on_key(up));
    CK_CHECK(!sent.empty());
    if (!sent.empty()) {
        const auto* input = std::get_if<ckm::proto::Input>(&sent.back());
        CK_CHECK(input != nullptr);
        if (input != nullptr) CK_CHECK(input->bytes == "\x1bOA");
    }
}

CK_TEST(a_click_the_child_asked_for_reaches_it_over_the_wire) {
    // A field report: a child that enabled mouse tracking (SGR modern) saw
    // zero mouse events after the wire carried a tracking LEVEL instead of a
    // bare bit (D-054) — this is the reattach-review's own regression class,
    // caught here by driving the exact chain a click takes: child enables
    // 1000+1006, the emulator's damage crosses the wire, the mirror rehydrates
    // it, and a real TerminalView's on_mouse must decide to forward it.
    FakeServer server;
    std::vector<ckm::proto::Message> sent;
    RemoteTerminalSubsession remote(server.id, server.profile,
                                    [&sent](const ckm::proto::Message& message) {
                                        sent.push_back(message);
                                    });
    server.child_printed("ready");
    remote.mirror().adopt(server.snapshot());

    ckv::ui::RoleRegistry registry;
    ckv::ui::StandardRoles roles = ckv::ui::intern_standard_roles(registry);
    ckv::ui::Theme theme = ckv::ui::make_classic_theme(registry, roles);
    ckv::widgets::TerminalView view(remote);
    view.set_context(ckv::ui::Context{&theme, &registry, nullptr});
    view.set_bounds(ckv::Rect{0, 0, 40, 8});

    // DEC 1000 (report presses) + DEC 1006 (SGR encoding) — the modern combo
    // the field report's outer host and the embedded emulator both named.
    server.child_printed("\x1b[?1000h\x1b[?1006h");
    for (const ckm::proto::GridDelta& delta : server.tick()) CK_CHECK(remote.mirror().apply(delta));
    CK_CHECK(remote.status().mouse_reporting_enabled);
    CK_CHECK(remote.status().mouse_tracking == ckv::core::TerminalMouseTracking::Buttons);

    ckv::MouseEvent click;
    click.action = ckv::MouseAction::Down;
    click.button = ckv::MouseButton::Left;
    click.cell = ckv::Point{3, 2};
    CK_CHECK(view.on_mouse(click));
    CK_CHECK(!sent.empty());
    if (!sent.empty()) {
        const auto* input = std::get_if<ckm::proto::Input>(&sent.back());
        CK_CHECK(input != nullptr);
        // SGR: `CSI < b ; x ; y M`, 1-based, button 0 = left-down.
        if (input != nullptr) CK_CHECK(input->bytes == "\x1b[<0;4;3M");
    }
}

CK_TEST(a_reader_pages_back_through_a_history_the_client_holds) {
    // Paging is local, which is why the client keeps the history at all
    // (the protocol spec): PgUp must not be a round trip, and after WP-6 it must work
    // while the server is unreachable.
    FakeServer server;
    std::vector<ckm::proto::Message> sent;
    RemoteTerminalSubsession remote(server.id, server.profile,
                                    [&sent](const ckm::proto::Message& message) {
                                        sent.push_back(message);
                                    });
    remote.mirror().set_history_limit(200);
    remote.mirror().adopt(server.snapshot());

    std::string burst;
    for (int line = 0; line < 30; ++line) burst += "line-" + std::to_string(line) + "\r\n";
    server.child_printed(burst);
    for (const ckm::proto::GridDelta& delta : server.tick()) CK_CHECK(remote.mirror().apply(delta));

    // The lines that scrolled away are in the client's own history, in order.
    const std::span<const ckv::Cell> history = remote.scrollback();
    CK_CHECK(!history.empty());
    std::string history_text;
    for (const ckv::Cell& cell : history)
        if (!cell.is_continuation()) history_text += cell.grapheme();
    CK_CHECK(history_text.find("line-0") != std::string::npos);
    CK_CHECK(history_text.find("line-5") != std::string::npos);

    ckv::ui::RoleRegistry registry;
    ckv::ui::StandardRoles roles = ckv::ui::intern_standard_roles(registry);
    ckv::ui::Theme theme = ckv::ui::make_classic_theme(registry, roles);
    ckv::widgets::TerminalView view(remote);
    view.set_context(ckv::ui::Context{&theme, &registry, nullptr});
    view.set_bounds(ckv::Rect{0, 0, 40, 8});
    ckv::scene::Surface surface(ckv::Size{40, 8}, ckv::Cell{});
    ckv::scene::Painter painter(surface, ckv::Rect{0, 0, 40, 8});

    ckv::KeyEvent page_up;
    page_up.chord.key = ckv::Key::PageUp;
    sent.clear();  // the view's own resize has already been sent; see above
    CK_CHECK(view.on_key(page_up));
    view.draw(painter);
    // Scrolled back: earlier lines on screen, and not one byte sent to the
    // server to make it happen.
    CK_CHECK(surface_text(surface).find("line-2") != std::string::npos);
    CK_CHECK(sent.empty());
}

CK_TEST(a_snapshot_carries_the_history_a_reader_had_before_they_attached) {
    // The reattach case, which is the one a reader notices: they come back to a
    // session that has been running for hours and page up. Nothing in the delta
    // stream can supply that — those lines scrolled away while nobody was
    // watching — so it has to arrive in the snapshot, and a client that took the
    // grid and left the history would look right until the moment somebody
    // scrolled.
    FakeServer server;
    std::string burst;
    for (int line = 0; line < 40; ++line) burst += "before-attach-" + std::to_string(line) + "\r\n";
    server.child_printed(burst);

    RemoteTerminalSubsession remote(server.id, server.profile, nullptr);
    remote.mirror().set_history_limit(200);
    // One snapshot and NOT ONE delta: everything checked below came out of it.
    remote.mirror().adopt(server.snapshot());

    const std::span<const ckv::Cell> history = remote.scrollback();
    CK_CHECK(!history.empty());
    std::string history_text;
    for (const ckv::Cell& cell : history)
        if (!cell.is_continuation()) history_text += cell.grapheme();
    CK_CHECK(history_text.find("before-attach-0") != std::string::npos);
    CK_CHECK(history_text.find("before-attach-20") != std::string::npos);
    // As many lines as the server had, in the same order.
    CK_CHECK(history.size() == server.terminal.scrollback().size());

    // And a reader can page into it straight away.
    ckv::ui::RoleRegistry registry;
    ckv::ui::StandardRoles roles = ckv::ui::intern_standard_roles(registry);
    ckv::ui::Theme theme = ckv::ui::make_classic_theme(registry, roles);
    ckv::widgets::TerminalView view(remote);
    view.set_context(ckv::ui::Context{&theme, &registry, nullptr});
    view.set_bounds(ckv::Rect{0, 0, 40, 8});
    ckv::KeyEvent page_up;
    page_up.chord.key = ckv::Key::PageUp;
    CK_CHECK(view.on_key(page_up));
    ckv::scene::Surface surface(ckv::Size{40, 8}, ckv::Cell{});
    ckv::scene::Painter painter(surface, ckv::Rect{0, 0, 40, 8});
    view.draw(painter);
    CK_CHECK(surface_text(surface).find("before-attach-2") != std::string::npos);
}

CK_TEST(a_reader_keeps_only_as_much_history_as_they_asked_for) {
    // The server's capacity is its own; the client's is the reader's
    // `[general] scrollback`. A client that kept everything the server sent
    // would hold text its reader had asked it to forget — and on a server
    // configured with a larger history than the client, silently use the memory
    // for it too.
    FakeServer server;
    std::string burst;
    for (int line = 0; line < 60; ++line) burst += "line-" + std::to_string(line) + "\r\n";
    server.child_printed(burst);

    RemoteTerminalSubsession remote(server.id, server.profile, nullptr);
    remote.mirror().set_history_limit(10);
    remote.mirror().adopt(server.snapshot());
    const std::size_t width = static_cast<std::size_t>(server.profile.cells.width);
    CK_CHECK(remote.scrollback().size() == 10U * width);
    // The newest ten, not the oldest: capacity drops what scrolled away first.
    std::string history_text;
    for (const ckv::Cell& cell : remote.scrollback())
        if (!cell.is_continuation()) history_text += cell.grapheme();
    CK_CHECK(history_text.find("line-0 ") == std::string::npos);
    CK_CHECK(history_text.find("line-4") != std::string::npos);
}

CK_TEST(a_resize_tells_the_server_the_size_of_this_terminals_view) {
    // THIS terminal's grid, not the client's desktop — and the distinction is
    // not pedantic. A terminal sized to the whole desktop, shown in a window
    // that is smaller by its frame, has only its bottom rows on view: a shell's
    // prompt is printed at the top, so the window looks empty while every layer
    // underneath works perfectly. That is what shipped, and what a reader
    // reported as "no output is visible in the terminal".
    //
    // The desktop and its pixel metric travel with `Attach` and `ClientResize`,
    // once per client; a terminal's own size travels per terminal.
    FakeServer server;
    std::vector<ckm::proto::Message> sent;
    RemoteTerminalSubsession remote(server.id, server.profile,
                                    [&sent](const ckm::proto::Message& message) {
                                        sent.push_back(message);
                                    });
    remote.mirror().adopt(server.snapshot());

    remote.resize(ckv::Size{100, 30}, ckv::Size{9, 18});
    CK_CHECK(sent.size() == 1U);
    if (!sent.empty()) {
        const auto* resize = std::get_if<ckm::proto::MoveResize>(&sent.front());
        CK_CHECK(resize != nullptr);
        if (resize != nullptr) {
            CK_CHECK(resize->term == server.id);
            CK_CHECK(resize->rect.width == 100U);
            CK_CHECK(resize->rect.height == 30U);
        }
    }
    // And the client's own cell metric is kept, because that is what a picture
    // is drawn at.
    CK_CHECK(remote.profile().cell_pixels.width == 9);
    CK_CHECK(remote.profile().cell_pixels.height == 18);
}

CK_TEST(an_exited_child_is_an_exited_terminal_to_the_view) {
    FakeServer server;
    RemoteTerminalSubsession remote(server.id, server.profile, nullptr);
    remote.mirror().adopt(server.snapshot());
    CK_CHECK(remote.state() == ckv::core::TerminalSubsessionState::Running ||
             remote.state() == ckv::core::TerminalSubsessionState::Ready);

    ckm::proto::TermClosed closed;
    closed.term = server.id;
    closed.exited = 1;
    closed.exit_status = 3;
    remote.mirror().apply(closed);
    CK_CHECK(remote.state() == ckv::core::TerminalSubsessionState::Exited);
    CK_CHECK(remote.status().exit_code.has_value());
    if (remote.status().exit_code.has_value()) CK_CHECK(*remote.status().exit_code == 3);
}

CK_TEST(a_snapshot_does_not_bring_an_exited_child_back_to_life) {
    // Reattaching hands a client every terminal whole. Before the snapshot
    // carried the child's fate, a mirror that let `adopt` clear what a
    // `TermClosed` had already told it showed a dead shell as a live terminal
    // after every reattach: no banner, no status, and a window a reader could
    // type into with nothing on the other end (M-R4).
    FakeServer server;
    server.child_printed("done\r\n");
    RemoteTerminalSubsession remote(server.id, server.profile, nullptr);
    remote.mirror().adopt(server.snapshot());

    ckm::proto::TermClosed closed;
    closed.term = server.id;
    closed.exited = 1;
    closed.exit_status = 3;
    closed.hold = 1;  // the window stays, banner and all (the session model on-exit)
    remote.mirror().apply(closed);
    CK_CHECK(remote.state() == ckv::core::TerminalSubsessionState::Exited);

    // The reattach: the same terminal, stated whole, exactly as a server states
    // it to a client that has come back — the exit included, which is what
    // makes the wire rather than the mirror's memory the thing being trusted.
    remote.mirror().adopt(server.snapshot_of_a_held_terminal(3));
    CK_CHECK(remote.state() == ckv::core::TerminalSubsessionState::Exited);
    CK_CHECK(remote.mirror().exited());
    CK_CHECK(remote.mirror().held());
    CK_CHECK(remote.status().exit_code.has_value());
    if (remote.status().exit_code.has_value()) CK_CHECK(*remote.status().exit_code == 3);
    // And the screen still came from the snapshot rather than from before it.
    CK_CHECK(mirror_text(remote.mirror()).find("done") != std::string::npos);

    // A client that was never told at all — one that attached AFTER the child
    // ended, which is the ordinary reattach — learns it from the same snapshot.
    // Nothing but the wire has spoken to this mirror.
    RemoteTerminalSubsession arriving(server.id, server.profile, nullptr);
    arriving.mirror().adopt(server.snapshot_of_a_held_terminal(3));
    CK_CHECK(arriving.state() == ckv::core::TerminalSubsessionState::Exited);
    CK_CHECK(arriving.mirror().held());
    CK_CHECK(arriving.status().exit_code.has_value());
    if (arriving.status().exit_code.has_value()) CK_CHECK(*arriving.status().exit_code == 3);

    // A snapshot that states no exit leaves what a `TermClosed` said, because
    // an exit is a one-way door and ids are never reused: the server has no way
    // to say "alive again" about a terminal by the same id, so silence must not
    // be read as one.
    remote.mirror().adopt(server.snapshot());
    CK_CHECK(remote.mirror().exited());

    // The one way back to alive is the server announcing a terminal, which is
    // sound because ids are never reused: a `TermOpened` really is a different
    // program behind the same mirror.
    remote.mirror().open(ckv::Size{40, 8});
    CK_CHECK(!remote.mirror().exited());
    CK_CHECK(!remote.mirror().held());
    CK_CHECK(!remote.status().exit_code.has_value());
    CK_CHECK(remote.state() != ckv::core::TerminalSubsessionState::Exited);
}

CK_TEST(the_keys_a_view_encodes_follow_the_kitty_flags_the_child_asked_for) {
    // M-R2, from the end that matters: the client encodes the keys, so a client
    // that was never told the child had switched the legacy fallback off sends
    // it the legacy encoding — and the server, meanwhile, answers the child's
    // re-probe with the enhancements ON. A bare Escape is the cheapest place to
    // see it: under the protocol it is `CSI 27 u`, and the whole reason a
    // program turns the protocol on is to tell that apart from the start of a
    // sequence.
    FakeServer server;
    std::vector<ckm::proto::Message> sent;
    RemoteTerminalSubsession remote(server.id, server.profile,
                                    [&sent](const ckm::proto::Message& message) {
                                        sent.push_back(message);
                                    });
    remote.mirror().adopt(server.snapshot());

    ckv::ui::RoleRegistry registry;
    ckv::ui::StandardRoles roles = ckv::ui::intern_standard_roles(registry);
    ckv::ui::Theme theme = ckv::ui::make_classic_theme(registry, roles);
    ckv::widgets::TerminalView view(remote);
    view.set_context(ckv::ui::Context{&theme, &registry, nullptr});
    view.set_bounds(ckv::Rect{0, 0, 40, 8});

    ckv::KeyEvent escape;
    escape.chord.key = ckv::Key::Escape;
    sent.clear();  // the view's own resize has already been sent
    CK_CHECK(view.on_key(escape));
    CK_CHECK(!sent.empty());
    if (!sent.empty()) {
        const auto* input = std::get_if<ckm::proto::Input>(&sent.back());
        CK_CHECK(input != nullptr);
        if (input != nullptr) CK_CHECK(input->bytes == "\x1b");
    }

    // The child pushes "disambiguate escape codes"; the server states the
    // change in the modes word; the same keystroke now encodes the other way.
    sent.clear();
    server.child_printed("\x1b[>1u");
    for (const ckm::proto::GridDelta& delta : server.tick()) CK_CHECK(remote.mirror().apply(delta));
    CK_CHECK(remote.status().keyboard_flags ==
             ckv::core::TerminalKeyboardFlags::DisambiguateEscapeCodes);
    CK_CHECK(view.on_key(escape));
    CK_CHECK(!sent.empty());
    if (!sent.empty()) {
        const auto* input = std::get_if<ckm::proto::Input>(&sent.back());
        CK_CHECK(input != nullptr);
        if (input != nullptr) CK_CHECK(input->bytes == "\x1b[27u");
    }

    // And a client that arrives later is told the same thing by the snapshot
    // rather than having to have watched the change go past.
    RemoteTerminalSubsession arriving(server.id, server.profile, nullptr);
    arriving.mirror().adopt(server.snapshot());
    CK_CHECK(arriving.status().keyboard_flags ==
             ckv::core::TerminalKeyboardFlags::DisambiguateEscapeCodes);
}

CK_TEST(a_reattach_keeps_the_clipboard_watermark_and_replays_no_write) {
    // M-R1's serial rule. The text of a clipboard write travels live and only
    // live: a snapshot that carried it would, on every reattach, put a child's
    // text over whatever its reader had copied since — minutes later, with
    // nobody asking. What the snapshot carries instead is the WATERMARK, so
    // that the next real write cannot land on a number a watcher has already
    // seen and be dropped in silence.
    FakeServer server;
    RemoteTerminalSubsession remote(server.id, server.profile, nullptr);
    remote.mirror().adopt(server.snapshot());
    CK_CHECK(remote.status().clipboard_serial == 0U);

    // The child asks; the server forwards the text once.
    server.child_printed("\x1b]52;c;aGVsbG8=\x07");  // "hello"
    remote.mirror().apply(server.clipboard_write());
    CK_CHECK(remote.snapshot().clipboard_text == "hello");
    const std::uint64_t after_the_write = remote.status().clipboard_serial;
    CK_CHECK(after_the_write != 0U);

    // The reattach. The watermark stands — it never goes backwards — and there
    // is no text behind it, so a view built over this mirror with a watermark
    // of its own at zero asks once and is given nothing to put anywhere.
    remote.mirror().adopt(server.snapshot());
    CK_CHECK(remote.status().clipboard_serial >= after_the_write);
    CK_CHECK(remote.snapshot().clipboard_text.empty());

    // And the write after the reattach still moves the number, which is the
    // half a reset watermark would have broken.
    server.child_printed("\x1b]52;c;d29ybGQ=\x07");  // "world"
    remote.mirror().apply(server.clipboard_write());
    CK_CHECK(remote.status().clipboard_serial > after_the_write);
    CK_CHECK(remote.snapshot().clipboard_text == "world");
}

CK_TEST(a_reattached_terminal_reports_the_printer_and_the_last_complaint) {
    // m-replay's other two. The printer is the one that a reader would notice
    // as a fault rather than as a missing feature: while the controller is on,
    // the child's output goes to the printer and NOT to the screen, so a client
    // that reported "idle" after a reattach would leave them watching a
    // terminal that has apparently stopped responding.
    FakeServer server;
    RemoteTerminalSubsession remote(server.id, server.profile, nullptr);
    remote.mirror().adopt(server.snapshot());
    CK_CHECK(!remote.status().printer_controller_active);
    CK_CHECK(remote.diagnostics().empty());

    // The complaint first and the printer second, because the controller
    // swallows everything a child writes after it — which is the whole reason
    // a reader has to be told it is on.
    server.child_printed("\x1b]99;x\x07");  // an OSC this terminal does not implement
    server.child_printed("\x1b[5i");        // MC: the printer controller on

    // Live: the two messages a server sends off the printer and diagnostics
    // damage flags.
    ckm::proto::PrintState printing;
    printing.term = server.id;
    printing.state = ckm::proto::PrinterState::Capturing;
    remote.mirror().apply(printing);
    ckm::proto::TermDiagnostic said;
    said.term = server.id;
    said.kind = ckm::proto::DiagnosticKind::UnsupportedSequence;
    said.text = "unsupported child OSC sequence";
    remote.mirror().apply(said);
    CK_CHECK(remote.status().printer_controller_active);
    CK_CHECK(remote.diagnostics().size() == 1U);

    // And after a reattach, from the snapshot alone.
    RemoteTerminalSubsession arriving(server.id, server.profile, nullptr);
    arriving.mirror().adopt(server.snapshot());
    CK_CHECK(arriving.status().printer_controller_active);
    CK_CHECK(arriving.diagnostics().size() == 1U);
    if (!arriving.diagnostics().empty())
        CK_CHECK(arriving.diagnostics().front().kind ==
                 ckv::core::TerminalDiagnostic::Kind::UnsupportedSequence);
    // The snapshot's copy is what the seam hands a view, so it is the same
    // answer either way round.
    CK_CHECK(arriving.snapshot().diagnostics.size() == 1U);
}

CK_TEST(a_mark_the_server_sends_is_the_mark_the_mirror_holds) {
    // `TermMeta`'s flags are marks — "since the reader was last in this
    // terminal" — so they are ASSIGNED rather than accumulated: the message
    // that says a reader has caught up is the same message with the bits off.
    FakeServer server;
    server.child_printed("\x1b]0;a caption\a");
    RemoteTerminalSubsession remote(server.id, server.profile, nullptr);
    remote.mirror().adopt(server.snapshot());
    CK_CHECK(!remote.mirror().bell_marked());
    CK_CHECK(!remote.mirror().activity_marked());

    ckm::proto::TermMeta meta;
    meta.term = server.id;
    // The title travels with the marks, and it is the one the server holds:
    // a message that stated an empty title would erase the caption a child
    // asked for.
    meta.title = "a caption";
    meta.flags = static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(ckm::proto::TermMetaFlag::Bell) |
        static_cast<std::uint8_t>(ckm::proto::TermMetaFlag::Activity));
    remote.mirror().apply(meta);
    CK_CHECK(remote.mirror().bell_marked());
    CK_CHECK(remote.mirror().activity_marked());
    CK_CHECK(remote.mirror().title() == "a caption");

    meta.flags = 0;
    remote.mirror().apply(meta);
    CK_CHECK(!remote.mirror().bell_marked());
    CK_CHECK(!remote.mirror().activity_marked());
    CK_CHECK(remote.mirror().title() == "a caption");
}

CK_TEST(a_terminal_that_only_left_the_session_reports_no_exit_status) {
    // `exited = 0` is the server saying the terminal went somewhere else — a
    // move — and it sends nothing in the status field because there is nothing
    // to report. Stored anyway, that zero becomes "the program finished with 0"
    // over a program that is still running in another session.
    FakeServer server;
    RemoteTerminalSubsession remote(server.id, server.profile, nullptr);
    remote.mirror().adopt(server.snapshot());

    ckm::proto::TermClosed left;
    left.term = server.id;
    left.exited = 0;
    left.exit_status = 0;
    remote.mirror().apply(left);
    CK_CHECK(!remote.mirror().exited());
    CK_CHECK(!remote.status().exit_code.has_value());
    CK_CHECK(remote.state() != ckv::core::TerminalSubsessionState::Exited);
}

CK_TEST(the_whole_client_ui_drives_a_terminal_it_does_not_own) {
    // The acceptance criterion, stated as a test: the real `ClientApp` — its
    // window, its footer, its prefix, its menus — with terminals that live
    // somewhere else. The only thing that changes is where the subsession comes
    // from; nothing below that line knows.
    FakeServer server;
    std::vector<ckm::proto::Message> sent;

    ckv::term::HeadlessTerminal host(ckv::Size{80, 24});
    ckv::ManualClock clock;
    ckv::ui::Application app(host, clock);

    ckm::client::ClientOptions options;
    options.settings.shell = "/bin/sh";
    options.settings.scrollback = 200;
    options.local_now = [] {
        return ckm::client::LocalMoment{{2026, 8, 17}, {12, 0, 0}};
    };
    RemoteTerminalSubsession* remote_ptr = nullptr;
    options.terminal_source = [&](ckm::client::TerminalRequest request) -> ckv::term::TerminalSubsession& {
        // What a client attached to a server does: ask for a terminal, and adopt
        // a mirror of it. The launch spec ckmux composed is what tells the server
        // which command to run — it is not run here.
        ckm::proto::NewTerminal ask;
        ask.command = request.launch.executable;
        sent.push_back(ask);
        auto remote = std::make_unique<RemoteTerminalSubsession>(
            server.id, request.launch.profile,
            [&sent](const ckm::proto::Message& message) { sent.push_back(message); });
        remote->mirror().set_history_limit(request.options.max_scrollback_lines);
        remote_ptr = remote.get();
        return app.adopt_terminal_subsession(std::move(remote));
    };

    ckm::client::ClientApp client(app, std::move(options));
    app.step(0);

    // A new terminal, through the same command a reader would use.
    CK_CHECK(client.new_terminal() != nullptr);
    CK_CHECK(remote_ptr != nullptr);
    if (remote_ptr == nullptr) return;
    bool asked = false;
    for (const ckm::proto::Message& message : sent)
        if (std::holds_alternative<ckm::proto::NewTerminal>(message)) asked = true;
    CK_CHECK(asked);

    // The server's terminal says something; the client's mirror takes it; the
    // window shows it.
    server.child_printed("a program in another process\r\n");
    remote_ptr->mirror().adopt(server.snapshot());
    // A change in a session is what tells the views over it to repaint, exactly
    // as it does for a local terminal — the client does not special-case where
    // the bytes came from.
    app.root().notify_terminal_subsession_changed(*remote_ptr);
    app.step(0);
    // Decoded rather than grepped. The presenter writes runs with cursor moves
    // between them, so "is the text in the bytes" is the wrong question — what a
    // reader sees is the screen those bytes produce, which is what a virtual
    // display gives back (the testing plan's own pattern for client tests).
    ckv::term::VirtualDisplay display(ckv::Size{80, 24});
    (void)display.write(host.written_bytes());
    std::string screen;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 80; ++x) {
            const ckv::Cell cell = display.frame().at(ckv::Point{x, y});
            if (!cell.is_continuation()) screen += cell.grapheme();
        }
    CK_CHECK(screen.find("a program in another process") != std::string::npos);

    // And the prefix still belongs to ckmux rather than to the child: the same
    // chord state machine, over a terminal in another process. `^B c` opens a
    // second terminal, which means a second request to the server rather than
    // two bytes down the input path.
    const std::size_t asks_before = std::count_if(
        sent.begin(), sent.end(), [](const ckm::proto::Message& message) {
            return std::holds_alternative<ckm::proto::NewTerminal>(message);
        });
    client.arm_prefix();
    client.resolve_prefix("c");
    app.step(0);
    const std::size_t asks_after = std::count_if(
        sent.begin(), sent.end(), [](const ckm::proto::Message& message) {
            return std::holds_alternative<ckm::proto::NewTerminal>(message);
        });
    CK_CHECK(asks_after == asks_before + 1);
    // Nothing of that chord reached the child: a prefix that leaked would have
    // produced an Input as well.
    bool leaked = false;
    for (std::size_t index = sent.size(); index-- > 0;) {
        if (std::holds_alternative<ckm::proto::NewTerminal>(sent[index])) break;
        if (std::holds_alternative<ckm::proto::Input>(sent[index])) leaked = true;
    }
    CK_CHECK(!leaked);
}

CK_TEST(a_click_inside_a_remote_terminal_window_reaches_the_child_over_the_wire) {
    // A field report (2026-08-18): a child that turned on SGR mouse tracking
    // saw zero mouse events, while the same host's KEYS reached it fine —
    // narrowing the break to something point-dispatch-specific rather than
    // the encode/status chain (already covered, and passing, in
    // `the_child_dialect_the_view_encodes_is_the_one_the_server_reported`'s
    // sibling test above). This drives the one layer that test does not: a
    // real `Desktop`/`Window`/`Application` hit-test, exactly as a reader's
    // click arrives — `topmost_view_at` finding the terminal view under the
    // point, not a direct call to `view.on_mouse`.
    FakeServer server;
    std::vector<ckm::proto::Message> sent;

    ckv::term::HeadlessTerminal host(ckv::Size{80, 24});
    ckv::ManualClock clock;
    ckv::ui::Application app(host, clock);

    ckm::client::ClientOptions options;
    options.settings.shell = "/bin/sh";
    options.settings.scrollback = 200;
    options.local_now = [] {
        return ckm::client::LocalMoment{{2026, 8, 17}, {12, 0, 0}};
    };
    RemoteTerminalSubsession* remote_ptr = nullptr;
    options.terminal_source = [&](ckm::client::TerminalRequest request) -> ckv::term::TerminalSubsession& {
        ckm::proto::NewTerminal ask;
        ask.command = request.launch.executable;
        sent.push_back(ask);
        auto remote = std::make_unique<RemoteTerminalSubsession>(
            server.id, request.launch.profile,
            [&sent](const ckm::proto::Message& message) { sent.push_back(message); });
        remote->mirror().set_history_limit(request.options.max_scrollback_lines);
        remote_ptr = remote.get();
        return app.adopt_terminal_subsession(std::move(remote));
    };

    ckm::client::ClientApp client(app, std::move(options));
    app.step(0);
    CK_CHECK(client.new_terminal() != nullptr);
    CK_CHECK(remote_ptr != nullptr);
    if (remote_ptr == nullptr) return;

    // The child turns on modern SGR mouse tracking, the mirror adopts it, and
    // the window over it repaints — same sequence a real reattach or a real
    // program's own startup takes.
    server.child_printed("\x1b[?1000h\x1b[?1006h");
    remote_ptr->mirror().adopt(server.snapshot());
    app.root().notify_terminal_subsession_changed(*remote_ptr);
    app.step(0);
    CK_CHECK(remote_ptr->status().mouse_reporting_enabled);

    ckv::widgets::Window* const window = client.desktop().windows()[0];
    CK_CHECK(window != nullptr);
    if (window == nullptr) return;
    const ckv::Rect content = window->content_rect();
    // Well inside the content area, away from the title bar and any frame
    // overlay (the scrollbar sits on the right edge, D-051) — an ordinary
    // click, not a boundary case.
    const ckv::Point at{content.x + content.width / 2, content.y + content.height / 2};

    const std::size_t inputs_before = std::count_if(
        sent.begin(), sent.end(),
        [](const ckm::proto::Message& message) { return std::holds_alternative<ckm::proto::Input>(message); });
    CK_CHECK(app.dispatch(ckv::MouseEvent{ckv::MouseAction::Down, ckv::MouseButton::Left, at,
                                          std::nullopt, ckv::Modifier::None}));
    const std::size_t inputs_after = std::count_if(
        sent.begin(), sent.end(),
        [](const ckm::proto::Message& message) { return std::holds_alternative<ckm::proto::Input>(message); });
    CK_CHECK(inputs_after == inputs_before + 1);
    if (inputs_after != inputs_before + 1) return;
    const auto* const input = std::get_if<ckm::proto::Input>(&sent.back());
    CK_CHECK(input != nullptr);
    // SGR: `CSI < 0 ; x ; y M` — button 0 (left) pressed, 1-based coordinates.
    if (input != nullptr) CK_CHECK(input->bytes.rfind("\x1b[<0;", 0) == 0 && input->bytes.back() == 'M');
}

CK_TEST(a_childs_clipboard_write_reaches_the_readers_clipboard_targets) {
    // M-R1, end to end through the client a reader actually has: the policy
    // said yes to the child, the server forwards the write, and it lands
    // wherever a yank from copy mode lands — because a reader configured those
    // targets for their clipboard, not for one way of filling it. Before this,
    // every one of those steps existed except the last, and the text was
    // dropped on the floor with the setting still claiming to honour it.
    FakeServer server;
    ckv::term::HeadlessTerminal host(ckv::Size{80, 24});
    ckv::ManualClock clock;
    ckv::ui::Application app(host, clock);

    ckm::client::ClientOptions options;
    options.settings.shell = "/bin/sh";
    options.settings.scrollback = 200;
    options.local_now = [] {
        return ckm::client::LocalMoment{{2026, 8, 17}, {12, 0, 0}};
    };
    // The reader's second clipboard target, recorded rather than forked: what
    // is on trial is that the copy reached the targets, and a test that forked
    // pbcopy would be testing the machine it runs on.
    std::vector<std::pair<std::string, std::string>> helpers;
    options.clipboard_writer = [&helpers](const std::string& command, std::string_view text) {
        helpers.emplace_back(command, std::string(text));
        return true;
    };
    RemoteTerminalSubsession* remote_ptr = nullptr;
    options.terminal_source =
        [&](ckm::client::TerminalRequest request) -> ckv::term::TerminalSubsession& {
        auto remote =
            std::make_unique<RemoteTerminalSubsession>(server.id, request.launch.profile, nullptr);
        remote->mirror().set_history_limit(request.options.max_scrollback_lines);
        remote_ptr = remote.get();
        return app.adopt_terminal_subsession(std::move(remote));
    };

    ckm::client::ClientApp client(app, std::move(options));
    app.step(0);
    CK_CHECK(client.new_terminal() != nullptr);
    CK_CHECK(remote_ptr != nullptr);
    if (remote_ptr == nullptr) return;

    remote_ptr->mirror().adopt(server.snapshot());
    app.root().notify_terminal_subsession_changed(*remote_ptr);
    app.step(0);
    // Nothing has been copied, and a mirror that has only been adopted must not
    // look as though something had: the watermark and the view's own start at
    // the same number precisely so that an attach copies nothing.
    CK_CHECK(client.internal_clipboard().empty());
    CK_CHECK(helpers.empty());

    server.child_printed("\x1b]52;c;aGVsbG8=\x07");  // "hello"
    remote_ptr->mirror().apply(server.clipboard_write());
    app.root().notify_terminal_subsession_changed(*remote_ptr);
    app.step(0);

    // ckmux's own clipboard, so `^B ]` pastes it whether or not anything
    // outside this process would take it...
    CK_CHECK(client.internal_clipboard() == "hello");
    // ...and the helper the reader named.
    bool the_helper_was_asked = false;
    for (const std::pair<std::string, std::string>& asked : helpers)
        if (asked.first == "pbcopy" && asked.second == "hello") the_helper_was_asked = true;
    CK_CHECK(the_helper_was_asked);
}
