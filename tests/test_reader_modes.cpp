// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// A reader who may only look (WP-49, the session model "Reader modes").
//
// WP-44 built the opt-in as a boolean — take the session, or join it — and the
// missing third answer is the one that makes sharing comfortable: join, and be
// unable to type. What these cases pin is that the refusal is the SERVER's. A
// client that greys its own views has adopted a convention, and a mode a stale
// or modified client can decline to honour is not a mode at all, so every case
// here speaks the wire directly and none of them goes through `ClientApp`.
//
// The other half of the mode is what a watcher KEEPS. Read-only means read-only
// to the session, not a crippled interface, and a fix that refused everything
// would pass a suite that only tested the refusals.
#if !defined(_WIN32)

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "common/config.hpp"
#include "common/proto.hpp"
#include "cvision/testing/cktest.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"
#include "server/terminals.hpp"

namespace {

using ckm::proto::AttachMode;
using ckm::proto::Message;
using ckm::proto::ReaderScope;

std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-modes" + std::to_string(static_cast<unsigned long>(::getpid()));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (std::string(name) + ".sock");
}

void forget(const std::filesystem::path& socket) {
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);
    std::filesystem::remove(std::filesystem::path(socket.string() + ".lock"), ignored);
    std::filesystem::remove(socket.parent_path(), ignored);
}

ckm::Settings test_settings() {
    ckm::Settings settings;
    settings.shell = "/bin/sh";
    settings.login_shell = false;
    settings.scrollback = 100;
    settings.max_fps = 30;
    settings.kill_empty_session = false;
    return settings;
}

struct WireClient {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader reader;

    bool connect(const std::filesystem::path& socket) {
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        if (result.status != ckm::platform::ConnectStatus::Connected) return false;
        stream = ckm::platform::Stream(result.fd);
        return true;
    }
    void say(const Message& message) { (void)stream.send(ckm::proto::encode(message)); }
    bool take(Message& message) {
        std::string arrived;
        (void)stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) return false;
        return reader.next(message) == ckm::proto::DecodeError::None;
    }
    void greet() {
        ckm::proto::Hello hello;
        hello.build = std::string(ckm::proto::kBuildIdentity);
        say(hello);
    }
};

// A server, a writing reader and a watching reader, with one `cat` between
// them. `cat` is the child because it makes the assertion a fact about the PTY
// rather than about the wire: what a reader types is echoed by the line
// discipline and written back by the program, so text that appears on the
// screen got all the way through, and text that does not never left this
// server. Counting messages would only prove what the server chose to send.
struct Session {
    ckv::ManualClock clock;
    ckm::server::Server server;
    WireClient writer;
    WireClient watcher;
    std::uint64_t term = 0;

    explicit Session(const std::filesystem::path& socket)
        : server(ckm::server::Server::Options{socket, test_settings()}, clock) {}

    bool join(WireClient& client, const std::filesystem::path& socket, AttachMode mode) {
        if (!client.connect(socket)) return false;
        client.greet();
        ckm::proto::Attach request;
        request.session = 0;
        request.columns = 80;
        request.rows = 24;
        request.mode = static_cast<std::uint8_t>(mode);
        client.say(request);
        for (int pass = 0; pass < 16; ++pass) {
            tick();
            Message message;
            while (client.take(message))
                if (std::holds_alternative<ckm::proto::Attached>(message)) return true;
        }
        return false;
    }

    bool open(const std::filesystem::path& socket) {
        if (server.start() != ckm::server::Server::StartStatus::Listening) return false;
        if (!join(writer, socket, AttachMode::Join)) return false;
        term = server.open_terminal(0, cat_spec()).id();
        if (!join(watcher, socket, AttachMode::Watch)) return false;
        settle();
        return term != 0;
    }

    static ckm::server::TerminalSpec cat_spec() {
        ckm::server::TerminalSpec spec;
        spec.command = "/bin/cat";
        spec.working_directory = "/";
        spec.columns = 80;
        spec.rows = 24;
        spec.pixel_width = 80 * 9;
        spec.pixel_height = 24 * 18;
        spec.environment = {{"TERM", "xterm-256color"}, {"PATH", "/usr/bin:/bin"}, {"LC_ALL", "C"}};
        return spec;
    }

    void tick() {
        clock.advance(40'000'000);
        (void)server.step();
    }

