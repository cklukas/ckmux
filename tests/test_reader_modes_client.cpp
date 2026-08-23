// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The reader-mode surfaces (WP-50): the Session menu's three acts, the two
// footer items, and the toasts that say somebody arrived, left, or changed what
// this reader may do.
//
// The server half is `test_reader_modes.cpp`, which speaks the wire and never
// builds a client. This file is the other end: no socket, every seam a
// `std::function` the case reads afterwards — so what it pins is what the
// CLIENT decides to send and to show, which is exactly the half a server test
// cannot see.
#include <cstdint>
#include <string>
#include <vector>

#include "client/client_app.hpp"
#include "common/proto.hpp"

#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"

namespace {

using ckm::client::ClientApp;
using ckm::client::ClientOptions;
using ckm::client::SessionRow;
using ckm::proto::AttachMode;
using ckm::proto::ReaderScope;
using ckv::ManualClock;
using ckv::Size;
using ckv::ui::Application;

struct Asked {
    ReaderScope scope = ReaderScope::Me;
    AttachMode mode = AttachMode::Join;
};

struct Fixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock clock;
    Application app{terminal, clock};
    std::vector<Asked> asks;
    ClientApp client;

    Fixture() : client(app, make_options()) {}

    ClientOptions make_options() {
        ClientOptions options;
        options.settings.shell = "/bin/cat";
        options.set_reader_mode = [this](ReaderScope scope, AttachMode mode) {
            asks.push_back(Asked{scope, mode});
        };
        // The picker's Attach needs somewhere to go, or its completion handler
        // drops the answer this suite is reading.
        options.attach_to_session = [](std::uint64_t, AttachMode) {};
        return options;
    }

    void settle() { app.step(clock.now_nanos()); }

    // The state a client is in when it is one of `readers` watching session 1.
    void in_a_session_with(int readers) {
        client.set_attached_session(1, "work");
        client.remember_sessions({SessionRow{1, "work", 2, readers}});
        settle();
    }

    bool can(std::string_view key) const {
        const ckv::ui::CommandId id =
            const_cast<Application&>(app).commands().id_for(key).value_or(ckv::ui::kInvalidCommand);
        return const_cast<Application&>(app).command_available(id);
    }
    bool run(std::string_view key) {
        const ckv::ui::CommandId id = app.commands().id_for(key).value_or(ckv::ui::kInvalidCommand);
        const bool ran = app.execute_command(id);
        settle();
        return ran;
    }
    bool footer_says(std::string_view needle) const {
        for (const std::string& label : const_cast<ClientApp&>(client).footer_labels())
            if (label.find(needle) != std::string::npos) return true;
        return false;
    }
    // Whether any toast on screen says this. Not `notifications()[0]`, which is
    // whichever one arrived first and stays there while later ones pile up
    // behind it — a case reading index 0 after two events is asserting about
    // the wrong one.
    bool toasted(std::string_view needle) const {
        auto* const centre = const_cast<ClientApp&>(client).notifications();
        if (centre == nullptr) return false;
        for (const auto& note : centre->notifications())
            if (note.text.find(needle) != std::string::npos) return true;
        return false;
    }
    // How many toasts are on screen, so a case can say "and this one is NEW"
    // rather than trusting an index. The centre has no clear(), and dismissing
    // by index while iterating is a trap, so the cases below mark instead of
    // sweep.
    std::size_t toast_count() const {
        auto* const centre = const_cast<ClientApp&>(client).notifications();
        return centre == nullptr ? 0U : centre->notifications().size();
    }
};

namespace cmd = ckm::client::commands;

}  // namespace

CK_TEST(the_reader_mode_items_are_offered_only_when_there_is_company) {
    // Greyed rather than hidden. A reader who has heard of "Take Session Over"
    // should find it in the menu and learn from its greyness that nobody else
    // is in here — an item that vanishes teaches nothing and reads as a build
    // that does not have the feature.
    Fixture f;
    f.in_a_session_with(1);
    CK_CHECK(!f.can(cmd::kTakeSessionOver));
    CK_CHECK(!f.can(cmd::kOthersReadOnly));
    // …except the self-restriction, which needs no company: a reader may
    // decide to only look at a session they are alone in.
    CK_CHECK(f.can(cmd::kWatchOnly));

    f.in_a_session_with(2);
    CK_CHECK(f.can(cmd::kTakeSessionOver));
    CK_CHECK(f.can(cmd::kOthersReadOnly));
    CK_CHECK(f.can(cmd::kWatchOnly));
}

CK_TEST(a_watcher_may_not_impose_a_mode_on_anybody_else) {
    // The client's half of the server's refusal. A watcher who could still
    // reach "Take Session Over" would send a request the server refuses, and
    // the reader would get an error for a menu item the client offered them.
    Fixture f;
    f.in_a_session_with(2);
    CK_CHECK(f.run(cmd::kWatchOnly));
    CK_CHECK(f.client.watching());
    CK_CHECK(!f.can(cmd::kTakeSessionOver));
    CK_CHECK(!f.can(cmd::kOthersReadOnly));
    // And out again, which is the one mode change a watcher may make.
    CK_CHECK(f.can(cmd::kWatchOnly));
}

