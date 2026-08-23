// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// One name, one session (the session model's `new-session` and `rename-session` rows,
// made to agree by owner ruling on 2026-08-20).
//
// The two rows contradicted each other for months: create said "name collision
// → reject with error", rename said "a duplicate is their business", and the
// code implemented the second in both places. The id is still the identity —
// it is what every message on the wire refers to — but the NAME is the only
// handle a reader has, in the picker and in `ckmux attach build`, and a handle
// that points at two things is not a handle. So both paths refuse.
//
// The interesting cases here are not the refusals. They are the three things
// that must NOT become refusals: renaming a session to the name it already
// has, renaming to an empty string, and using two names that differ only in
// case. Each is a way a "reject duplicates" rule can quietly take something
// away from a reader that nobody meant to take.
#if !defined(_WIN32)

#include <cstdint>
#include <filesystem>
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

using ckm::proto::Message;

std::filesystem::path private_socket(std::string_view name) {
    const char* const base = std::getenv("TMPDIR");
    std::filesystem::path directory =
        std::filesystem::path(base != nullptr && *base != '\0' ? base : "/tmp");
    directory /= "ckmux-names" + std::to_string(static_cast<unsigned long>(::getpid()));
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

// A server, a connected client, and the two answers a naming request can get:
// the session list (it happened) or an `Error` (it did not).
struct Fixture {
    std::filesystem::path socket;
    ckv::ManualClock clock;
    ckm::server::Server server;
    ckm::platform::Stream stream;
    ckm::proto::FrameReader reader;

    explicit Fixture(std::string_view name)
        : socket(private_socket(name)),
          server(ckm::server::Server::Options{(forget(private_socket(name)), private_socket(name)),
                                              test_settings()},
                 clock) {
        CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);
        ckm::platform::ConnectResult result = ckm::platform::connect_to_server(socket);
        CK_CHECK(result.status == ckm::platform::ConnectStatus::Connected);
        stream = ckm::platform::Stream(result.fd);
        ckm::proto::Hello hello;
        hello.build = std::string(ckm::proto::kBuildIdentity);
        say(hello);
        (void)settle();
    }

    ~Fixture() {
        server.terminals().close_all();
        forget(socket);
    }

    void say(const Message& message) { (void)stream.send(ckm::proto::encode(message)); }

    // Steps the server and drains everything that came back, keeping the last
    // session list and the last error seen. Both are cleared first, so each
    // call answers about THIS request rather than about any before it.
    struct Answer {
        bool listed = false;
        bool refused = false;
        std::uint16_t code = 0;
        std::string human;
        std::string context;
        std::vector<ckm::proto::SessionInfo> sessions;
    };

    Answer settle(int passes = 12) {
        Answer answer;
        for (int pass = 0; pass < passes; ++pass) {
            (void)server.step();
            std::string arrived;
            (void)stream.receive(arrived);
            if (!arrived.empty() && !reader.append(arrived)) break;
            Message message;
            while (reader.next(message) == ckm::proto::DecodeError::None) {
                if (const auto* list = std::get_if<ckm::proto::SessionList>(&message)) {
                    answer.listed = true;
                    answer.sessions = list->sessions;
                } else if (const auto* error = std::get_if<ckm::proto::Error>(&message)) {
                    answer.refused = true;
                    answer.code = error->code;
                    answer.human = error->human;
                    answer.context = error->context;
                }
            }
        }
        return answer;
    }

    Answer create(std::string name) {
        ckm::proto::NewSession ask;
        ask.name = std::move(name);
        ask.spawn_first = 0;
        say(ask);
        return settle();
    }

    Answer rename(std::uint64_t id, std::string name) {
        ckm::proto::RenameSession ask;
        ask.id = id;
        ask.name = std::move(name);
        say(ask);
        return settle();
    }

    std::uint64_t id_of(const std::vector<ckm::proto::SessionInfo>& sessions,
                        std::string_view name) {
        for (const ckm::proto::SessionInfo& info : sessions)
            if (info.name == name) return info.id;
        return 0;
    }

    // How many sessions the server holds, asked fresh rather than counted off
    // a stale list: "the refusal did not create it anyway" is half the claim.
    std::size_t session_count() {
        say(ckm::proto::ListSessions{});
        return settle().sessions.size();
    }
};

constexpr std::uint16_t kNameTaken = static_cast<std::uint16_t>(ckm::proto::ErrorCode::NameTaken);

}  // namespace

