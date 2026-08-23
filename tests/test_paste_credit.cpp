// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Paste, paced by what the child can take (WP-18).
//
// A reader's keystrokes are human-rate: they go out as `Input` and nothing has
// to pace them. A paste is not — it is however much text their clipboard holds,
// arriving at once — and sent the same way it fills the connection's queue and
// then the terminal's faster than the program can drain either. So a paste is
// cut into chunks and only a couple are on the wire at a time, the next going
// when the server acks one, which it does as it writes that chunk to the PTY.
//
// The pacing is against the CHILD, not against the socket. That is the whole
// design: a server that acked on receipt would pace against its own buffer and
// let the entire paste through at once, which is what this exists to stop.
#if !defined(_WIN32)

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "client/server_session.hpp"
#include "common/proto.hpp"
#include "cvision/testing/cktest.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"
#include "server/terminals.hpp"

namespace {

using ckm::client::ServerSession;
using ckm::proto::Message;
using ckm::proto::PasteAck;
using ckm::proto::PasteChunk;

// A session with no socket under it: the constructor takes the "send this"
// callback, so a test can hold the wire in a vector and read what a paste
// actually put on it.
struct Wire {
    std::vector<Message> sent;
    ServerSession session{[this](const Message& message) { sent.push_back(message); }};

    std::vector<PasteChunk> chunks() const {
        std::vector<PasteChunk> found;
        for (const Message& message : sent)
            if (const auto* chunk = std::get_if<PasteChunk>(&message)) found.push_back(*chunk);
        return found;
    }
    void ack(std::uint32_t seq) {
        const Message message = PasteAck{seq};
        (void)session.handle(message);
    }
    // Everything the chunks carry, in the order they were sent — which is what
    // the child will see, and the only thing a reader cares about.
    std::string delivered() const {
        std::string text;
        for (const PasteChunk& chunk : chunks()) text += chunk.bytes;
        return text;
    }
};

// Longer than one chunk and not a multiple of it, so the last chunk is a
// partial one and an off-by-one in the cutting shows up as lost or duplicated
// text rather than as a suspiciously round number of chunks.
std::string long_paste(std::size_t chunks_wanted = 3) {
    std::string text;
    text.reserve(ServerSession::kPasteChunkBytes * chunks_wanted);
    for (std::size_t i = 0; i < ServerSession::kPasteChunkBytes * (chunks_wanted - 1) + 7U; ++i)
        text += static_cast<char>('a' + (i % 26));
    return text;
}

}  // namespace

// --- What goes on the wire, and when ---------------------------------------

CK_TEST(a_paste_that_fits_in_one_chunk_goes_at_once) {
    Wire w;
    w.session.paste(7, "hello");

    const std::vector<PasteChunk> chunks = w.chunks();
    CK_CHECK(chunks.size() == 1U);
    if (chunks.empty()) return;
    CK_CHECK(chunks[0].term == 7U);
    CK_CHECK(chunks[0].bytes == "hello");
    // Marked final, because it is: a child that is told a paste has ended can
    // stop treating what follows as pasted text.
    CK_CHECK(chunks[0].final_chunk == 1);
    CK_CHECK(w.session.pending_paste_chunks() == 0U);
    CK_CHECK(w.session.paste_chunks_in_flight() == 1U);
}

CK_TEST(a_long_paste_puts_only_the_credit_on_the_wire) {
    Wire w;
    const std::string text = long_paste(3);
    w.session.paste(7, text);

    // Three chunks' worth of text, two of them out and one waiting. This is
    // the whole point of the package: the rest is held here rather than in the
    // socket, where nothing can decide how fast it should go.
    CK_CHECK(w.chunks().size() == ServerSession::kPasteCredit);
    CK_CHECK(w.session.paste_chunks_in_flight() == ServerSession::kPasteCredit);
    CK_CHECK(w.session.pending_paste_chunks() == 1U);
    // And nothing in flight is marked final while text is still waiting.
    for (const PasteChunk& chunk : w.chunks()) CK_CHECK(chunk.final_chunk == 0);
}

