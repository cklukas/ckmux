// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/run_client.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "client/server_connection.hpp"
#include "client/server_session.hpp"
#include "client/capability_grace.hpp"
#include "platform/paths.hpp"
#include "cvision/core/assert.hpp"
#include "cvision/core/clock.hpp"
#include "cvision/term/terminal.hpp"
#include "cvision/ui/application.hpp"

namespace ckm::client {
namespace {

// The application's own sources, plus the socket. ckVision hands out its
// descriptors precisely so a host can wait on them together with its own
// (D-021); this is that pattern with one extra fd.
int wait_for_work(ckv::ui::Application& app, int socket_fd, bool want_write, int timeout_ms) {
    std::vector<pollfd> waiting;
    for (const ckv::term::WaitHandle& handle : app.wait_handles())
        if (handle.kind == ckv::term::WaitHandleKind::PosixFileDescriptor)
            waiting.push_back(pollfd{static_cast<int>(handle.value), POLLIN, 0});
    if (socket_fd >= 0)
        waiting.push_back(
            pollfd{socket_fd, static_cast<short>(POLLIN | (want_write ? POLLOUT : 0)), 0});
    if (waiting.empty()) return 0;
    return ::poll(waiting.data(), static_cast<nfds_t>(waiting.size()), timeout_ms);
}

// One of a tile fraction's four numbers, wire to window layer (WP-30). The wire
// counts ten-thousandths of a content area because every other number on it is
// an integer; the window layer counts fractions of one because that is what
// ckVision's own `Desktop::TileFraction` answers in. This function is the whole
// of the difference, and it lives here for the same reason the id translation
// below it does: this file is the seam between the two vocabularies.
double tile_share(std::uint16_t fixed) {
    return static_cast<double>(fixed) / static_cast<double>(proto::kTileFractionWhole);
}

// The same conversion the other way. Written as `!(share > 0.0)` rather than
// `share <= 0.0` so that a NaN — which compares false against everything — is
// answered with "no share" instead of being handed to `llround`.
std::uint16_t tile_fixed(double share) {
    if (!(share > 0.0)) return 0;
    const long long scaled =
        std::llround(share * static_cast<double>(proto::kTileFractionWhole));
    return static_cast<std::uint16_t>(std::clamp<long long>(scaled, 0, proto::kTileFractionWhole));
}

}  // namespace

int run_attached_client(ckv::term::Terminal& host, ckv::Clock& clock, RunOptions options) {
    ServerConnection connection =
        connect_to_server(options.socket, options.executable, proto::ClientKind::Ui);
    if (!connection.ok()) {
        std::fprintf(stderr, "ckmux: %s\n", connection.problem.c_str());
        return 1;
    }

    // The session is declared BEFORE the application, and the order is the
    // point. `Application` owns the desktop, its windows and their
    // `TerminalView`s; the session owns the mirrors those views borrow, and a
    // view borrows its subsession for as long as it exists (ckVision's
    // lifetime rule). Locals are destroyed in reverse, so with the application
    // first the mirrors were freed and the views were then destroyed reading
    // them. Declaring the session first makes the end of this function
    // provably safe rather than accidentally so.
    ServerSession session([&connection](const proto::Message& message) {
        (void)connection.stream.send(proto::encode(message));
    });
    // `ckmux attach --share` (WP-44), set before anything can attach. It rides
    // every `Attach` rather than only the first: a heal, a session switch and a
    // reconnection all re-attach, and one that dropped the flag would take over
    // the session this reader chose to share — at the moment they are least
    // able to tell why it happened.
    session.set_share(options.share_session);
    ckv::ui::Application app(host, clock);
    // Pace against the reader's own terminal (ckVision's architecture §4,
    // docs/graphics.md "Knowing the terminal took it"). Writing a frame proves
    // its bytes left this process and nothing more; a host that accepts Sixel
    // faster than it DRAWS it queues the difference, and a client that keeps
    // composing regardless is producing frames nobody will ever see. What the
    // reader sees then is a picture running further behind every frame — a
    // child's animation still turning where its window was seconds ago, and
    // going on after the child that drew it has exited.
    //
    // A multiplexer is exactly the application that cannot assume a modest
    // frame rate: what it relays is whatever the child draws, and a child may
    // draw a full-screen picture as fast as its own terminal will take one
    // (ckvision_spin does, and ckmux's emulator takes them at memory speed).
    // So the loop is closed here rather than by guessing a rate: each frame
    // carries a Device Status Report, the reply says that frame was taken in,
    // and ckVision holds the next picture back until it comes. A host that
    // never answers is not waited for — tracking turns itself off — and a
    // frame with no picture in it is never held back at all, so a terminal
    // showing only text is paced exactly as it was before.
    app.set_frame_completion_tracking(true);
    // What this display can actually show, carried on every Attach: the
    // server folds it into the graphics advertisement the children this
    // client opens are given (WP-16). The evidence a capability probe
    // resolves to — Sixel's DA1 parameter 4 chief among them — arrives
    // asynchronously, and this application (and the terminal it wraps) exist
    // before any reply has had a chance to land. Reading `capabilities()`
    // only here, once, read whatever was true before the probe answered:
    // every terminal this client ever opened advertised no Sixel support to
    // its child, regardless of what the real host offered (field report,
    // 2026-08-18 — cksetup's own Terminal Report showed DA1 without
    // parameter 4 on a host that has it). The handler keeps this live for
    // every terminal opened from here on; this first call is only the
    // fallback for whatever opens before the first reply does.
    session.set_host_sixel(host.capabilities().sixel_graphics);
    // Whether the host has answered the capability probe at all yet. The
    // fallback above is a guess made before the evidence exists, and the FIRST
    // terminal — the one a reader actually lands in — was being opened against
    // that guess, so on a Sixel-capable host the terminal every reader uses
    // advertised no Sixel and every picture drawn into it was dropped. Field
    // report, 2026-08-20: the spin demo reporting no Sixel on a host that has
    // it. Terminals opened later were already correct, which is exactly what
    // made this survive: the bug was invisible to anyone who opened a second
    // terminal before looking.
    bool host_caps_answered = false;
    app.set_capability_changed_handler([&session, &host, &host_caps_answered] {
        session.set_host_sixel(host.capabilities().sixel_graphics);
        host_caps_answered = true;
    });
    // Where the bytes are reassembled into frames. Declared here rather than
    // beside the loop because a reconnection has to start it over: half a frame
    // from a server that has gone is not the beginning of one from its
    // successor.
    proto::FrameReader reader;
    // The client, once it exists. Declared here because the teardown below has
    // to reach both halves of what this loop holds.
    ClientApp* client = nullptr;
    // Everything this client was showing of a server that is no longer there.
    //
    // Windows first, then mirrors, and never the other way round: a
    // `TerminalView` borrows the subsession it draws, so dropping the mirrors
    // first leaves every view on the desktop pointing at freed memory — and
    // the next repaint, or the window's own destructor, reads it.
    // `connection_lost()` also forgets which terminals the window layer was
    // told about, which is right here: there are none left to tell it about.
    const auto stop_showing_this_server = [&client, &session]() {
        if (client != nullptr) {
            client->forget_terminals();
            client->set_attached_session(0, {});
        }
        session.connection_lost();
    };
    // A server ends with its last session (the session model), so losing the connection
    // is an ordinary thing that happens to a client whose reader ended their
    // work — not a failure. The next request that needs a server starts one,
    // exactly as the first one did.
    const auto ensure_connected = [&connection, &reader, &options,
                                   &stop_showing_this_server]() -> bool {
        if (connection.stream.open()) return true;
        ServerConnection fresh =
            connect_to_server(options.socket, options.executable, proto::ClientKind::Ui);
        if (!fresh.ok()) return false;
        connection = std::move(fresh);
        reader = proto::FrameReader{};
        // Nothing this client held survives: the mirrors were of terminals in
        // another process, and the attachment was to a session that server had.
        stop_showing_this_server();
        return true;
    };
    session.set_history_limit(static_cast<std::size_t>(options.client.settings.scrollback));
    // What the VIEWS over this server's terminals may draw, from the reader's
    // own configuration (the configuration spec). The child's advertisement is the server's
    // to decide — it runs the emulator, and `Attach.host_sixel` is this
    // client's half of that — but the profile a `TerminalView` reads is the
    // client's, and left at the built-in default a reader's `[terminal]`
    // settings reached the server and never the window they were looking at.
    {
        ckv::term::TerminalCapabilityProfile profile = ckv::term::embedded_xterm_sixel_profile();
        profile.osc_policy = ckv::core::TerminalOscPolicy::StoreMetadata;
        profile.mouse_reporting = options.client.settings.mouse;
        profile.alternate_scroll = options.client.settings.alternate_scroll;
        profile.sixel = options.client.settings.sixel == SixelMode::Auto;
        profile.clipboard_policy = options.client.settings.osc52
                                       ? ckv::core::TerminalClipboardPolicy::AllowWrite
                                       : ckv::core::TerminalClipboardPolicy::Deny;
        session.set_profile(std::move(profile));
    }

    // Windows follow the server's terminals. A window opens when a terminal is
    // announced — which covers both a reader asking for a new one and every
    // terminal that was already running when this client attached, and is why a
    // reattach draws what is there rather than starting something else.
    //
    // The mirrors are owned by the session rather than adopted into the
    // application: a mirror has no descriptors to wait on and nothing to drain,
    // so adoption would buy nothing and would split the ownership of the very
    // thing the routing looks up by id.
    std::vector<RemoteTerminalSubsession*> pending;
    options.client.terminal_source =
        [&pending](TerminalRequest request) -> ckv::term::TerminalSubsession& {
        // The launch spec is composed and then dropped, and that is the honest
        // answer for a terminal that is not launched here: what a child runs,
        // where, and what it is told it can draw are the SERVER's, because the
        // server is the process that holds the child (the architecture spec). The one part
        // of it a client keeps — the capability profile its views read — is
        // set once on the session above, not per window.
        (void)request;
        // Never empty by construction: the one caller that opens a window for
        // a remote terminal — `on_terminal_opened` below, which is also where
        // a reattach's reconciliation arrives — pushes the subsession the
        // window is FOR immediately before asking for the window. An empty
        // queue would mean a window opened with no terminal behind it, which
        // is a routing mistake worth finding rather than a reference worth
        // inventing.
        CKV_ASSERT(!pending.empty());
        RemoteTerminalSubsession* ready = pending.front();
        pending.erase(pending.begin());
        return *ready;
    };
    // What a new terminal runs. Copied out of the options rather than read
    // through them, because they are MOVED into the client below: every read
    // after that point is a read of a moved-from object, and this one quietly
    // became "" — so the server picked the shell and a reader's `[general]
    // shell` did nothing at all. Filled in once the client has resolved it.
    std::string shell_for_new_terminals;
    options.client.request_new_terminal = [&session, &shell_for_new_terminals]() {
        proto::NewTerminal ask;
        ask.command = shell_for_new_terminals;
        ask.host_sixel = session.host_sixel() ? 1 : 0;
        session.request(ask);
        return true;
    };
    // Detaching, and quitting, are the same act for an attached client: say so
    // to the server, and stop drawing. The programs go on running — which is
    // the whole point, and why quitting must not ask a reader whether to close
    // anything.
    bool detached = false;
    options.client.detach_from_server = [&session, &app, &detached]() {
        detached = true;
        session.request(proto::Detach{});
        app.request_quit();
    };
    // Asking is not being told: the list arrives later, and only a list this
    // client asked for on a READER's behalf opens the picker. The others — the
    // one after every attach, the one after a rename — keep the names current
    // and put nothing in anybody's way.
    bool show_picker_when_the_list_arrives = false;
    options.client.list_sessions = [&session, &ensure_connected, &show_picker_when_the_list_arrives]() {
        show_picker_when_the_list_arrives = true;
        if (!ensure_connected()) return;
        session.request(proto::ListSessions{});
    };
    // Switching sessions is a detach and an attach — and everything on the
    // screen belongs to the session being left, so it goes. A client that kept
    // its windows would be showing a reader the terminals of a session it is no
    // longer watching, each one frozen at the last frame it saw.
    // One way to start watching a session, wherever the asking came from.
    //
    // The windows go first, because they belong to the session being left. The
    // server needs no `Detach`: an `Attach` to another session IS the switch,
    // and it leaves the one being left with its programs running and nobody
    // watching — which is exactly the state ckmux exists to keep (the session model).
    const auto switch_to = [&session, &client, &host, &ensure_connected](std::uint64_t id) {
        if (session.attached() && session.session() == id) return;
        if (!ensure_connected()) return;
        if (client != nullptr) {
            client->forget_terminals();
            // The mirrors live on — this is a client leaving a session, not a
            // connection going — so the session has to be told that what it
            // announced is no longer on screen. Without it, coming back to a
            // session whose mirrors are still held announces nothing and the
            // reader gets an empty desktop over running programs (C4).
            session.windows_forgotten();
        }
        session.attach(id, host.size(), host.capabilities().cell_pixels);
    };
    options.client.attach_to_session = switch_to;
    // Creating a session means going to it: a reader who typed a name and
    // pressed Create was not asking for a row in a list.
    bool waiting_for_a_new_session = false;
    options.client.create_session = [&session, &ensure_connected,
                                     &waiting_for_a_new_session](const std::string& name) {
        // The one place a server is started rather than found: a reader asking
        // for a session after the last one ended is asking for a server too,
        // and they should not have to know that.
        if (!ensure_connected()) return;
        proto::NewSession ask;
        ask.name = name;
        waiting_for_a_new_session = true;
        session.request(ask);
    };
    options.client.rename_session = [&session](const std::string& name) {
        proto::RenameSession ask;
        ask.id = session.session();
        ask.name = name;
        session.request(ask);
    };
    options.client.kill_session = [&session](bool force, int grace_seconds) {
        proto::KillSession ask;
        ask.session = session.session();
        ask.force = force ? 1 : 0;
        ask.grace_seconds = static_cast<std::uint16_t>(std::max(0, grace_seconds));
        session.request(ask);
    };
    // The close dialog's promise, carried to the server: asked first, killed
    // after the grace only when the reader ticked it. The window stays up —
    // it goes when the TermClosed comes back, because only the server knows
    // when (or whether) the program actually ended.
    options.client.close_terminal_in_session = [&session](ckv::term::TerminalSubsession& terminal,
                                                          bool force, int grace_seconds) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return;
        proto::CloseTerminal ask;
        ask.term = *id;
        ask.force = force ? 1 : 0;
        ask.grace_seconds = static_cast<std::uint16_t>(std::max(0, grace_seconds));
        session.request(ask);
    };
    // The session model's `kill-terminal`, carried to the server. No force and no grace
    // because the operation has neither: the whole of it is SIGKILL now.
    options.client.kill_terminal_in_session = [&session](ckv::term::TerminalSubsession& terminal) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return;
        proto::KillTerminal ask;
        ask.term = *id;
        session.request(ask);
    };
    // Naming a terminal, and handing the name back — one seam for both,
    // because an empty name IS the request to use the default title again.
    // Sent rather than applied here: a custom title is session state (the session model)
    // and the server is what makes it survive a detach and reach the other
    // client watching the same session. What comes back is the answer.
    options.client.rename_terminal = [&session](ckv::term::TerminalSubsession& terminal,
                                                const std::string& name) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return;
        proto::RenameTerminal ask;
        ask.id = *id;
        ask.name = name;
        session.request(ask);
    };
    // And the answer, read back off the mirror the server states it into. A
    // query on the client's own title poll rather than a push, because the
    // caption is assembled from this and the child's title together and two
    // readings taken a tick apart could disagree.
    options.client.custom_title = [&session](const ckv::term::TerminalSubsession& terminal) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return std::string{};
        const RemoteTerminalSubsession* const remote = session.terminal(*id);
        return remote == nullptr ? std::string{} : remote->mirror().custom_title();
    };
    // What the child exited with, for the frame's `[exit N]` badge (WP-13).
    // Read off the mirror, which learned it from `TermClosed` and keeps it —
    // the seam carries `state()` but no number, so without this the badge
    // could only say that something ended, not what.
    options.client.exit_status =
        [&session](const ckv::term::TerminalSubsession& terminal) -> std::optional<int> {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return std::nullopt;
        const RemoteTerminalSubsession* const remote = session.terminal(*id);
        return remote == nullptr ? std::nullopt : remote->mirror().exit_status();
    };
    // A paste goes out credit-paced (WP-18) — the session owns the queue,
    // because `PasteAck` carries a seq and nothing else, so the numbering has
    // to be per connection rather than per terminal. False for anything that
    // is not one of this session's mirrors, and the client then writes into
    // the subsession directly, which is what a serverless ckmux always does.
    options.client.paste_credited = [&session](const ckv::term::TerminalSubsession& terminal,
                                               std::string bytes) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return false;
        session.paste(*id, std::move(bytes));
        return true;
    };
    // What each terminal has been doing while the reader was elsewhere
    // (WP-19). Straight off the mirror, which decodes both flags from a
    // snapshot and from every `TermMeta`.
    options.client.terminal_marks =
        [&session](const ckv::term::TerminalSubsession& terminal) -> ckm::client::TerminalMarks {
        ckm::client::TerminalMarks marks;
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return marks;
        const RemoteTerminalSubsession* const remote = session.terminal(*id);
        if (remote == nullptr) return marks;
        marks.bell = remote->mirror().bell_marked();
        marks.activity = remote->mirror().activity_marked();
        // And the counts, which are what the client decides on: the level says
        // a mark is up, the serial says how many times — and only the second
        // can tell a reader that a terminal they visited has rung since.
        marks.bell_serial = remote->mirror().bell_serial();
        marks.activity_serial = remote->mirror().activity_serial();
        return marks;
    };
    // Why the last save failed, in the system's own words, so the message the
    // reader is shown names the cause rather than only the fact. Lives out
    // here because the write and the explanation are two hooks and have to
    // agree about one attempt.
    std::string save_problem;
    // --- The virtual printer's client half (PRINT-3…6) --------------------
    //
    // `ClientApp` takes every printer fact as an injected hook so that a test
    // can drive it without a filesystem or a server. Those hooks were declared
    // and used when the printer landed; NOTHING assigned them here, and the
    // effect was not a crash but something quieter and worse: the frame button
    // counts jobs from the emulator's own scalars (the M1 fallback), while the
    // Print Output dialog — the reader's only route to a captured document, and
    // the only place View / Save lives — has no fallback and so reported
    // "Nothing has been captured from this terminal" while the button beside it
    // said two jobs were held. Both readings were on one screen at one moment.
    // A reader could not open, view or save a single job.
    //
    // Everything needed was already in the mirror. What was missing was this.
    options.client.printer_status =
        [&session](const ckv::term::TerminalSubsession& terminal) -> ckm::client::PrinterButtonModel {
        ckm::client::PrinterButtonModel model;
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return model;
        const RemoteTerminalSubsession* const remote = session.terminal(*id);
        if (remote == nullptr) return model;
        model.mode = remote->mirror().printer_mode();
        model.state = remote->mirror().printer_state();
        model.pending_bytes = remote->mirror().printer_bytes();
        model.jobs = remote->mirror().printer_jobs();
        // `answered` stays the client's own: it is a fact about what THIS
        // reader has been shown, and a second client attaching has been shown
        // nothing (client_app.hpp).
        return model;
    };
    options.client.printer_jobs =
        [&session](const ckv::term::TerminalSubsession& terminal) -> std::vector<proto::PrintJobInfo> {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return {};
        const RemoteTerminalSubsession* const remote = session.terminal(*id);
        if (remote == nullptr) return {};
        const std::span<const proto::PrintJobInfo> jobs = remote->mirror().print_jobs();
        return std::vector<proto::PrintJobInfo>(jobs.begin(), jobs.end());
    };
    options.client.print_job_text =
        [&session](const ckv::term::TerminalSubsession& terminal,
                   std::uint64_t job) -> const std::string* {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return nullptr;
        const RemoteTerminalSubsession* const remote = session.terminal(*id);
        if (remote == nullptr) return nullptr;
        // Null until a fetch has completed, which is what the preview shows as
        // "fetching" rather than as an empty document.
        return remote->mirror().print_job_text(job);
    };
    options.client.fetch_print_job = [&session](ckv::term::TerminalSubsession& terminal,
                                                std::uint64_t job) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return;
        proto::PrintJobFetch ask;
        ask.term = *id;
        ask.job = job;
        session.request(ask);
    };
    options.client.discard_print_job = [&session](ckv::term::TerminalSubsession& terminal,
                                                  std::uint64_t job) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return;
        proto::PrintJobDiscard ask;
        ask.term = *id;
        // Zero means every job this terminal holds, which is what the frame
        // button's Discard means when nothing in particular is selected.
        ask.job = job;
        session.request(ask);
    };
    // The one hook that genuinely touches the filesystem, which is exactly why
    // it is injected rather than called directly from the client.
    //
    // `printer_save_problem` carries the reason across, because a save that
    // failed silently is the worst of the four outcomes: the reader believes
    // they have the document and finds out when they need it.
    options.client.write_print_file = [&save_problem](const std::string& path,
                                                      std::string_view bytes) {
        save_problem.clear();
        std::error_code failed;
        // `~` is APPLIED here, at the filesystem boundary — the first place a
        // path stops being something a reader typed and becomes something the
        // kernel is handed — but what it MEANS is decided in
        // `platform/paths`, which is the one place that answers that and the
        // only place with a suite saying so (`tests/test_paths.cpp`).
        //
        // It was decided here once, inline, and the residue is why it moved:
        // a relative `HOME` produced a path resolved against whatever
        // directory the client was started in, and an unset `HOME` left the
        // `~` in the string and reproduced the original `<cwd>/~/Documents/`
        // bug exactly. A guard whose failure mode is the defect it guards
        // against is not one.
        const std::filesystem::path target = ckm::platform::expand_user_path(path);
        const std::string resolved = target.string();
        if (target.has_parent_path()) {
            std::filesystem::create_directories(target.parent_path(), failed);
            failed.clear();
        }
        // Exclusive create: a save must never quietly overwrite a document the
        // reader already has. The dialog is what asks about replacing.
        const int fd = ::open(resolved.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            save_problem = std::strerror(errno);
            return false;
        }
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ::ssize_t wrote = ::write(fd, bytes.data() + written, bytes.size() - written);
            if (wrote <= 0) {
                if (errno == EINTR) continue;
                save_problem = std::strerror(errno);
                (void)::close(fd);
                return false;
            }
            written += static_cast<std::size_t>(wrote);
        }
        if (::close(fd) != 0) {
            save_problem = std::strerror(errno);
            return false;
        }
        return true;
    };
    options.client.printer_save_problem = [&save_problem]() { return save_problem; };

    // And the host's own bell, when the reader asked for one.
    options.client.ring_host_bell = [&host] { host.bell(); };
    // The one message that changes a session's coordinate space (WP-40). Sent
    // only when a reader asks for it; `ClientResize`, which goes out whenever
    // this client's own window changes size, deliberately does not.
    options.client.fit_session_desktop = [&session](ckv::Size desktop) {
        proto::SetDesktopSize fit;
        fit.columns = static_cast<std::uint16_t>(std::max(0, desktop.width));
        fit.rows = static_cast<std::uint16_t>(std::max(0, desktop.height));
        session.request(fit);
    };
    // `Enter restart` on a held window: the same command, in the same window,
    // which is the server's to do because the server owns the child. Set only
    // here — a client with no server leaves it unset, and the footer stops
    // offering the key rather than offering one that does nothing.
    options.client.request_respawn = [&session](const ckv::term::TerminalSubsession& terminal) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return;
        proto::RespawnTerminal ask;
        ask.term = *id;
        session.request(ask);
    };
    options.client.move_terminal = [&session](ckv::term::TerminalSubsession& terminal,
                                              std::uint64_t destination, bool to_new_session) {
        const std::optional<std::uint64_t> id = session.id_of(terminal);
        if (!id) return;
        proto::MoveTerminal ask;
        ask.term = *id;
        ask.destination_session = destination;
        ask.to_new_session = to_new_session ? 1 : 0;
        session.request(ask);
    };
    // Where the reader's windows are (WP-29). This is the translation the whole
    // arrangement has to pass through, and the reason it lives here rather than
    // at either end: the window layer knows windows and the mirrors they show,
    // the session layer knows wire ids, and this function is the one place that
    // has both. The client has already decided WHEN to report — it debounces a
    // drag into one settled arrangement — and the session decides whether it
    // may be sent at all; between them there is only this renaming.
    options.client.report_layout = [&session](const std::vector<WindowPlacement>& arrangement) {
        std::vector<proto::LayoutEntry> entries;
        entries.reserve(arrangement.size());
        for (const WindowPlacement& placed : arrangement) {
            if (placed.terminal == nullptr) continue;
            // A window whose terminal this session does not know is a window
            // whose terminal has just gone — a close racing the settle timer.
            // Left out rather than reported as some other id: the entry that
            // vanishes IS how the wire says a window closed, and the z-order
            // gap it leaves behind means nothing, because a stack position is
            // only ever compared with the others in the same message.
            const std::optional<std::uint64_t> id = session.id_of(*placed.terminal);
            if (!id) continue;
            // The tile share travels with the rect rather than instead of it
            // (WP-30). Both, because they answer different questions and the
            // far end decides which one applies: the rect is where the window
            // was on THIS desktop, and the share is what fraction of any
            // desktop it should have. A window that was floating carries a zero
            // share, which is how the wire says "there was no tiling".
            entries.push_back(proto::LayoutEntry{
                *id, proto::to_wire(placed.rect), placed.z_order,
                static_cast<std::uint8_t>(placed.zoomed ? 1 : 0),
                proto::TileFraction{tile_fixed(placed.tile.x), tile_fixed(placed.tile.y),
                                    tile_fixed(placed.tile.width),
                                    tile_fixed(placed.tile.height)}});
        }
        session.report_layout(std::move(entries));
    };
    // The session usually has terminals already — that is what attaching to one
    // MEANS. One is asked for only if it turns out to be empty, once the
    // snapshot has said so.
    options.client.open_terminal_at_startup = false;

    // The View readouts' server half (WP-39): the client says whether anybody
    // is watching, the session owns the standing wish and restates it on
    // every Attached — because the server's subscription is per connection
    // and this client reconnects.
    options.client.watch_stats = [&session](bool on) { session.set_stats_watched(on); };

    ClientApp application(app, std::move(options.client));
    client = &application;
    // Asked of the client rather than of the options it was given: an empty
    // `[general] shell` is resolved in its constructor, so this is the one
    // place the answer is complete.
    shell_for_new_terminals = application.settings().shell;

    // What to do with a session list depends on when it arrives. The first one
    // is the startup decision — attach, or ask — and every one after it is a
    // reader having asked what is running.
    //
    //   * Nothing running: start a session and attach to it. A reader who ran
    //     `ckmux` wants a terminal, not a dialog about how to get one.
    //   * One session, nobody watching it: attach. This is the reattach that
    //     the whole project is for, and it needs no permission.
    //   * Anything else — a session already in use, or several to choose from —
    //     is a genuine choice, and only a reader can make it. The picker offers
    //     taking it over, a new one, or carrying on with no session at all.
    bool startup_decided = false;
    const std::uint64_t preselected = options.preselected_session;
    session.on_sessions = [&client, &session, &switch_to, &startup_decided, &waiting_for_a_new_session,
                           &show_picker_when_the_list_arrives, preselected](
                              const std::vector<proto::SessionInfo>& sessions) {
        if (client == nullptr) return;
        std::vector<SessionRow> rows;
        rows.reserve(sessions.size());
        for (const proto::SessionInfo& info : sessions)
            rows.push_back(SessionRow{info.id, info.name, info.terminals, info.attached != 0});

        // A session this client asked for, now that it exists. The newest is
        // the one it asked for: ids only ever go up.
        if (waiting_for_a_new_session && !rows.empty()) {
            waiting_for_a_new_session = false;
            startup_decided = true;
            std::uint64_t newest = 0;
            for (const SessionRow& row : rows) newest = std::max(newest, row.id);
            // The list is what names sessions, so it is remembered before the
            // attach that will ask for the name of the one just made.
            client->remember_sessions(rows);
            switch_to(newest);
            return;
        }
        if (!startup_decided) {
            startup_decided = true;
            // `ckmux attach <name>` bypasses the picker (the interface spec). The id
            // was resolved against this same list before the window opened, so
            // the only way it is missing here is a session that ended in
            // between — in which case this falls through to the ordinary
            // startup rather than attaching to something else.
            if (preselected != 0) {
                const auto found = std::find_if(
                    rows.begin(), rows.end(),
                    [preselected](const SessionRow& row) { return row.id == preselected; });
                if (found != rows.end()) {
                    client->remember_sessions(rows);
                    switch_to(preselected);
                    return;
                }
            }
            if (rows.empty()) {
                waiting_for_a_new_session = true;
                startup_decided = false;
                proto::NewSession ask;
                session.request(ask);
                return;
            }
            if (rows.size() == 1 && !rows.front().attached) {
                client->remember_sessions(rows);
                switch_to(rows.front().id);
                return;
            }
            show_picker_when_the_list_arrives = false;
            client->show_session_picker(std::move(rows));
            return;
        }
        if (!show_picker_when_the_list_arrives) {
            client->remember_sessions(std::move(rows));
            return;
        }
        show_picker_when_the_list_arrives = false;
        client->show_session_picker(std::move(rows));
    };

    // A request the server could not honour, in front of the reader who made
    // it. The client already sent one of eight `Error`s back for every
    // impossible request; what was missing was anybody to show it.
    // `ckmux attach --adopt-size` (WP-40) is spent the first time the server
    // states its desktop, which is the earliest moment there is anything to
    // answer. Once, and then never again: the flag is a request made at the
    // door, not a standing claim. A client that re-asserted its size would
    // fight the next reader who asked for theirs, and the session would end up
    // belonging to whoever's terminal resized last — exactly what WP-40 took
    // away from `ClientResize` in the first place.
    bool size_still_to_adopt = options.adopt_session_size;
    session.on_session_desktop = [&client, size_still_to_adopt](ckv::Size world) mutable {
        // The world, into the desktop's extent (WP-43). The session layer
        // knows what the server said; the client knows what a desktop is;
        // this file is the one place that has both, as ever.
        if (client == nullptr) return;
        client->set_session_desktop(world);
        if (!size_still_to_adopt) return;
        size_still_to_adopt = false;
        // Through the client's own command rather than a second
        // `SetDesktopSize` built here, so that what "this screen" means is
        // decided in one place and the reader who typed the flag is told it
        // took, exactly as the reader who picked the menu item is.
        client->fit_desktop_to_screen();
    };
    session.on_stats = [&client](RemoteTerminalSubsession& remote,
                                 const proto::TermStats& stats) {
        // Onto that terminal's window frame, formatted per the View toggles.
        // The client cannot tell these numbers from its own local sampler's,
        // which is the seam discipline (the work queue WP-39).
        if (client != nullptr) client->receive_terminal_stats(remote, stats);
    };
    session.on_error = [&client](const proto::Error& error) {
        if (client == nullptr) return;
        client->show_server_error(error.code, error.context, error.human);
    };

    // A terminal the server has ended takes its window with it — including
    // every terminal of a session this client has just left, which is how a
    // switch clears what it was showing.
    session.on_terminal_closed = [&session, &client](std::uint64_t id) {
        if (client == nullptr) return;
        if (RemoteTerminalSubsession* remote = session.terminal(id))
            client->close_window_for_terminal(*remote);
    };

    session.on_terminal_opened = [&pending, &client](RemoteTerminalSubsession& remote) {
        pending.push_back(&remote);
        // The window, now that there is something behind it. `open_terminal` is
        // the same call M1 makes; what differs is only when it happens.
        // Empty: the client numbers and places it, exactly as it does for a
        // local one. The child's own caption replaces the name as soon as it
        // sets one.
        if (client != nullptr) (void)client->open_terminal({});
    };

    // And where the reader left those windows (WP-30) — the same translation
    // `report_layout` above does, run the other way, and here for the same
    // reason: the session layer knows wire ids and the window layer knows
    // windows, so a terminal is named by the mirror it is showing on the way in
    // exactly as it is on the way out.
    session.on_layout = [&session, &client](const std::vector<proto::LayoutEntry>& entries,
                                            ServerSession::LayoutStatement statement) {
        if (client == nullptr) return;
        // Only a snapshot is a reattach, and only a reattach lays windows out.
        //
        // A `Delta` is the server's record moving while the reader watches, and
        // the reader is the one who moved it — so there is nothing in one to
        // restore, and acting on it can only ever take a window, a raise or the
        // keyboard away from somebody mid-gesture. That is not a theoretical
        // risk: it buried the close-terminal dialog behind the very terminal it
        // was asking about, and swallowed the keystrokes that open the Terminal
        // Report, because a terminal opened after the snapshot has its place
        // first stated by a delta a fraction of a second into the session.
        if (statement != ServerSession::LayoutStatement::Snapshot) return;
        std::vector<WindowPlacement> arrangement;
        arrangement.reserve(entries.size());
        for (const proto::LayoutEntry& entry : entries) {
            // A place for a terminal this client holds no mirror for is a place
            // for a window that does not exist — a terminal that closed while
            // the statement was in flight, or one this client has not been
            // announced yet. Left out rather than guessed at; the window layer
            // leaves such a terminal unplaced, so a later statement still counts.
            const ckv::term::TerminalSubsession* const terminal = session.terminal(entry.term);
            if (terminal == nullptr) continue;
            arrangement.push_back(WindowPlacement{
                terminal, proto::from_wire(entry.rect), entry.z_order, entry.zoomed != 0,
                TileShare{tile_share(entry.tile.x), tile_share(entry.tile.y),
                          tile_share(entry.tile.width), tile_share(entry.tile.height)}});
        }
        client->apply_layout(arrangement);
    };

    // An empty session gets a terminal; a session with programs in it gets left
    // alone. Checked once, after the first snapshot, because that is the first
    // moment the answer exists.
    bool asked_for_the_first_terminal = false;
    // When this client first had a session to open a terminal in. The wait
    // below is bounded from here rather than from process start, so a slow
    // server does not spend the reader's budget before the question is even
    // askable.
    std::int64_t could_have_opened_since = 0;
    const auto open_one_if_the_session_is_empty = [&session, &asked_for_the_first_terminal,
                                                   &shell_for_new_terminals, &host_caps_answered,
                                                   &could_have_opened_since, &clock]() {
        if (asked_for_the_first_terminal || !session.attached()) return;
        // The first terminal waits for the host to say what it can draw.
        //
        // `host_sixel` is decided once, when the terminal is created: the
        // server folds it into the advertisement its child reads out of DA1,
        // and a program that has already asked never asks again. Opening
        // before the probe has answered therefore does not merely guess — it
        // commits the guess for the lifetime of the terminal a reader spends
        // all their time in.
        //
        // Bounded, because a host that never answers must not cost a reader
        // their terminal. A host that supports the probe replies in microseconds
        // (it is a round trip to a program already running); a host that does
        // not, never replies at all, and after the deadline the fallback stands
        // and behaviour is exactly what it was before this wait existed.
        const std::int64_t now = clock.now_nanos();
        if (could_have_opened_since == 0) could_have_opened_since = now;
        // The decision itself lives in `client/capability_grace.hpp`, where it
        // can be asserted without standing up a client, a server and a host.
        // What stays here is the clock reading and the "since when" stamp,
        // which are facts about this loop rather than about the rule.
        if (!first_terminal_may_open(host_caps_answered, now - could_have_opened_since)) return;
        asked_for_the_first_terminal = true;
        if (!session.terminal_ids().empty()) return;
        proto::NewTerminal ask;
        ask.command = shell_for_new_terminals;
        ask.host_sixel = session.host_sixel() ? 1 : 0;
        session.request(ask);
    };

    // One frame before the first question. The session list can arrive in the
    // very first poll pass — before the application has ever laid the desktop
    // out — and a dialog presented into an unlaid desktop is sized against
    // nothing: a zero-rect window, on screen but invisible, and modal, so it
    // also swallows every key a reader presses at what looks like a working
    // ckmux. One step gives the desktop its real bounds first.
    (void)app.step(clock.now_nanos());

    // Asked before anything is attached to: what this client does next depends
    // on what is already running, and only the server knows that.
    session.request(proto::ListSessions{});

    session.on_attached = [&client, &session, &asked_for_the_first_terminal](std::uint64_t id) {
        if (client == nullptr) return;
        // Asked BEFORE the state moves and before the fresh list arrives:
        // "was somebody already watching this?" is what tells a reader they
        // have just taken a session from another client rather than picked up
        // an idle one (WP-14). A moment later the answer is yes either way,
        // because by then the somebody is them.
        const bool took_it_over = client->session_shows_attached(id);
        // The name comes from the list rather than the attach: a client that
        // attached from a picker already has it, and one that did not will get
        // it with the next list. Zero would be a lie either way — it is
        // watching this session now.
        client->set_attached_session(id, client->session_name(id));
        if (took_it_over) {
            // Said on this side too, not only on the side that lost it: latest
            // client wins with no prompt and no confirmation (the session model), so
            // the only thing that tells a reader they displaced somebody is
            // this line. Not persistent — they are looking at the session they
            // asked for, and nothing is missing from their screen.
            const std::string name = client->session_name(id);
            client->notify(name.empty() ? std::string("Took over this session")
                                        : "Took over '" + name + "'");
        }
        // The next empty-session check belongs to THIS attachment: a session
        // with nothing in it gets a terminal, and one with programs in it is
        // left exactly as its reader left it.
        asked_for_the_first_terminal = false;
        // So the name is right in the prompts that use it, and so that a reader
        // who takes a session over sees the list they are now part of.
        session.request(proto::ListSessions{});
    };
    session.on_detached = [&app, &client, &session, &detached,
                           &show_picker_when_the_list_arrives](proto::DetachReason reason,
                                                              const std::string& text) {
        // Nothing is written to stderr here, and nowhere else in this loop
        // either: an attached client's stderr IS the reader's terminal, and
        // ckVision is drawing on it. One line about a takeover landed in the
        // middle of a window frame and stayed there.
        //
        // A detach this client asked for is a client on its way out. Any other
        // — a takeover, a session ended under it — leaves it running with
        // nothing to watch, which is a state it can be in perfectly well: the
        // commands that need a session go grey and the reader can pick another.
        if (detached) {
            // On its way out at the reader's own request: the farewell line
            // below is printed once ckVision has given the terminal back, and
            // a message on a screen that is about to be torn down is one
            // nobody can read.
            app.request_quit();
            return;
        }
        // Everything else happened TO this reader, and until WP-14 it was
        // silent: the windows vanished, the desktop went empty, and the only
        // account of why was a `text` this loop threw away. It cannot go to
        // stderr, for the reason above — so it goes where a message that must
        // not interrupt belongs, a notification over the desktop. The wording
        // is `ClientApp::report_detached`'s, beside ckmux's other sentences.
        if (client != nullptr) client->report_detached(reason, text);
        if (client != nullptr) {
            client->forget_terminals();
            client->set_attached_session(0, {});
            // The connection is still up and the mirrors are still held: this
            // client was taken over, not disconnected. Telling the session its
            // windows are gone is what makes attaching again — to this session
            // or another — draw the terminals rather than an empty desktop
            // (C4). It was the takeover case that found this.
            session.windows_forgotten();
        }
        // What now? A reader whose session was taken from them, or ended under
        // them, is looking at an empty desktop; the list says what else is
        // there and the picker lets them go to it.
        show_picker_when_the_list_arrives = true;
        session.request(proto::ListSessions{});
    };

    // Anything worth telling the reader on the way out, printed once the
    // application has finished with the terminal.
    std::string farewell;
    std::int64_t next_reconnect_nanos = 0;
    while (!app.quit_requested()) {
        const std::int64_t now = clock.now_nanos();
        // The application's next timer, or nothing at all when it has none. A
        // cap either way: the socket is not the only reason to wake, and a loop
        // that slept until a timer would not notice a window being resized.
        const std::optional<std::int64_t> deadline = app.next_timer_deadline_nanos();
        const std::int64_t until = deadline.has_value() && *deadline > now ? *deadline - now : 0;
        const int timeout_ms =
            static_cast<int>(std::min<std::int64_t>(until > 0 ? until / 1'000'000 : 50, 50));
        (void)wait_for_work(app, connection.stream.fd(), connection.stream.wants_write(),
                            std::max(1, timeout_ms));

        if (connection.stream.wants_write() && !connection.stream.flush()) {
            // A flush that fails is the server going away mid-write, and the
            // promise is the same as reading EOF three lines down: this client
            // keeps running (WP-8). Quitting here was the pre-WP-8 answer, and
            // it is why a reader who ended their last session could sometimes
            // watch their whole ckmux vanish instead of getting the picker —
            // whether the doomed request's flush beat the server's close was a
            // coin toss. The bytes were for nobody; the reconnect throttle
            // below decides what happens next.
            connection.stream.close();
            stop_showing_this_server();
        }
        std::string arrived;
        const bool alive = !connection.stream.open() || connection.stream.receive(arrived);
        if (!arrived.empty() && !reader.append(arrived)) {
            // A frame this build cannot hold — too large for the reader's own
            // limits, or framing that cannot be resynchronised. Said out loud
            // for the same reason the decode failure below is: an exit status
            // of 0 with nothing printed is, to a reader and to a script,
            // indistinguishable from the quit they asked for.
            farewell = "ckmux: the server sent a frame this build cannot hold";
            break;
        }
        for (;;) {
            proto::Message message;
            const proto::DecodeError error = reader.next(message);
            if (error == proto::DecodeError::Incomplete) break;
            if (error != proto::DecodeError::None) {
                // Said after the loop, once ckVision has given the terminal
                // back: a line printed now would land on top of the frame it
                // is still drawing.
                farewell = "ckmux: the server sent something this build cannot read";
                app.request_quit();
                break;
            }
            (void)session.handle(message);
        }
        session.heal_if_needed();
        // The reader's own window, if they have resized it. Asked of the host
        // terminal every pass rather than hooked to an event, because that is
        // where the answer is — ckVision resizes the desktop from its own
        // SIGWINCH handling and a client watching the same fact twice would
        // have two sizes to keep in step. The call sends nothing unless the
        // size actually moved.
        session.desktop_resized(host.size(), host.capabilities().cell_pixels);
        open_one_if_the_session_is_empty();
        // Every window whose terminal has news repaints, which is the same
        // notification a local terminal raises — the client does not care where
        // the bytes came from. "Has news" is asked, not assumed: a local
        // subsession only reaches notify_terminal_subsession_changed when its
        // own drain() found something (ckVision's Application::step()), and
        // a remote one's drain() is permanently a no-op (input arrives as
        // socket messages, not a per-terminal read) — so unconditionally
        // notifying every terminal here was standing in for that gate, at
        // the cost of every terminal repainting, and every picture on
        // screen being re-fingerprinted and potentially re-encoded, on
        // every single pass of this loop rather than only when one
        // genuinely had something new.
        for (const std::uint64_t id : session.terminal_ids())
            if (RemoteTerminalSubsession* remote = session.terminal(id)) {
                if (!remote->damage().any()) continue;
                app.root().notify_terminal_subsession_changed(*remote);
                remote->clear_damage();
            }
        (void)app.step(clock.now_nanos());
        if (!alive) {
            // The server has gone — because a reader ended the last session, or
            // because it failed. Either way this client keeps running with
            // nothing attached: its windows belong to terminals that no longer
            // exist, and the commands that need a session go grey until there
            // is one again. Quitting on it would take a reader's whole ckmux
            // away for something they may well have asked for.
            connection.stream.close();
            stop_showing_this_server();
        }
        // A reader who was waiting for a session list still is, connection or
        // no connection. Asking again starts a server, which is what makes
        // "end the last session, then start another" one uninterrupted thing —
        // and it RETRIES on later passes rather than deciding once, because
        // the moment after a server exits is exactly when connecting to its
        // socket can fail: the old one may not have unbound yet. Throttled, so
        // a machine where no server can start is not asked twenty times a
        // second.
        if (show_picker_when_the_list_arrives && !connection.stream.open()) {
            const std::int64_t now_nanos = clock.now_nanos();
            if (now_nanos >= next_reconnect_nanos) {
                next_reconnect_nanos = now_nanos + 1'000'000'000;
                if (ensure_connected()) session.request(proto::ListSessions{});
            }
        }
    }
    if (connection.stream.open()) {
        if (!detached) (void)connection.stream.send(proto::encode(proto::Detach{}));
        (void)connection.stream.flush();
    }
    if (!farewell.empty()) {
        std::fprintf(stderr, "%s\n", farewell.c_str());
        return 1;
    }
    return 0;
}

}  // namespace ckm::client
