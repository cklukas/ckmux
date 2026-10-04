// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "common/proto.hpp"
#include "platform/socket.hpp"
#include "server/server.hpp"
#include "scratch_directory.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "cvision/core/clock.hpp"
#include "cvision/testing/cktest.hpp"

namespace {
using Message = ckm::proto::Message;

struct Reader {
    ckm::platform::Stream stream;
    ckm::proto::FrameReader frames;
    void connect(const std::filesystem::path& endpoint, ckm::proto::ClientKind kind) {
        auto connection = ckm::platform::connect_to_server(endpoint);
        CK_CHECK(connection.status == ckm::platform::ConnectStatus::Connected);
        stream = connection.take_stream();
        ckm::proto::Hello greeting;
        greeting.client_kind = kind;
        say(greeting);
    }
    void say(const Message& message) { CK_CHECK(stream.send(ckm::proto::encode(message))); }
    std::vector<Message> take() {
        std::string arrived;
        (void)stream.receive(arrived);
        if (!arrived.empty()) CK_CHECK(frames.append(arrived));
        std::vector<Message> result;
        Message message;
        while (frames.next(message) == ckm::proto::DecodeError::None)
            result.push_back(std::move(message));
        return result;
    }
};

struct Fixture {
    ckmtest::ScratchDirectory scratch{"cli-replies"};
    std::filesystem::path endpoint;
    ckv::ManualClock clock;
    ckm::server::Server server;
    Reader first;
    Reader second;
    Reader ui;
    Fixture()
        : endpoint(
#if defined(_WIN32)
              scratch.path().filename()
#else
              scratch.path() / "s"
#endif
          ), server({endpoint, ckm::Settings{}}, clock) {
        CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);
        for (auto* reader : {&first, &second, &ui}) {
            // A named-pipe listener rearms its next instance when accepted.
            // Pump this single-threaded server between real connections.
            reader->connect(endpoint, reader == &ui ? ckm::proto::ClientKind::Ui
                                                   : ckm::proto::ClientKind::Cli);
            const auto messages = settle(*reader);
            CK_CHECK(std::count_if(messages.begin(), messages.end(), [](const Message& message) {
                return std::holds_alternative<ckm::proto::HelloAck>(message);
            }) == 1);
        }
    }
    std::vector<Message> settle(Reader& reader) {
        std::vector<Message> result;
        for (int pass = 0; pass < 20; ++pass) {
            clock.advance(40'000'000);
            CK_CHECK(server.step());
            auto next = reader.take();
            result.insert(result.end(), std::make_move_iterator(next.begin()),
                          std::make_move_iterator(next.end()));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return result;
    }
    ckm::proto::SessionList create(Reader& reader, std::string name) {
        ckm::proto::NewSession request;
        request.name = std::move(name);
        request.spawn_first = 0;
        reader.say(request);
        const auto messages = settle(reader);
        CK_CHECK(messages.size() == 1U);
        if (messages.size() == 1U) {
            const auto* list = std::get_if<ckm::proto::SessionList>(&messages.front());
            CK_CHECK(list != nullptr);
            if (list != nullptr) return *list;
        }
        return {};
    }
};
}

CK_TEST(cli_creation_replies_are_private_while_ui_pickers_still_update) {
    Fixture fixture;
    const auto first = fixture.create(fixture.first, "first");
    CK_CHECK(first.sessions.size() == 1U);
    // Force the exact failing order: CLI2 has greeted, but CLI1 creates first.
    CK_CHECK(fixture.settle(fixture.second).empty());
    const auto pushed = fixture.settle(fixture.ui);
    CK_CHECK(pushed.size() == 1U);
    if (pushed.size() == 1U) CK_CHECK(std::holds_alternative<ckm::proto::SessionList>(pushed.front()));
    const auto second = fixture.create(fixture.second, "second");
    CK_CHECK(second.sessions.size() == 2U);
    if (!second.sessions.empty()) CK_CHECK(second.sessions.back().name == "second");
    CK_CHECK(fixture.settle(fixture.first).empty());
}

CK_TEST(unnamed_cli_creation_gets_its_own_immediate_snapshot_not_an_earlier_push) {
    Fixture fixture;
    (void)fixture.create(fixture.first, "other");
    const auto created = fixture.create(fixture.second, {});
    CK_CHECK(created.sessions.size() == 2U);
    if (created.sessions.size() == 2U) {
        // Names use the highest existing session-N suffix, not the wire id.
        CK_CHECK(created.sessions.back().name == "session-1");
        CK_CHECK(created.sessions.back().id > created.sessions.front().id);
    }
    fixture.first.say(ckm::proto::ListSessions{});
    const auto listed = fixture.settle(fixture.first);
    CK_CHECK(listed.size() == 1U);
    if (listed.size() == 1U) {
        const auto* list = std::get_if<ckm::proto::SessionList>(&listed.front());
        CK_CHECK(list != nullptr);
        if (list != nullptr) CK_CHECK(list->sessions == created.sessions);
    }
}

CK_TEST(cli_kill_completion_is_delivered_then_its_list_subscription_ends) {
    Fixture fixture;
    const auto first = fixture.create(fixture.first, "doomed");
    (void)fixture.create(fixture.second, "guard");
    CK_CHECK(first.sessions.size() == 1U);
    if (first.sessions.size() != 1U) return;
    fixture.first.say(ckm::proto::KillSession{first.sessions.front().id, 1, 0});
    const auto killed = fixture.settle(fixture.first);
    CK_CHECK(killed.size() == 1U);
    if (killed.size() == 1U) {
        const auto* list = std::get_if<ckm::proto::SessionList>(&killed.front());
        CK_CHECK(list != nullptr);
        if (list != nullptr) {
            CK_CHECK(list->sessions.size() == 1U);
            if (list->sessions.size() == 1U) CK_CHECK(list->sessions.front().name == "guard");
        }
    }
    CK_CHECK(fixture.settle(fixture.second).empty());
    (void)fixture.create(fixture.second, "later");
    CK_CHECK(fixture.settle(fixture.first).empty());
}