    void settle(int passes = 40) {
        for (int pass = 0; pass < passes; ++pass) {
            tick();
            Message drained;
            while (writer.take(drained)) { /* drained */ }
            while (watcher.take(drained)) { /* drained */ }
        }
    }

    void type(WireClient& who, std::string bytes) {
        ckm::proto::Input keys;
        keys.term = term;
        keys.bytes = std::move(bytes);
        who.say(keys);
    }

    // Everything the child's terminal is showing, rows joined. Read from the
    // server's own emulator rather than from a mirror, because a mirror only
    // holds what the server chose to send it and the question here is what the
    // CHILD received.
    std::string screen() {
        ckm::server::Terminal* const terminal = server.terminals().find(term);
        if (terminal == nullptr) return {};
        std::string text;
        for (const ckv::Cell& cell : terminal->session().cells()) text += cell.grapheme();
        return text;
    }

    // Whether `needle` reaches the child within a generous window. Generous
    // because a negative answer must not be a race: a case asserting that text
    // never arrived has to have waited longer than the case asserting that it
    // did.
    bool child_saw(std::string_view needle, int passes = 60) {
        for (int pass = 0; pass < passes; ++pass) {
            settle(4);
            if (screen().find(needle) != std::string::npos) return true;
        }
        return false;
    }

    // The first `Error` this reader is given, or code 0 for none.
    std::uint16_t error_for(WireClient& who, int passes = 16) {
        for (int pass = 0; pass < passes; ++pass) {
            tick();
            Message message;
            while (who.take(message))
                if (const auto* error = std::get_if<ckm::proto::Error>(&message)) return error->code;
        }
        return 0;
    }

    void stop() { server.terminals().close_all(); }
};

constexpr std::uint16_t kReadOnly = static_cast<std::uint16_t>(ckm::proto::ErrorCode::ReadOnly);
constexpr std::uint16_t kInvalid = static_cast<std::uint16_t>(ckm::proto::ErrorCode::InvalidRequest);

}  // namespace

CK_TEST(what_a_watcher_types_never_reaches_the_child) {
    const std::filesystem::path socket = private_socket("watch-input");
    forget(socket);
    Session session(socket);
    CK_CHECK(session.open(socket));

    // The positive partner FIRST, and it is not a formality: it establishes
    // that this fixture can see input arrive at all. Without it, "the watcher's
    // text never appeared" is equally consistent with a test that could never
    // have seen any text — which is the shape of a case that passes while
    // measuring nothing.
    session.type(session.writer, "hello-from-the-writer\r");
    CK_CHECK(session.child_saw("hello-from-the-writer"));

    // And the refusal, over the same path, into the same child, read the same
    // way.
    session.type(session.watcher, "forbidden-from-the-watcher\r");
    CK_CHECK(!session.child_saw("forbidden-from-the-watcher"));

    // The writer is unaffected by the attempt: a refusal must not wedge the
    // terminal for the reader who may use it.
    session.type(session.writer, "still-writing\r");
    CK_CHECK(session.child_saw("still-writing"));

    session.stop();
    forget(socket);
}

CK_TEST(a_watchers_keystroke_is_dropped_in_silence_and_nothing_else_is) {
    // The one deliberate silence in a handler whose rule is that everything is
    // answered — because a client waiting on a reply that never comes cannot
    // tell a server from a hung one. The exception is argued in the session model: this
    // client asked for the mode and knows it is watching, and an `Error` per
    // keystroke would put a frame on the hot path for every key a reader leans
    // on. Pinned so that a later "make it consistent" cannot quietly undo the
    // reasoning without a test saying so.
    const std::filesystem::path socket = private_socket("watch-silence");
    forget(socket);
    Session session(socket);
    CK_CHECK(session.open(socket));

    session.type(session.watcher, "abc");
    CK_CHECK(session.error_for(session.watcher) == 0);

    // A discrete act, on the other hand, IS answered — one refusal for one
    // thing the reader did, which is what makes it worth sending.
    ckm::proto::NewTerminal ask;
    ask.command = "/bin/sh";
    session.watcher.say(ask);
    CK_CHECK(session.error_for(session.watcher) == kReadOnly);

    session.stop();
    forget(socket);
}

