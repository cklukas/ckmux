// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/proto.hpp"
#include "client/server_connection.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"
#include "scratch_directory.hpp"
#include "cvision/core/clock.hpp"
#include "cvision/testing/cktest.hpp"
#include <chrono>
#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {
using Message = ckm::proto::Message;
struct Reader {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader frames;
    void say(const Message& message) { CK_CHECK(stream.send(ckm::proto::encode(message))); }
    std::vector<Message> take() {
        std::string bytes;
        (void)stream.receive(bytes);
        if (!bytes.empty()) CK_CHECK(frames.append(bytes));
        std::vector<Message> messages;
        Message message;
        while (frames.next(message) == ckm::proto::DecodeError::None) messages.push_back(std::move(message));
        return messages;
    }
};
struct Fixture {
    ckmtest::ScratchDirectory scratch{"wire-security"};
    std::filesystem::path endpoint;
    ckv::ManualClock clock;
    ckm::server::Server server;
    Reader good, other;
    Fixture() : endpoint(
#if defined(_WIN32)
        scratch.path().filename()
#else
        scratch.path() / "s"
#endif
        ), server({endpoint, ckm::Settings{}}, clock) {
        CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);
        connect(good);
        ckm::proto::NewSession create;
        create.name = "guard";
        create.spawn_first = 0;
        good.say(create);
        check_guard(settle(good), 1);
        connect(other);
        CK_CHECK(server.client_count() == 2);
    }
    std::vector<Message> settle(Reader& reader) {
        std::vector<Message> messages;
        for (int pass = 0; pass < 40; ++pass) {
            clock.advance(10'000'000);
            CK_CHECK(server.step());
            auto next = reader.take();
            messages.insert(messages.end(), std::make_move_iterator(next.begin()), std::make_move_iterator(next.end()));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return messages;
    }
    void connect(Reader& reader) {
        auto connection = ckm::platform::connect_to_server(endpoint);
        CK_CHECK(connection.status == ckm::platform::ConnectStatus::Connected);
        reader.stream = connection.take_stream();
        ckm::proto::Hello greeting;
        greeting.client_kind = ckm::proto::ClientKind::Cli;
        reader.say(greeting);
        const auto messages = settle(reader);
        CK_CHECK(messages.size() == 1);
        if (messages.size() == 1) CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(messages.front()));
    }
    void connect_until_greeted(Reader& reader) {
        auto connection = ckm::platform::connect_to_server(endpoint);
        CK_CHECK(connection.status == ckm::platform::ConnectStatus::Connected);
        reader.stream = connection.take_stream();
        ckm::proto::Hello greeting;
        greeting.client_kind = ckm::proto::ClientKind::Cli;
        reader.say(greeting);
        std::vector<Message> messages;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (messages.empty() && std::chrono::steady_clock::now() < deadline) {
            clock.advance(10'000'000);
            CK_CHECK(server.step());
            messages = reader.take();
            if (messages.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CK_CHECK(messages.size() == 1);
        if (messages.size() == 1) CK_CHECK(std::holds_alternative<ckm::proto::HelloAck>(messages.front()));
    }
    static void check_guard(const std::vector<Message>& messages, std::size_t count) {
        CK_CHECK(messages.size() == count);
        for (const auto& message : messages) {
            const auto* list = std::get_if<ckm::proto::SessionList>(&message);
            CK_CHECK(list != nullptr);
            if (list == nullptr) continue;
            CK_CHECK(list->sessions.size() == 1);
            if (list->sessions.size() == 1) CK_CHECK(list->sessions.front().name == "guard");
        }
    }
    void reject_only_other(std::string bytes) {
        CK_CHECK(other.stream.send(bytes));
        (void)settle(other);
        CK_CHECK(server.client_count() == 1);
        good.say(ckm::proto::ListSessions{});
        check_guard(settle(good), 1);
        // A real subsequent connection proves the listener still accepts after
        // the malformed peer disappears; no direct handler call substitutes it.
        Reader later;
        connect(later);
        later.say(ckm::proto::ListSessions{});
        check_guard(settle(later), 1);
    }
};
} // namespace