CK_TEST(each_ack_releases_exactly_one_more_chunk) {
    Wire w;
    w.session.paste(7, long_paste(4));
    CK_CHECK(w.chunks().size() == 2U);

    w.ack(1);
    CK_CHECK(w.chunks().size() == 3U);
    CK_CHECK(w.session.paste_chunks_in_flight() == 2U);

    w.ack(2);
    CK_CHECK(w.chunks().size() == 4U);

    // The last one out is the last one there is, and says so.
    const std::vector<PasteChunk> chunks = w.chunks();
    CK_CHECK(chunks.back().final_chunk == 1);
    CK_CHECK(w.session.pending_paste_chunks() == 0U);

    // Acking the rest leaves nothing in flight and sends nothing new.
    w.ack(3);
    w.ack(4);
    CK_CHECK(w.chunks().size() == 4U);
    CK_CHECK(w.session.paste_chunks_in_flight() == 0U);
}

CK_TEST(the_text_arrives_whole_and_in_order) {
    // The property a reader actually has: what they copied is what the child
    // is given, byte for byte, whatever the chunking did in between.
    Wire w;
    const std::string text = long_paste(4);
    w.session.paste(7, text);
    for (std::uint32_t seq = 1; seq <= 4; ++seq) w.ack(seq);

    CK_CHECK(w.delivered() == text);
    const std::vector<PasteChunk> chunks = w.chunks();
    CK_CHECK(chunks.size() == 4U);
    if (chunks.size() != 4U) return;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        CK_CHECK(chunks[i].seq == static_cast<std::uint32_t>(i + 1));
        CK_CHECK(chunks[i].term == 7U);
        CK_CHECK(chunks[i].final_chunk == (i + 1 == chunks.size() ? 1 : 0));
    }
}

CK_TEST(two_terminals_pasting_do_not_ack_each_others_chunks) {
    // `PasteAck` carries a seq and nothing else, so the numbering has to be
    // per CONNECTION. Were it per terminal, an ack for chunk 1 would be
    // ambiguous the moment two windows pasted at once — and the credit would
    // be spent on the wrong queue.
    Wire w;
    w.session.paste(7, long_paste(2));
    w.session.paste(9, "short");

    const std::vector<PasteChunk> first = w.chunks();
    CK_CHECK(first.size() == 2U);
    if (first.size() != 2U) return;
    CK_CHECK(first[0].term == 7U && first[1].term == 7U);
    CK_CHECK(first[0].seq != first[1].seq);

    // The second terminal's text waits its turn rather than jumping the queue:
    // one credit pool, first come first served.
    CK_CHECK(w.session.pending_paste_chunks() == 1U);
    w.ack(first[0].seq);
    const std::vector<PasteChunk> after = w.chunks();
    CK_CHECK(after.size() == 3U);
    if (after.size() != 3U) return;
    CK_CHECK(after.back().term == 9U);
    CK_CHECK(after.back().bytes == "short");
    // Sequence numbers are unique across both terminals.
    CK_CHECK(after[0].seq != after[2].seq && after[1].seq != after[2].seq);
}

// --- What happens when the reader is no longer there -----------------------

CK_TEST(a_paste_does_not_survive_the_session_being_taken_away) {
    // The dangerous case, and the reason this is not just a queue. A takeover
    // detaches this client while the connection lives, and the server routes a
    // chunk by terminal id without asking who is attached — so finishing the
    // paste would type the rest of a reader's clipboard into a window that now
    // belongs to somebody else.
    Wire w;
    w.session.paste(7, long_paste(4));
    CK_CHECK(w.session.pending_paste_chunks() == 2U);

    w.session.windows_forgotten();
    CK_CHECK(w.session.pending_paste_chunks() == 0U);
    CK_CHECK(w.session.paste_chunks_in_flight() == 0U);

    const std::size_t sent_before = w.chunks().size();
    w.ack(1);
    w.ack(2);
    CK_CHECK(w.chunks().size() == sent_before);  // nothing follows the acks
}

