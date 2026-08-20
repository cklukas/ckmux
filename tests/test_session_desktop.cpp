// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// A session's desktop is the session's (WP-40, plans/03 "Two clients at once").
//
// A terminal is a window on a virtual desktop, and a PTY's size is that
// window's content rect — so no client's screen appears anywhere in the chain
// between a reader's terminal and a child's `TIOCSWINSZ`. That is the property
// these cases pin, because it is what lets two readers of different sizes watch
// one session without either of them reflowing the other's windows or
// SIGWINCHing anybody's child.
//
// It also fixes something that was quietly useless: `Snapshot` and
// `LayoutDelta` have always carried `desktop_columns`/`desktop_rows`, and the
// server filled both from the client it was talking to — telling a client how
// big its own terminal is, which it already knew.
#if !defined(_WIN32)

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

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
    directory /= "ckmux-desktop" + std::to_string(static_cast<unsigned long>(::getpid()));
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

ckm::Settings test_settings(ckm::DesktopSizePolicy policy = ckm::DesktopSizePolicy::Fixed) {
    ckm::Settings settings;
    settings.shell = "/bin/sh";
    settings.login_shell = false;
    settings.scrollback = 100;
    settings.max_fps = 30;
    settings.desktop_size = policy;
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
    void attach(std::uint16_t columns, std::uint16_t rows) {
        ckm::proto::Attach request;
        request.session = 0;
        request.columns = columns;
        request.rows = rows;
        request.pixel_width = static_cast<std::uint16_t>(columns * 9);
        request.pixel_height = static_cast<std::uint16_t>(rows * 18);
        say(request);
    }
};

// Steps the server until this client has an `Attached`, and answers with the
// desktop the snapshot named — which is the whole observable of this package.
bool attached_desktop(ckm::server::Server& server, WireClient& client, ckv::Size& desktop,
                      int passes = 16) {
    for (int pass = 0; pass < passes; ++pass) {
        if (!server.step()) return false;
        Message message;
        while (client.take(message)) {
            const auto* attached = std::get_if<ckm::proto::Attached>(&message);
            if (attached == nullptr) continue;
            desktop = ckv::Size{attached->snapshot.desktop_columns,
                                attached->snapshot.desktop_rows};
            return true;
        }
    }
    return false;
}

}  // namespace