CK_TEST(stream_write_failure_preserves_final_reply_and_moves_preserve_write_failure) {
    // Exercise the application-facing transport, not a protocol callback.
    // Both an untouched reply and a partially consumed reply must survive.
    for (const std::size_t prefix : {std::size_t{0}, std::size_t{5}, std::size_t{4093}}) {
        ckmtest::ScratchDirectory scratch{"tail"};
        const auto endpoint =
#if defined(_WIN32)
            scratch.path().filename();
#else
            scratch.path() / "s";
#endif
        ckm::platform::Listener listener;
        CK_CHECK(listener.listen(endpoint) == ckm::platform::Listener::Status::Listening);
        auto connection = ckm::platform::connect_to_server(endpoint);
        CK_CHECK(connection.status == ckm::platform::ConnectStatus::Connected);
        auto client = connection.take_stream();
        ckm::platform::Listener::AcceptResult accepted;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            accepted = listener.accept_one();
        } while (accepted.status == ckm::platform::Listener::AcceptStatus::Idle &&
                 std::chrono::steady_clock::now() < deadline);
        CK_CHECK(accepted.status == ckm::platform::Listener::AcceptStatus::Accepted);
        auto server = accepted.take_stream();
        if (!server.open() || !client.open()) return;
        std::string reply(4093, '\0');
        for (std::size_t i = 0; i != reply.size(); ++i) reply[i] = static_cast<char>(i % 251);
        const bool cli_reply = prefix == reply.size();
        const ckm::proto::Pong final_reply{123456789};
        CK_CHECK(server.send(cli_reply ? reply + ckm::proto::encode(final_reply) : reply));
        while (server.wants_write() && std::chrono::steady_clock::now() < deadline)
            CK_CHECK(server.flush());
        CK_CHECK(server.queued() == 0);
        std::string received;
        if (prefix != 0) {
            while (received.size() != prefix && std::chrono::steady_clock::now() < deadline)
                CK_CHECK(client.receive(received, prefix - received.size()));
            CK_CHECK(received.size() == prefix);
        }
        server.close();
        CK_CHECK(!client.send("late request"));
        CK_CHECK(client.open());
        CK_CHECK(client.queued() == 0);
        CK_CHECK(!client.wants_write());
        // Both move paths must retain the failed write state and buffered read.
        ckm::platform::Stream moved(std::move(client));
        client = std::move(moved);
        CK_CHECK(!client.flush());
        CK_CHECK(!client.send("a second late request"));
        CK_CHECK(client.queued() == 0);
        if (cli_reply) {
            ckm::proto::FrameReader frames;
            Message message;
            CK_CHECK(ckm::client::await_message(client, frames, message));
            const auto* pong = std::get_if<ckm::proto::Pong>(&message);
            CK_CHECK(pong != nullptr);
            if (pong != nullptr) CK_CHECK(pong->nonce == final_reply.nonce);
            continue;
        }
        bool alive = true;
        while (alive && std::chrono::steady_clock::now() < deadline) {
            const auto before = received.size();
            alive = client.receive(received, 3);
            CK_CHECK(received.size() - before <= 3);
        }
        CK_CHECK(!alive);
        CK_CHECK(received == reply);
    }
}

CK_TEST(server_wire_oversized_advertised_payload_drops_only_the_offender) {
    Fixture fixture;
    auto frame = ckm::proto::encode(ckm::proto::ListSessions{});
    CK_CHECK(frame.size() == ckm::proto::kHeaderBytes);
    frame.replace(0, 4, 4, static_cast<char>(0xff));
    fixture.reject_only_other(std::move(frame));
}
CK_TEST(server_wire_unknown_message_type_drops_only_the_offender) {
    Fixture fixture;
    auto frame = ckm::proto::encode(ckm::proto::ListSessions{});
    frame[4] = static_cast<char>(0xff); frame[5] = static_cast<char>(0xff);
    fixture.reject_only_other(std::move(frame));
}
CK_TEST(server_wire_invalid_payload_drops_only_the_offender) {
    Fixture fixture;
    auto frame = ckm::proto::encode(ckm::proto::NewSession{});
    // Keep a valid type but advertise zero bytes for its required fields.
    frame.resize(ckm::proto::kHeaderBytes);
    frame.replace(0, 4, 4, '\0');
    fixture.reject_only_other(std::move(frame));
}
CK_TEST(server_wire_partial_and_coalesced_valid_frames_are_not_malformed) {
    Fixture fixture;
    const auto frame = ckm::proto::encode(ckm::proto::ListSessions{});
    CK_CHECK(fixture.other.stream.send(std::string_view(frame).substr(0, 3)));
    CK_CHECK(fixture.settle(fixture.other).empty());
    CK_CHECK(fixture.server.client_count() == 2);
    CK_CHECK(fixture.other.stream.send(std::string_view(frame).substr(3)));
    Fixture::check_guard(fixture.settle(fixture.other), 1);
    CK_CHECK(fixture.other.stream.send(frame + frame));
    Fixture::check_guard(fixture.settle(fixture.other), 2);
    fixture.good.say(ckm::proto::ListSessions{});
    Fixture::check_guard(fixture.settle(fixture.good), 1);
}

