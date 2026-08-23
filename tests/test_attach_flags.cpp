// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The two flags `ckmux attach` grew after WP-40 and WP-44 had landed their
// wires: `--adopt-size` and `--share`.
//
// `--share` is why this suite exists at all. `Attach.share` shipped with WP-44
// — declared, encoded, decoded, handled by the server, and round-tripped by
// `test_proto`'s catalogue — and no line in the entire client ever set it. The
// catalogue guard cannot see that: it proves every alternative of the wire
// variant survives a round trip, which is a claim about the ENCODER, not about
// whether anything in the program ever produces the value. A field with no
// producer is shaped and dead in the one direction that guard does not look.
//
// So the cases below are deliberately in two layers, and the second is the
// point: what a reader typed becomes a flag (parse), AND the flag becomes a
// byte on the wire (produce). A suite with only the first would have passed on
// the day WP-44 shipped unreachable.
#if !defined(_WIN32)

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "client/cli.hpp"
#include "client/server_session.hpp"
#include "common/proto.hpp"
#include "cvision/testing/cktest.hpp"

namespace {

ckm::client::CliRequest parse(const std::vector<std::string>& words) {
    return ckm::client::parse_cli(words);
}

// Every `Attach` a `ServerSession` sends, in order.
struct AttachRecorder {
    std::vector<ckm::proto::Attach> attaches;
    ckm::client::ServerSession session{nullptr};

    AttachRecorder() {
        session = ckm::client::ServerSession([this](const ckm::proto::Message& message) {
            if (const auto* attach = std::get_if<ckm::proto::Attach>(&message))
                attaches.push_back(*attach);
        });
    }
};

}  // namespace

CK_TEST(attach_takes_its_flags_before_the_session_and_defaults_both_off) {
    // The default is the behaviour every reader had before these flags, which
    // is what makes them safe to add: nothing changes for anybody who does not
    // type them.
    const ckm::client::CliRequest plain = parse({"attach", "work"});
    CK_CHECK(plain.ok());
    CK_CHECK(plain.name == "work");
    CK_CHECK(!plain.share);
    CK_CHECK(!plain.adopt_size);

    const ckm::client::CliRequest shared = parse({"attach", "--share", "work"});
    CK_CHECK(shared.ok());
    CK_CHECK(shared.name == "work" && shared.share && !shared.adopt_size);

    const ckm::client::CliRequest sized = parse({"attach", "--adopt-size", "work"});
    CK_CHECK(sized.ok());
    CK_CHECK(sized.name == "work" && sized.adopt_size && !sized.share);

    // Both, in either order: they are independent requests about independent
    // things — who else may watch, and whose screen the world is measured in.
    for (const std::vector<std::string>& words :
         {std::vector<std::string>{"attach", "--share", "--adopt-size", "17"},
          std::vector<std::string>{"attach", "--adopt-size", "--share", "17"}}) {
        const ckm::client::CliRequest both = parse(words);
        CK_CHECK(both.ok());
        CK_CHECK(both.name == "17" && both.share && both.adopt_size);
    }
}

CK_TEST(a_misspelled_flag_is_refused_rather_than_taken_for_a_session_name) {
    // The failure this prevents is not the refusal — it is the DIAGNOSIS. A
    // reader who types `--adopt-siz` means the flag; resolving it as a session
    // name answers "no such session `--adopt-siz`" and sends them to `ckmux
    // ls` to look for something that was never going to be there.
    const ckm::client::CliRequest typo = parse({"attach", "--adopt-siz", "work"});
    CK_CHECK(!typo.ok());
    CK_CHECK(typo.problem.find("--adopt-siz") != std::string::npos);
    CK_CHECK(typo.usage.find("--adopt-size") != std::string::npos);

    // Flags alone are not a session, and two sessions are still two.
    const ckm::client::CliRequest nameless = parse({"attach", "--share"});
    CK_CHECK(!nameless.ok());
    CK_CHECK(nameless.problem.find("needs the session") != std::string::npos);

    const ckm::client::CliRequest crowded = parse({"attach", "--share", "one", "two"});
    CK_CHECK(!crowded.ok());
    CK_CHECK(crowded.problem.find("not several") != std::string::npos);
}

CK_TEST(kill_session_did_not_inherit_the_flags_when_attach_grew_them) {
    // `attach` and `kill-session` shared one parsing branch until these flags
    // arrived, and splitting a branch is exactly where a permission leaks into
    // the neighbour that was never meant to have it. `kill-session --share` is
    // meaningless, and meaningless input on the command that ENDS a reader's
    // programs must be refused rather than ignored.
    for (const char* flag : {"--share", "--adopt-size"}) {
        const ckm::client::CliRequest killed = parse({"kill-session", flag, "work"});
        CK_CHECK(!killed.ok());
    }
    // And its own form still works, which is what says the split did not break
    // the half that was already right.
    const ckm::client::CliRequest ordinary = parse({"kill-session", "work"});
    CK_CHECK(ordinary.ok());
    CK_CHECK(ordinary.name == "work");
}

CK_TEST(the_share_flag_reaches_the_wire_on_every_attach_not_only_the_first) {
    // The case the catalogue guard cannot state: somebody SENDS it.
    AttachRecorder shared;
    shared.session.set_share(true);
    CK_CHECK(shared.session.shares());
    shared.session.attach(7, ckv::Size{100, 30}, ckv::Size{9, 18});
    CK_CHECK(shared.attaches.size() == 1);
    CK_CHECK(shared.attaches[0].share == 1);

    // A heal, a session switch and a reconnection all re-`Attach`. One that
    // dropped the flag would silently convert a reader who chose to share into
    // a reader who took the session over — and the moment it happens is the
    // moment they are least able to tell why.
    shared.session.attach(7, ckv::Size{100, 30}, ckv::Size{9, 18});
    shared.session.attach(9, ckv::Size{80, 24}, ckv::Size{9, 18});
    CK_CHECK(shared.attaches.size() == 3);
    for (const ckm::proto::Attach& attach : shared.attaches) CK_CHECK(attach.share == 1);

    // The negative half, stated exactly rather than as "not set": a client
    // that did not ask must send a 0, because the server reads this byte to
    // decide between joining and taking over.
    AttachRecorder alone;
    CK_CHECK(!alone.session.shares());
    alone.session.attach(7, ckv::Size{100, 30}, ckv::Size{9, 18});
    CK_CHECK(alone.attaches.size() == 1);
    CK_CHECK(alone.attaches[0].share == 0);
}

#endif  // !defined(_WIN32)