CK_TEST(a_watchers_paste_is_acked_and_discarded_rather_than_left_to_wedge) {
    // WP-18's credit is about the CLIENT'S queue, not about what became of the
    // bytes: a chunk left unacked holds the rest of that paste and every later
    // one behind it. So a watcher who pastes must get an ack and no paste —
    // refusing without acking would be a reader whose client stops pasting
    // forever, and acking without refusing would be no mode at all.
    const std::filesystem::path socket = private_socket("watch-paste");
    forget(socket);
    Session session(socket);
    CK_CHECK(session.open(socket));

    ckm::proto::PasteChunk chunk;
    chunk.term = session.term;
    chunk.seq = 41;
    chunk.bytes = "pasted-by-the-watcher";
    session.watcher.say(chunk);

    bool acked = false;
    for (int pass = 0; pass < 16 && !acked; ++pass) {
        session.tick();
        Message message;
        while (session.watcher.take(message))
            if (const auto* ack = std::get_if<ckm::proto::PasteAck>(&message))
                if (ack->seq == 41) acked = true;
    }
    CK_CHECK(acked);
    CK_CHECK(!session.child_saw("pasted-by-the-watcher"));

    session.stop();
    forget(socket);
}

CK_TEST(a_watcher_may_not_change_the_session_and_is_told_which_request_failed) {
    // Every entry in the session model's refusal table, by name. A table in a plan that
    // no test walks is a table that drifts, and the `context` is asserted
    // because "something was refused" is not a thing a reader can act on.
    const std::filesystem::path socket = private_socket("watch-refusals");
    forget(socket);
    Session session(socket);
    CK_CHECK(session.open(socket));

    const auto refused = [&](const Message& message, std::string_view context) {
        session.watcher.say(message);
        for (int pass = 0; pass < 16; ++pass) {
            session.tick();
            Message arrived;
            while (session.watcher.take(arrived)) {
                const auto* error = std::get_if<ckm::proto::Error>(&arrived);
                if (error == nullptr) continue;
                CK_CHECK(error->code == kReadOnly);
                CK_CHECK(error->context == context);
                return true;
            }
        }
        return false;
    };

    ckm::proto::NewTerminal make;
    make.command = "/bin/sh";
    CK_CHECK(refused(make, "NewTerminal"));

    ckm::proto::CloseTerminal close;
    close.term = session.term;
    CK_CHECK(refused(close, "CloseTerminal"));

    ckm::proto::KillTerminal kill;
    kill.term = session.term;
    CK_CHECK(refused(kill, "KillTerminal"));

    ckm::proto::RenameTerminal rename_term;
    rename_term.id = session.term;
    rename_term.name = "mine";
    CK_CHECK(refused(rename_term, "RenameTerminal"));

    ckm::proto::RenameSession rename_session;
    rename_session.id = 0;
    rename_session.name = "mine";
    CK_CHECK(refused(rename_session, "RenameSession"));

    ckm::proto::SetDesktopSize resize;
    resize.columns = 200;
    resize.rows = 60;
    CK_CHECK(refused(resize, "SetDesktopSize"));

    // The arrangement is session state, so a watcher rearranging it would move
    // the other reader's windows.
    ckm::proto::LayoutEntry entry;
    entry.term = session.term;
    entry.rect = ckm::proto::Rect{3, 3, 20, 8};
    ckm::proto::SetLayout layout;
    layout.entries.push_back(entry);
    CK_CHECK(refused(layout, "SetLayout"));

    // And the one that is not a window at all: `MoveResize`'s rect is the
    // terminal's own grid and it SIZES A PTY. A watcher's mirror reports one
    // without any reader asking it to, so leaving it out would have every
    // watcher silently resizing the children of the session they came to look
    // at — the defect this case exists to keep out.
    ckm::proto::MoveResize grid;
    grid.term = session.term;
    grid.rect = ckm::proto::Rect{0, 0, 40, 12};
    CK_CHECK(refused(grid, "MoveResize"));

    ckm::proto::KillSession end;
    end.session = 0;
    CK_CHECK(refused(end, "KillSession"));

    // Strictly worse than any of the above: a watcher who can end the server
    // ends every session on the machine, in every window, for every reader.
    CK_CHECK(refused(ckm::proto::KillServer{}, "KillServer"));

    session.stop();
    forget(socket);
}