CK_TEST(server_wire_more_than_64_authenticated_clients_receive_simultaneous_replies_and_rearm) {
    Fixture fixture;
    std::vector<std::unique_ptr<Reader>> additional;
    // Two existing plus65 new clients require more than64 live application
    // read sources, independently of how many native events a stream exposes.
    for (int index = 0; index < 65; ++index) {
        auto reader = std::make_unique<Reader>();
        fixture.connect_until_greeted(*reader);
        additional.push_back(std::move(reader));
    }
    CK_CHECK(fixture.server.client_count() == 67);
    std::vector<Reader*> readers{&fixture.good, &fixture.other};
    for (const auto& reader : additional) readers.push_back(reader.get());
    std::vector<unsigned> replies(readers.size(), 0);
    for (auto* reader : readers) reader->say(ckm::proto::ListSessions{});
    for (int pass = 0; pass < 80; ++pass) {
        fixture.clock.advance(10'000'000);
        CK_CHECK(fixture.server.step());
        for (std::size_t index = 0; index < readers.size(); ++index) {
            const auto messages = readers[index]->take();
            if (!messages.empty()) {
                Fixture::check_guard(messages, 1);
                replies[index] += static_cast<unsigned>(messages.size());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (const auto count : replies) CK_CHECK(count == 1);
    // Retiring the tail source must not poison the next wait, and the listener
    // must accept a replacement while still owning more than64 other clients.
    additional.back()->stream.close();
    (void)fixture.settle(fixture.good);
    CK_CHECK(fixture.server.client_count() == 66);
    Reader replacement;
    fixture.connect_until_greeted(replacement);
    CK_CHECK(fixture.server.client_count() == 67);
    replacement.say(ckm::proto::ListSessions{});
    Fixture::check_guard(fixture.settle(replacement), 1);
    fixture.good.say(ckm::proto::ListSessions{});
    Fixture::check_guard(fixture.settle(fixture.good), 1);
}

CK_TEST(server_wire_nonreading_client_saturates_its_queue_without_harming_other_clients) {
    Fixture fixture;
    // Populate through the authenticated protocol, without launching shells.
    // Replies are large enough to exceed the actual production queue limit;
    // the fixture never lowers that limit or injects private queue state.
    for (int index = 0; index < 100; ++index) {
        ckm::proto::NewSession create;
        create.name = "queue-witness-session-" + std::to_string(index);
        create.spawn_first = 0;
        fixture.good.say(create);
    }
    unsigned created_replies = 0;
    const auto creation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (created_replies < 100 && std::chrono::steady_clock::now() < creation_deadline) {
        fixture.clock.advance(10'000'000);
        CK_CHECK(fixture.server.step());
        for (const auto& message : fixture.good.take()) {
            CK_CHECK(std::holds_alternative<ckm::proto::SessionList>(message));
            ++created_replies;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(created_replies == 100);
    CK_CHECK(fixture.server.client_count() == 2);
    const auto check_list = [](const std::vector<Message>& messages) {
        for (const auto& message : messages) {
            const auto* list = std::get_if<ckm::proto::SessionList>(&message);
            CK_CHECK(list != nullptr);
            if (list == nullptr) continue;
            CK_CHECK(list->sessions.size() == 101);
            CK_CHECK(std::count_if(list->sessions.begin(), list->sessions.end(),
                [](const auto& session) { return session.name == "guard"; }) == 1);
        }
    };
    const auto request = ckm::proto::encode(ckm::proto::ListSessions{});
    std::string batch;
    for (int index = 0; index < 64; ++index) batch += request;
    std::size_t highest_queued = 0;
    unsigned healthy_replies = 0;
    unsigned sent_requests = 0;
    const auto saturation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (fixture.server.client_count() == 2 && sent_requests < 8'000 &&
           std::chrono::steady_clock::now() < saturation_deadline) {
        CK_CHECK(fixture.other.stream.send(batch));
        sent_requests += 64;
        fixture.good.say(ckm::proto::ListSessions{});
        fixture.clock.advance(10'000'000);
        CK_CHECK(fixture.server.step());
        highest_queued = std::max(highest_queued, fixture.server.queued_bytes());
        const auto messages = fixture.good.take();
        check_list(messages);
        healthy_replies += static_cast<unsigned>(messages.size());
        // Deliberately never read the offender's replies. Native completion
        // processing remains part of the real stream and server wait loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(highest_queued > ckm::platform::Stream::kHighWaterBytes);
    // There are two queues, but the healthy peer has only tiny list replies.
    // The bound includes one protocol-sized healthy reply, not another limit.
    CK_CHECK(highest_queued <= ckm::platform::Stream::kHardLimitBytes +
                              ckm::proto::kMaxPayloadBytes + ckm::proto::kHeaderBytes);
    CK_CHECK(fixture.server.client_count() == 1);
    CK_CHECK(healthy_replies > 0);
    fixture.good.say(ckm::proto::ListSessions{});
    const auto remaining = fixture.settle(fixture.good);
    CK_CHECK(!remaining.empty());
    check_list(remaining);
    Reader later;
    fixture.connect_until_greeted(later);
    later.say(ckm::proto::ListSessions{});
    const auto accepted = fixture.settle(later);
    CK_CHECK(accepted.size() == 1);
    check_list(accepted);
}