CK_TEST(a_paste_does_not_survive_the_connection_going) {
    Wire w;
    w.session.paste(7, long_paste(3));
    w.session.connection_lost();
    CK_CHECK(w.session.pending_paste_chunks() == 0U);
    CK_CHECK(w.session.paste_chunks_in_flight() == 0U);
}

CK_TEST(an_ack_for_nothing_is_harmless) {
    // A server that acked twice, or a stale ack arriving after the queue was
    // dropped, must not wrap the credit counter round to something enormous —
    // which would let the NEXT paste out all at once, silently, and only in a
    // session where something had already gone wrong.
    Wire w;
    w.ack(1);
    w.ack(2);
    CK_CHECK(w.session.paste_chunks_in_flight() == 0U);

    w.session.paste(7, long_paste(3));
    CK_CHECK(w.chunks().size() == ServerSession::kPasteCredit);
}

CK_TEST(pasting_nothing_says_nothing) {
    Wire w;
    w.session.paste(7, "");
    CK_CHECK(w.sent.empty());
}

// --- The server half -------------------------------------------------------

namespace {

std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-paste" + std::to_string(static_cast<unsigned long>(::getpid()));
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
};

// Steps the server until something arrives for the client, or until it is
// clear nothing will.
bool pump_until(ckm::server::Server& server, WireClient& client, Message& message,
                int passes = 12) {
    for (int pass = 0; pass < passes; ++pass) {
        if (!server.step()) return false;
        if (client.take(message)) return true;
    }
    return false;
}

}  // namespace

CK_TEST(the_server_acks_a_chunk_it_could_not_deliver) {
    // The anti-stall rule, and the one that is easy to get wrong: the credit is
    // about the CLIENT'S queue, not about whether the terminal is still there.
    // A reader who pastes into a window whose program has just ended would
    // otherwise wait for ever on an ack that never comes — holding the rest of
    // that paste, and every later paste behind it, in a queue nothing drains.
    const std::filesystem::path socket = private_socket("ack-anyway");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient client;
    CK_CHECK(client.connect(socket));
    ckm::proto::Hello hello;
    hello.build = std::string(ckm::proto::kBuildIdentity);
    client.say(hello);
    Message reply;
    CK_CHECK(pump_until(server, client, reply));  // HelloAck

    PasteChunk chunk;
    chunk.term = 4242;  // no such terminal, and there never was one
    chunk.seq = 17;
    chunk.final_chunk = 1;
    chunk.bytes = "text for nobody";
    client.say(chunk);

    Message answer;
    CK_CHECK(pump_until(server, client, answer));
    const auto* ack = std::get_if<PasteAck>(&answer);
    CK_CHECK(ack != nullptr);
    if (ack == nullptr) return;
    CK_CHECK(ack->seq == 17U);

    forget(socket);
}


// --- One terminal, two clients (WP-42) -------------------------------------
//
// The server routes a `PasteChunk` by terminal id and asks nothing about who
// is attached — exactly as it does for `Input`. So two CONNECTED clients can
// paste into one terminal today, without multi-attach existing at all: a
// client that has been taken over still holds its socket, and a CLI client
// never attaches in the first place. Two pastes then interleave, chunk by
// chunk, and the child is given text neither reader typed.
//
// The ack is the observable. `PasteAck` means "written to the PTY" — that is
// the whole of the pacing contract — so the ORDER acks come back in is the
// order the writes happened in, and a test can assert the rule without
// reading a child's echo or waiting on real time.