CK_TEST(a_watcher_keeps_everything_that_is_their_own) {
    // The half a fix that refused everything would pass. Read-only means
    // read-only TO THE SESSION, and the session model's second table is as load-bearing
    // as its first: a watcher who cannot ask what sessions exist, cannot see
    // the terminal they came to watch, and cannot stop watching is not in a
    // mode, they are in a broken client.
    const std::filesystem::path socket = private_socket("watch-keeps");
    forget(socket);
    Session session(socket);
    CK_CHECK(session.open(socket));

    // Questions are answered.
    session.watcher.say(ckm::proto::ListSessions{});
    bool listed = false;
    for (int pass = 0; pass < 16 && !listed; ++pass) {
        session.tick();
        Message message;
        while (session.watcher.take(message))
            if (std::holds_alternative<ckm::proto::SessionList>(message)) listed = true;
    }
    CK_CHECK(listed);

    // This reader's own screen size is their own, and so is a resnapshot — the
    // universal recovery, which a mode that refused it would take away from the
    // reader least able to do without it.
    ckm::proto::ClientResize smaller;
    smaller.columns = 60;
    smaller.rows = 20;
    session.watcher.say(smaller);
    CK_CHECK(session.error_for(session.watcher) == 0);

    ckm::proto::Attach again;
    again.session = 0;
    again.columns = 60;
    again.rows = 20;
    again.mode = static_cast<std::uint8_t>(AttachMode::Watch);
    session.watcher.say(again);
    bool resnapshotted = false;
    for (int pass = 0; pass < 16 && !resnapshotted; ++pass) {
        session.tick();
        Message message;
        while (session.watcher.take(message))
            if (std::holds_alternative<ckm::proto::Attached>(message)) resnapshotted = true;
    }
    CK_CHECK(resnapshotted);

    // And a watcher still SEES the session, which is the entire point of being
    // in it: what the other reader types arrives here as an ordinary delta.
    session.type(session.writer, "visible-to-the-watcher\r");
    CK_CHECK(session.child_saw("visible-to-the-watcher"));

    session.stop();
    forget(socket);
}

CK_TEST(a_reader_may_stop_watching_but_may_not_take_a_session_from_themselves) {
    const std::filesystem::path socket = private_socket("mode-self");
    forget(socket);
    Session session(socket);
    CK_CHECK(session.open(socket));

    // `{Me, TakeOver}` is refused: attaching is how a session is taken, and
    // reading a scope of *me* as "take it from the others" would make the most
    // destructive act on this wire reachable by a typo in one byte.
    ckm::proto::SetReaderMode impossible;
    impossible.scope = static_cast<std::uint8_t>(ReaderScope::Me);
    impossible.mode = static_cast<std::uint8_t>(AttachMode::TakeOver);
    session.watcher.say(impossible);
    CK_CHECK(session.error_for(session.watcher) == kInvalid);
    // And it changed nothing — still watching, still refused.
    ckm::proto::NewTerminal still;
    still.command = "/bin/sh";
    session.watcher.say(still);
    CK_CHECK(session.error_for(session.watcher) == kReadOnly);

    // `{Me, Join}` is how a reader stops watching, and it is the one thing a
    // watcher may do that changes what they may do.
    ckm::proto::SetReaderMode stop_watching;
    stop_watching.scope = static_cast<std::uint8_t>(ReaderScope::Me);
    stop_watching.mode = static_cast<std::uint8_t>(AttachMode::Join);
    session.watcher.say(stop_watching);
    session.settle(4);
    session.type(session.watcher, "no-longer-watching\r");
    CK_CHECK(session.child_saw("no-longer-watching"));

    // Nothing was sent back for a mode this reader asked for themselves: they
    // just did it, and a server telling them so is a round trip that teaches
    // nothing.
    CK_CHECK(session.error_for(session.watcher) == 0);

    session.stop();
    forget(socket);
}

