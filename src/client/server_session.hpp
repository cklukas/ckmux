// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// A client's side of one attachment (WP-6).
//
// It owns the mirrors, routes what arrives into them, and knows the two things a
// client has to know about being attached:
//
//   * **A detach is not a failure.** It is a socket closing, a takeover, or a
//     reader asking. The terminals carry on without it, which is the promise the
//     whole project exists for, so nothing here tries to keep an attachment
//     alive.
//   * **Anything lost is healed by a snapshot, never by repair.** A gap in the
//     sequence, a delta that does not fit, a server that says it fell behind:
//     the answer is always to ask for the terminal whole again. A mirror
//     repaired from deltas it never saw would be showing a reader a screen no
//     program ever drew (plans/05).
//
// It deliberately does not own the socket or the loop — the client's own poll
// loop reads bytes and hands frames here — so this is testable with no socket at
// all, and so the same object works over a real connection or a recorded one.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "client/remote_terminal.hpp"
#include "common/proto.hpp"
#include "cvision/core/image.hpp"

namespace ckm::client {

class ServerSession {
public:
    using Send = std::function<void(const proto::Message&)>;

    // What a terminal appearing and disappearing looks like to whoever is
    // drawing windows. Both are optional: a test that only checks mirrors sets
    // neither, and the client sets both.
    std::function<void(RemoteTerminalSubsession&)> on_terminal_opened;
    std::function<void(std::uint64_t terminal)> on_terminal_closed;
    // Why the attachment ended, in the words the server used. Shown to the
    // reader as-is: "taken over by another client" is the message, and any
    // paraphrase of it here would be a second place to get it wrong.
    std::function<void(proto::DetachReason, const std::string&)> on_detached;
    // The attachment was granted, and to which session. Separate from the
    // snapshot that comes with it because a client shows the session's NAME in
    // places a snapshot knows nothing about — the title, the prompts that end
    // or rename it — and needs to be told the moment it is watching one.
    std::function<void(std::uint64_t session)> on_attached;
    // What the server said when asked what sessions exist. Asked for by
    // `ListSessions`, which is a question with an answer rather than a value a
    // client can hold: sessions come and go without this client's involvement.
    std::function<void(const std::vector<proto::SessionInfo>&)> on_sessions;

    explicit ServerSession(Send send);

    // Asks to attach, declaring this client's desktop. Sending it again is how a
    // client resnapshots — the server answers an `Attach` with the terminals
    // whole, which is exactly what a client that has lost track needs and is why
    // healing needs no message of its own.
    void attach(std::uint64_t session, ckv::Size desktop, ckv::Size cell_pixels);

    // Sends one message to the server. The mirrors send their own input and
    // resizes; this is for what belongs to the attachment rather than to a
    // terminal — asking for a new one, closing one, naming one.
    void request(const proto::Message& message) const {
        if (send_) send_(message);
    }

    // One message from the server. Returns false for a message this client does
    // not understand, which the caller logs rather than acts on.
    bool handle(const proto::Message& message);

    // Called after a batch of messages: asks the server for anything the mirrors
    // have discovered they need. Separate from `handle` so that a burst of
    // deltas costs at most one request, rather than one per delta.
    void heal_if_needed();

    // The connection this session was carried on has gone. Everything held
    // here described a server's terminals, and that server no longer exists, so
    // none of it is true any more — a mirror kept across a reconnection would
    // be showing a reader a screen from a process that has exited.
    void connection_lost();

    RemoteTerminalSubsession* terminal(std::uint64_t id);
    // The wire id of one of this session's mirrors, or nothing when the
    // subsession is not one of them. The client's dialogs hold a
    // `TerminalSubsession&` — the same handle a local terminal answers to —
    // so the id is recovered here rather than widening that type.
    std::optional<std::uint64_t> id_of(const ckv::term::TerminalSubsession& terminal) const;
    std::vector<std::uint64_t> terminal_ids() const;
    bool attached() const noexcept { return attached_; }
    std::uint64_t session() const noexcept { return session_; }
    // How many times this client has had to ask for a terminal whole again. A
    // counter, because "it happened once at startup" and "it happens every
    // second" are different faults, and only one of them is a bug.
    std::uint64_t resnapshots() const noexcept { return resnapshots_; }
    // How many times this client has been handed every terminal whole: once for
    // the attach, and once for each healing after it. The pair of counters is
    // what tells a reader's bug report apart from a healthy session — a client
    // that is being healed every second is not the same fault as one that was
    // healed at startup.
    std::uint64_t attachments() const noexcept { return attachments_; }

    // What the reader's configuration says a mirror keeps, and what a new
    // mirror is built with. Set once by the client.
    void set_history_limit(std::size_t lines) noexcept { history_limit_ = lines; }
    void set_profile(ckv::term::TerminalCapabilityProfile profile) {
        profile_ = std::move(profile);
    }
    // Whether this client's OUTER terminal reported Sixel, carried on every
    // `Attach` (WP-16): the server folds it into what the children this
    // client opens are told they may draw. Set once by the client, from the
    // host terminal's own capability report.
    void set_host_sixel(bool reported) noexcept { host_sixel_ = reported; }

private:
    RemoteTerminalSubsession& ensure_terminal(std::uint64_t id);

    // A picture whose pixels are still arriving (WP-16): AddBegin opened it,
    // chunks fill it in order, End completes it into `completed_images_`,
    // where the Place that follows collects it. Ids are global to the
    // connection, so the store is here rather than in any one mirror.
    struct PendingImage {
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint32_t next_seq = 0;
        std::string bytes;
    };

    Send send_;
    std::map<std::uint64_t, std::unique_ptr<RemoteTerminalSubsession>> terminals_;
    std::map<std::uint64_t, PendingImage> pending_images_;
    std::map<std::uint64_t, std::shared_ptr<const ckv::Image>> completed_images_;
    ckv::term::TerminalCapabilityProfile profile_;
    std::size_t history_limit_ = 0;
    std::uint64_t session_ = 0;
    ckv::Size desktop_{0, 0};
    ckv::Size cell_pixels_{0, 0};
    std::uint64_t resnapshots_ = 0;
    std::uint64_t attachments_ = 0;
    bool attached_ = false;
    bool host_sixel_ = false;
    // The scene identity a remote terminal's rasters carry. Minted per
    // terminal, well away from Application's own adopted-subsession counter
    // (which starts at 1'000'000), because the compositor keys picture
    // placements by it and two terminals must never share one.
    int next_raster_identity_ = 2'000'000;
};

}  // namespace ckm::client
