// Copyright (c) 2026 C. Klukas. All rights reserved.
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
        terminal.clear_damage();
        return state;
    }

    // What the child printed, as the messages a client would receive.
    std::vector<ckm::proto::GridDelta> tick() {
        std::vector<ckm::proto::GridDelta> deltas;
        if (std::optional<ckm::proto::GridDelta> delta = engine.flush(id, terminal))
            deltas.push_back(std::move(*delta));
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
    // The gap rule (plans/05). A client that guessed would show a screen no
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

CK_TEST(the_child_dialect_the_view_encodes_is_the_one_the_server_reported) {
    // Keys are encoded client-side against mirrored mode state (plans/04), so the
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

CK_TEST(a_reader_pages_back_through_a_history_the_client_holds) {
    // Paging is local, which is why the client keeps the history at all
    // (plans/05): PgUp must not be a round trip, and after WP-6 it must work
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
    // display gives back (plans/09's own pattern for client tests).
    ckv::term::VirtualDisplay display(ckv::Size{80, 24});
    (void)display.write(host.written_bytes());
    std::string screen;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 80; ++x) {
            const ckv::Cell& cell = display.frame().at(ckv::Point{x, y});
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