namespace {

ckm::server::TerminalSpec spec_running(std::string command) {
    ckm::server::TerminalSpec spec;
    spec.command = std::move(command);
    spec.working_directory = "/";
    spec.columns = 80;
    spec.rows = 24;
    spec.pixel_width = 80 * 9;
    spec.pixel_height = 24 * 18;
    spec.environment = {{"TERM", "xterm-256color"}, {"PATH", "/usr/bin:/bin"}, {"LC_ALL", "C"}};
    return spec;
}

PasteChunk chunk_of(std::uint64_t term, std::uint32_t seq, bool last, std::string bytes) {
    PasteChunk chunk;
    chunk.term = term;
    chunk.seq = seq;
    chunk.final_chunk = last ? 1 : 0;
    chunk.bytes = std::move(bytes);
    return chunk;
}

// Steps the server a few times and reports whether this client has an ack
// waiting. For the cases whose claim is that one has NOT arrived.
bool acked(ckm::server::Server& server, WireClient& client, std::uint32_t seq, int passes = 8) {
    for (int pass = 0; pass < passes; ++pass) {
        if (!server.step()) return false;
        Message message;
        while (client.take(message)) {
            const auto* ack = std::get_if<PasteAck>(&message);
            if (ack != nullptr && ack->seq == seq) return true;
        }
    }
    return false;
}

}  // namespace

CK_TEST(a_second_clients_paste_waits_for_the_first_to_finish) {
    const std::filesystem::path socket = private_socket("two-pastes");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));

    WireClient first;
    WireClient second;
    CK_CHECK(first.connect(socket));
    CK_CHECK(second.connect(socket));
    ckm::proto::Hello hello;
    hello.build = std::string(ckm::proto::kBuildIdentity);
    first.say(hello);
    second.say(hello);
    Message greeting;
    CK_CHECK(pump_until(server, first, greeting));
    CK_CHECK(pump_until(server, second, greeting));

    // The first reader starts a paste and does not finish it.
    first.say(chunk_of(terminal.id(), 1, /*last=*/false, "first client, part one\n"));
    CK_CHECK(acked(server, first, 1));

    // The second reader pastes into the SAME terminal, whole and final.
    second.say(chunk_of(terminal.id(), 1, /*last=*/true, "second client, all of it\n"));
    // It must not be written yet: a terminal takes one paste at a time, and
    // the first reader's is still open. No ack means not written.
    CK_CHECK(!acked(server, second, 1));

    // The first reader finishes.
    first.say(chunk_of(terminal.id(), 2, /*last=*/true, "first client, part two\n"));
    CK_CHECK(acked(server, first, 2));

    // And the slot passes to whoever was waiting, without them resending.
    CK_CHECK(acked(server, second, 1));

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_client_that_goes_away_mid_paste_does_not_wedge_the_terminal) {
    // The failure this must not have: a reader whose laptop slept holding a
    // half-finished paste, and a terminal nobody else may paste into for the
    // rest of the session. The slot is released when its holder's connection
    // goes, exactly as every other per-connection resource is.
    const std::filesystem::path socket = private_socket("wedge");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);
    ckm::server::Terminal& terminal = server.open_terminal(0, spec_running("sleep 30"));

    WireClient waiting;
    ckm::proto::Hello hello;
    hello.build = std::string(ckm::proto::kBuildIdentity);
    {
        WireClient leaver;
        CK_CHECK(leaver.connect(socket));
        leaver.say(hello);
        Message greeting;
        CK_CHECK(pump_until(server, leaver, greeting));
        leaver.say(chunk_of(terminal.id(), 1, /*last=*/false, "half a paste"));
        CK_CHECK(acked(server, leaver, 1));
    }  // and its socket closes with the paste still open

    CK_CHECK(waiting.connect(socket));
    waiting.say(hello);
    Message greeting;
    CK_CHECK(pump_until(server, waiting, greeting));
    waiting.say(chunk_of(terminal.id(), 1, /*last=*/true, "the next reader's text"));
    CK_CHECK(acked(server, waiting, 1));

    server.terminals().close_all();
    forget(socket);
}

#endif  // !defined(_WIN32)