CK_TEST(the_first_client_to_attach_sets_the_sessions_desktop) {
    // A session made by `ckmux new` has no screen of its own to take a size
    // from, so the first reader to arrive supplies one. After that it is the
    // session's.
    const std::filesystem::path socket = private_socket("first");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient reader;
    CK_CHECK(reader.connect(socket));
    reader.greet();
    reader.attach(100, 30);

    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, reader, desktop));
    CK_CHECK(desktop.width == 100 && desktop.height == 30);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_second_client_of_another_size_does_not_move_the_desktop) {
    // The rule the whole package exists for. Under the default policy a
    // reader arriving on a laptop does not reflow the windows of a session
    // arranged on a large screen — and does not SIGWINCH a single child in it.
    const std::filesystem::path socket = private_socket("fixed");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(ckm::server::Server::Options{socket, test_settings()}, clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient wide;
    CK_CHECK(wide.connect(socket));
    wide.greet();
    wide.attach(200, 60);
    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, wide, desktop));
    CK_CHECK(desktop.width == 200 && desktop.height == 60);

    WireClient narrow;
    CK_CHECK(narrow.connect(socket));
    narrow.greet();
    narrow.attach(80, 24);
    ckv::Size after{0, 0};
    CK_CHECK(attached_desktop(server, narrow, after));
    // The newcomer is TOLD the session's desktop, not its own screen. What it
    // does about the difference is its business (WP-43); what it must not do
    // is change everybody else's coordinate space by arriving.
    CK_CHECK(after.width == 200 && after.height == 60);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(fit_smallest_shrinks_for_a_newcomer_and_stays_shrunk) {
    // Offered for readers arriving from tmux, and deliberately STICKY: it
    // shrinks to a newcomer and does not grow back when they leave. A session
    // whose geometry oscillates with people's attach cycles is worse than one
    // that is merely too small — every oscillation is a reflow and a SIGWINCH
    // storm through every child.
    const std::filesystem::path socket = private_socket("smallest");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(
        ckm::server::Server::Options{socket, test_settings(ckm::DesktopSizePolicy::FitSmallest)},
        clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient wide;
    CK_CHECK(wide.connect(socket));
    wide.greet();
    wide.attach(200, 60);
    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, wide, desktop));
    CK_CHECK(desktop.width == 200 && desktop.height == 60);

    {
        WireClient narrow;
        CK_CHECK(narrow.connect(socket));
        narrow.greet();
        narrow.attach(80, 24);
        ckv::Size shrunk{0, 0};
        CK_CHECK(attached_desktop(server, narrow, shrunk));
        CK_CHECK(shrunk.width == 80 && shrunk.height == 24);
    }  // and the small reader goes

    WireClient wide_again;
    CK_CHECK(wide_again.connect(socket));
    wide_again.greet();
    wide_again.attach(200, 60);
    ckv::Size after{0, 0};
    CK_CHECK(attached_desktop(server, wide_again, after));
    CK_CHECK(after.width == 80 && after.height == 24);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(fit_latest_takes_whoever_arrived_last) {
    const std::filesystem::path socket = private_socket("latest");
    forget(socket);
    ckv::ManualClock clock;
    ckm::server::Server server(
        ckm::server::Server::Options{socket, test_settings(ckm::DesktopSizePolicy::FitLatest)},
        clock);
    CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

    WireClient wide;
    CK_CHECK(wide.connect(socket));
    wide.greet();
    wide.attach(200, 60);
    ckv::Size desktop{0, 0};
    CK_CHECK(attached_desktop(server, wide, desktop));

    WireClient narrow;
    CK_CHECK(narrow.connect(socket));
    narrow.greet();
    narrow.attach(80, 24);
    ckv::Size after{0, 0};
    CK_CHECK(attached_desktop(server, narrow, after));
    CK_CHECK(after.width == 80 && after.height == 24);

    server.terminals().close_all();
    forget(socket);
}

CK_TEST(a_reader_resizing_their_own_window_changes_nothing_about_the_session) {
    // `ClientResize` says what it always meant: this client's screen, and the
    // cell metric a child's pixel fields derive from. It does not say anything
    // about the session's desktop, and under every policy — including the ones
    // that DO follow a client's size at attach — it must not move it, because a
    // reader dragging their own window corner has not asked to reflow anybody's
    // arrangement.
    for (const ckm::DesktopSizePolicy policy :
         {ckm::DesktopSizePolicy::Fixed, ckm::DesktopSizePolicy::FitSmallest,
          ckm::DesktopSizePolicy::FitLatest}) {
        const std::filesystem::path socket = private_socket("resize");
        forget(socket);
        ckv::ManualClock clock;
        ckm::server::Server server(ckm::server::Server::Options{socket, test_settings(policy)},
                                   clock);
        CK_CHECK(server.start() == ckm::server::Server::StartStatus::Listening);

        WireClient reader;
        CK_CHECK(reader.connect(socket));
        reader.greet();
        reader.attach(120, 40);
        ckv::Size desktop{0, 0};
        CK_CHECK(attached_desktop(server, reader, desktop));
        CK_CHECK(desktop.width == 120 && desktop.height == 40);

        ckm::proto::ClientResize resize;
        resize.columns = 60;
        resize.rows = 20;
        resize.pixel_width = 60 * 9;
        resize.pixel_height = 20 * 18;
        reader.say(resize);
        for (int pass = 0; pass < 6; ++pass) (void)server.step();

        // Re-attaching is the only thing that reads a client's size, so ask
        // again the only way a test can: the session still says what it said.
        WireClient second;
        CK_CHECK(second.connect(socket));
        second.greet();
        second.attach(120, 40);
        ckv::Size after{0, 0};
        CK_CHECK(attached_desktop(server, second, after));
        CK_CHECK(after.width == 120 && after.height == 40);

        server.terminals().close_all();
        forget(socket);
    }
}

#endif  // !defined(_WIN32)