CK_TEST(a_second_session_cannot_take_a_name_that_is_already_in_use) {
    Fixture f("create");
    const Fixture::Answer first = f.create("build");
    CK_CHECK(!first.refused);
    CK_CHECK(f.id_of(first.sessions, "build") != 0);

    const Fixture::Answer second = f.create("build");
    CK_CHECK(second.refused);
    CK_CHECK(second.code == kNameTaken);
    CK_CHECK(second.context == "NewSession");
    // The name a reader typed, back in the sentence, so they can see WHICH of
    // their names collided without going to look.
    CK_CHECK(second.human.find("build") != std::string::npos);
    // And the refusal refused: a session that was created and then reported as
    // an error would be the worst of both answers.
    CK_CHECK(f.session_count() == 1);
}

CK_TEST(a_rename_into_a_taken_name_is_refused_and_the_old_name_survives) {
    // The half the owner's ruling added, and the reason it had to be added: a
    // name a reader cannot take at creation is one they could otherwise reach
    // in two steps, by making it under another name and renaming into it.
    Fixture f("rename");
    (void)f.create("build");
    const Fixture::Answer made = f.create("scratch");
    const std::uint64_t scratch = f.id_of(made.sessions, "scratch");
    CK_CHECK(scratch != 0);

    const Fixture::Answer refused = f.rename(scratch, "build");
    CK_CHECK(refused.refused);
    CK_CHECK(refused.code == kNameTaken);
    CK_CHECK(refused.context == "RenameSession");

    // Refused means unchanged, not half-applied.
    const std::vector<ckm::proto::SessionInfo> after = (f.say(ckm::proto::ListSessions{}), f.settle().sessions);
    CK_CHECK(after.size() == 2);
    CK_CHECK(f.id_of(after, "scratch") == scratch);
    CK_CHECK(f.id_of(after, "build") != scratch);
}

CK_TEST(a_session_may_be_renamed_to_the_name_it_already_has) {
    // The case a naive "is this name taken?" check gets wrong, and it is the
    // ordinary one: a reader opens the rename dialog, changes nothing, and
    // presses OK. Telling them the name is taken — by themselves — is a
    // refusal with no remedy, so `except_id` exists exactly for this.
    Fixture f("self");
    const Fixture::Answer made = f.create("build");
    const std::uint64_t build = f.id_of(made.sessions, "build");
    CK_CHECK(build != 0);

    const Fixture::Answer again = f.rename(build, "build");
    CK_CHECK(!again.refused);
    const std::vector<ckm::proto::SessionInfo> after = (f.say(ckm::proto::ListSessions{}), f.settle().sessions);
    CK_CHECK(after.size() == 1);
    CK_CHECK(f.id_of(after, "build") == build);
}

CK_TEST(an_empty_rename_still_leaves_the_name_alone_rather_than_clearing_it) {
    // Pre-existing behaviour, asserted here because this package is where it
    // could have been lost: an empty name is not a duplicate of anything, so a
    // collision check written without care would sail past and blank the name.
    // A session with no name is a row in the picker with nothing to point at.
    Fixture f("empty");
    const Fixture::Answer made = f.create("build");
    const std::uint64_t build = f.id_of(made.sessions, "build");

    const Fixture::Answer blank = f.rename(build, "");
    CK_CHECK(!blank.refused);
    const std::vector<ckm::proto::SessionInfo> after = (f.say(ckm::proto::ListSessions{}), f.settle().sessions);
    CK_CHECK(f.id_of(after, "build") == build);
}

CK_TEST(names_differing_only_in_case_are_two_names) {
    // Claimed in the session model and in the code comment, so asserted rather than
    // assumed. Case folding is a locale question — Turkish dotless i is the
    // standard example — and a multiplexer has no business answering it. A
    // reader who wants `Build` and `build` as separate sessions gets them.
    Fixture f("case");
    CK_CHECK(!f.create("Build").refused);
    CK_CHECK(!f.create("build").refused);
    CK_CHECK(f.session_count() == 2);
}

CK_TEST(the_name_the_server_invents_never_collides_with_one_a_reader_chose) {
    // `next_session_name()` counts one past the largest `session-N` it can
    // see, and that has to hold when the largest was typed by a reader rather
    // than generated — otherwise the server refuses its own next session, or
    // worse, creates the duplicate its own rule forbids.
    Fixture f("invented");
    CK_CHECK(!f.create("session-7").refused);
    const Fixture::Answer invented = f.create("");
    CK_CHECK(!invented.refused);
    CK_CHECK(f.id_of(invented.sessions, "session-8") != 0);
    CK_CHECK(f.session_count() == 2);
}

#endif  // !defined(_WIN32)
