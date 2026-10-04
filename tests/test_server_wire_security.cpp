// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/proto.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"
#include "scratch_directory.hpp"
#include "cvision/core/clock.hpp"
#include "cvision/testing/cktest.hpp"
#include <chrono>
#include <iterator>
#include <string>
#include <thread>
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