CK_TEST(watch_only_asks_for_itself_and_is_believed_at_once) {
    Fixture f;
    f.in_a_session_with(1);

    CK_CHECK(f.run(cmd::kWatchOnly));
    CK_CHECK(f.asks.size() == 1);
    CK_CHECK(f.asks[0].scope == ReaderScope::Me);
    CK_CHECK(f.asks[0].mode == AttachMode::Watch);
    // Believed without waiting: the server sends no `ReaderMode` back for a
    // mode this reader asked for, so nothing else will ever tell this client
    // what it just did. A client that waited would grey nothing while being
    // refused everything.
    CK_CHECK(f.client.watching());
    CK_CHECK(f.footer_says("read-only"));

    CK_CHECK(f.run(cmd::kWatchOnly));
    CK_CHECK(f.asks.size() == 2);
    CK_CHECK(f.asks[1].mode == AttachMode::Join);
    CK_CHECK(!f.client.watching());
    CK_CHECK(!f.footer_says("read-only"));
}

CK_TEST(others_read_only_asks_for_them_and_lets_go_when_they_leave) {
    Fixture f;
    f.in_a_session_with(2);

    CK_CHECK(f.run(cmd::kOthersReadOnly));
    CK_CHECK(f.asks.size() == 1);
    CK_CHECK(f.asks[0].scope == ReaderScope::Others);
    CK_CHECK(f.asks[0].mode == AttachMode::Watch);
    // The reader who asked is unaffected — `Others` means others, and a client
    // that greyed itself here would be unable to undo what it had just done.
    CK_CHECK(!f.client.watching());

    // Handing it back is the same item again.
    CK_CHECK(f.run(cmd::kOthersReadOnly));
    CK_CHECK(f.asks.size() == 2);
    CK_CHECK(f.asks[1].mode == AttachMode::Join);

    // And when the company goes, the box goes with it: a tick describing
    // nobody is a state the reader would have to reason about to dismiss.
    CK_CHECK(f.run(cmd::kOthersReadOnly));
    CK_CHECK(f.asks.size() == 3);
    f.in_a_session_with(1);
    CK_CHECK(!f.can(cmd::kOthersReadOnly));
    // Re-entering company must not resurrect it: the readers it applied to are
    // gone, and the server has forgotten it too.
    f.in_a_session_with(2);
    CK_CHECK(f.run(cmd::kOthersReadOnly));
    CK_CHECK(f.asks.back().mode == AttachMode::Watch);
}

CK_TEST(taking_the_session_over_aims_at_the_others_and_never_at_oneself) {
    Fixture f;
    f.in_a_session_with(3);
    CK_CHECK(f.run(cmd::kTakeSessionOver));
    CK_CHECK(f.asks.size() == 1);
    CK_CHECK(f.asks[0].scope == ReaderScope::Others);
    CK_CHECK(f.asks[0].mode == AttachMode::TakeOver);
    // `{Me, TakeOver}` is refused at the wire, so a client that sent it would
    // be asking for an error. Stated here as well as at the server, because
    // this is the code that chooses the scope.
    for (const Asked& ask : f.asks)
        CK_CHECK(!(ask.scope == ReaderScope::Me && ask.mode == AttachMode::TakeOver));
}

CK_TEST(the_footer_says_who_else_is_here_and_whether_you_may_type) {
    // A toast expires and these facts do not. A reader who cannot see that
    // somebody else is typing into the terminal in front of them will blame
    // the program.
    Fixture f;
    f.in_a_session_with(1);
    CK_CHECK(!f.footer_says("readers"));
    CK_CHECK(!f.footer_says("read-only"));

    f.in_a_session_with(2);
    CK_CHECK(f.footer_says("2 readers"));

    f.in_a_session_with(4);
    CK_CHECK(f.footer_says("4 readers"));

    CK_CHECK(f.run(cmd::kWatchOnly));
    CK_CHECK(f.footer_says("read-only"));
    CK_CHECK(f.footer_says("4 readers"));
}

CK_TEST(a_reader_is_told_when_somebody_joins_and_when_they_are_alone_again) {
    Fixture f;
    f.in_a_session_with(1);

    f.in_a_session_with(2);
    CK_CHECK(f.toasted("Another reader joined this session"));

    f.in_a_session_with(1);
    CK_CHECK(f.toasted("You have this session to yourself again"));
}

CK_TEST(a_reader_is_told_when_somebody_else_changes_what_they_may_do) {
    // The asymmetry the server encodes and the client has to honour: a mode
    // this reader asked for needs no announcement — they just did it — and one
    // done TO them is the only thing that would otherwise be invisible.
    Fixture f;
    f.in_a_session_with(2);

    f.client.set_reader_mode(AttachMode::Watch, /*told=*/true);
    f.settle();
    CK_CHECK(f.toasted("Another reader made this session read-only for you"));
    CK_CHECK(f.client.watching());
    CK_CHECK(f.footer_says("read-only"));

    f.client.set_reader_mode(AttachMode::Join, /*told=*/true);
    f.settle();
    CK_CHECK(f.toasted("You can type in this session again"));
    CK_CHECK(!f.client.watching());
}

CK_TEST(a_mode_a_reader_chose_for_themselves_is_not_announced_back_to_them) {
    // The negative partner of the case above, and it is not a formality: the
    // two calls differ only in a bool, so a client that ignored it would pass
    // every assertion there and tell a reader what they had just told it.
    Fixture f;
    f.in_a_session_with(2);
    const std::size_t before = f.toast_count();
    f.client.set_reader_mode(AttachMode::Watch, /*told=*/false);
    f.settle();
    CK_CHECK(f.toast_count() == before);
    // …and it still took effect, so the silence is about the announcement
    // rather than about the mode.
    CK_CHECK(f.client.watching());
}