CK_TEST(one_reader_can_put_the_others_on_watch_and_give_typing_back) {
    const std::filesystem::path socket = private_socket("mode-others");
    forget(socket);
    Session session(socket);
    CK_CHECK(session.open(socket));

    // The watcher stops watching, so that BOTH readers can type — the state
    // this case needs before it can show one of them taking that away.
    ckm::proto::SetReaderMode join_properly;
    join_properly.scope = static_cast<std::uint8_t>(ReaderScope::Me);
    join_properly.mode = static_cast<std::uint8_t>(AttachMode::Join);
    session.watcher.say(join_properly);
    session.settle(4);
    session.type(session.watcher, "both-can-type\r");
    CK_CHECK(session.child_saw("both-can-type"));

    // Now the other reader says "look, but don't touch".
    ckm::proto::SetReaderMode hush;
    hush.scope = static_cast<std::uint8_t>(ReaderScope::Others);
    hush.mode = static_cast<std::uint8_t>(AttachMode::Watch);
    session.writer.say(hush);

    // The reader it was done TO is told, because they did not ask for it —
    // unlike the reader who changed their own mode above, who was not.
    bool told = false;
    std::uint8_t told_mode = 255;
    for (int pass = 0; pass < 16 && !told; ++pass) {
        session.tick();
        Message message;
        while (session.watcher.take(message))
            if (const auto* mode = std::get_if<ckm::proto::ReaderMode>(&message)) {
                told = true;
                told_mode = mode->mode;
            }
    }
    CK_CHECK(told);
    CK_CHECK(told_mode == static_cast<std::uint8_t>(AttachMode::Watch));

    session.type(session.watcher, "hushed\r");
    CK_CHECK(!session.child_saw("hushed"));
    // And the reader who asked is unaffected: `Others` means others.
    session.type(session.writer, "asker-still-types\r");
    CK_CHECK(session.child_saw("asker-still-types"));

    // Given back, which is the half that makes it a mode rather than a
    // punishment.
    ckm::proto::SetReaderMode unhush;
    unhush.scope = static_cast<std::uint8_t>(ReaderScope::Others);
    unhush.mode = static_cast<std::uint8_t>(AttachMode::Join);
    session.writer.say(unhush);
    session.settle(4);
    session.type(session.watcher, "typing-again\r");
    CK_CHECK(session.child_saw("typing-again"));

    session.stop();
    forget(socket);
}

CK_TEST(a_reader_can_ask_to_have_the_session_to_themselves) {
    // `{Others, TakeOver}` — "I want this session to myself". The eviction is
    // the one the attach path already performs, which is the whole reason
    // there is no `DetachOthers` verb: one message, one enum, and the
    // destructive combination falls out of the table rather than being invented
    // beside it.
    const std::filesystem::path socket = private_socket("mode-alone");
    forget(socket);
    Session session(socket);
    CK_CHECK(session.open(socket));

    ckm::proto::SetReaderMode alone;
    alone.scope = static_cast<std::uint8_t>(ReaderScope::Others);
    alone.mode = static_cast<std::uint8_t>(AttachMode::TakeOver);
    session.writer.say(alone);

    bool dropped = false;
    ckm::proto::DetachReason why = ckm::proto::DetachReason::User;
    for (int pass = 0; pass < 16 && !dropped; ++pass) {
        session.tick();
        Message message;
        while (session.watcher.take(message))
            if (const auto* detached = std::get_if<ckm::proto::Detached>(&message)) {
                dropped = true;
                why = detached->reason;
            }
    }
    CK_CHECK(dropped);
    CK_CHECK(why == ckm::proto::DetachReason::Takeover);

    // The reader who asked is still attached and still working — an eviction
    // that took the asker with it would pass a case that only looked at the
    // other end.
    session.type(session.writer, "alone-now\r");
    CK_CHECK(session.child_saw("alone-now"));

    session.stop();
    forget(socket);
}

CK_TEST(an_attach_mode_this_build_cannot_read_takes_over_rather_than_sharing) {
    // The safe reading of "I do not understand you". A newer client asking for
    // a mode this server has never heard of must not be silently granted a
    // share it did not get — a reader believing they are watching while they
    // are in fact typing into a colleague's session is the one outcome worth
    // ruling out, and it is strictly worse than an unwanted takeover, which is
    // both visible and immediately reversible.
    const std::filesystem::path socket = private_socket("mode-unknown");
    forget(socket);
    Session session(socket);
    CK_CHECK(session.open(socket));

    WireClient newer;
    CK_CHECK(newer.connect(socket));
    newer.greet();
    ckm::proto::Attach request;
    request.session = 0;
    request.columns = 80;
    request.rows = 24;
    request.mode = 200;  // no such mode, in this build or any yet written
    newer.say(request);

    bool writer_dropped = false;
    for (int pass = 0; pass < 20; ++pass) {
        session.tick();
        Message message;
        while (newer.take(message)) { /* drained */ }
        while (session.writer.take(message))
            if (std::holds_alternative<ckm::proto::Detached>(message)) writer_dropped = true;
    }
    CK_CHECK(writer_dropped);

    session.stop();
    forget(socket);
}

#endif  // !defined(_WIN32)
