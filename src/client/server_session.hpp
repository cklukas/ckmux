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
#include <set>
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
    // A request this server could not honour, with its code and the context it
    // failed in. Shown to the reader by whoever draws: a request is always
    // answered, and an answer nobody surfaces is indistinguishable from a
    // server that has hung.
    std::function<void(const proto::Error&)> on_error;
    // Which of the two situations a layout statement came out of. They carry
    // the same shape and mean entirely different things, so the caller is told
    // which rather than left to guess (WP-30).
    enum class LayoutStatement {
        // The attach snapshot: the arrangement a client has just been handed,
        // for windows it has just opened. This is a REATTACH, and the only
        // moment there is anything to lay down.
        Snapshot,
        // The server's tick, while this client is watching. There is at most
        // one watcher per session (plans/03's takeover) and it is the client
        // that reported the arrangement, so this is normally that client's own
        // report coming home — news about the server's record, never an
        // instruction to move a window the reader is looking at.
        Delta,
    };

    // Where this session's windows belong, as the server holds it (WP-30).
    //
    // Both situations reach it, tagged, because both are the server stating an
    // arrangement and only one of them is a reattach. Each hands over the WHOLE
    // arrangement, because a `z_order` is a position among windows and a
    // partial list is one nothing can lay down.
    //
    // A `Snapshot` statement is announced AFTER `on_terminal_opened` for every
    // terminal in it, and that order is load-bearing: a place stated before the
    // window it is about exists is a place nothing can be put in.
    std::function<void(const std::vector<proto::LayoutEntry>&, LayoutStatement)> on_layout;

    explicit ServerSession(Send send);

    // Asks to attach, declaring this client's desktop. Sending it again is how a
    // client resnapshots — the server answers an `Attach` with the terminals
    // whole, which is exactly what a client that has lost track needs and is why
    // healing needs no message of its own.
    void attach(std::uint64_t session, ckv::Size desktop, ckv::Size cell_pixels);

    // The reader's own terminal changed size while this client was attached.
    //
    // The desktop travelled once, at attach time, and a host terminal resized
    // afterwards reached the server only at the next `Attach` — so a reader who
    // made their window bigger kept the pixel metric of the old one until they
    // detached and came back (`ClientResize` had a server handler and no
    // sender at all). Idempotent: called from the client's loop on every pass,
    // it sends a message only when the size has actually moved.
    void desktop_resized(ckv::Size desktop, ckv::Size cell_pixels);

    // Where this client's windows now are: the whole arrangement of the session
    // it is attached to, in one `SetLayout` (WP-29).
    //
    // The report is composed by whoever owns the windows and translated to wire
    // ids by the one place that has both halves; this end owns only the two
    // rules about putting it on the wire:
    //
    //   * **Never while unattached.** A layout belongs to a session, and a
    //     client in none would be answered with `Error{NoSuchSession}` — an
    //     error a client provoked by asking a question it already knew the
    //     answer to, which is the kind of noise that makes a real one hard to
    //     see.
    //   * **Never an arrangement already reported.** Edge-triggered, exactly as
    //     the server's own producer is: the client debounces a drag into one
    //     report, and this is what keeps a report that says nothing new — a
    //     window dragged back where it started, a heal that rebuilt nothing —
    //     off the wire entirely.
    //
    // An empty arrangement is not sent at all: a client with no windows is
    // saying nothing about any terminal, and a terminal that has gone takes its
    // stored layout with it on the server side anyway.
    void report_layout(std::vector<proto::LayoutEntry> entries);

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

    // The window layer has taken its windows down while this attachment lives
    // on: a client leaving a session it was watching, or one whose session was
    // taken over. The programs keep running and the mirrors stay; only the
    // windows go.
    //
    // Saying so is not bookkeeping. `on_terminal_opened` fires when a mirror is
    // CREATED, so a reattach whose mirrors were still held announced nothing
    // and the reader met an empty desktop over a running session (C4). This is
    // what makes the next snapshot announce them all again — and what keeps a
    // mid-session heal, which re-adopts every mirror down the same path, from
    // opening a second window over each of the ones already on screen.
    void windows_forgotten();

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
    // Whether this client's OUTER terminal reported Sixel, carried on
    // `Attach` and on every `NewTerminal` (WP-16): the server folds it into
    // what the children this client opens are told they may draw. The
    // capability answer arrives asynchronously — DA1's reply chief among it
    // — so this is read fresh at the moment each such request is built
    // rather than trusted from whatever `Attach` carried at connection time,
    // which a pane opened seconds or minutes later must not still be bound
    // by (field report, 2026-08-18: every terminal a client ever opened
    // advertised no Sixel to its child, regardless of what the real host
    // answered, because only Attach carried this and nothing ever resent it
    // once the probe actually came back).
    void set_host_sixel(bool reported) noexcept { host_sixel_ = reported; }
    bool host_sixel() const noexcept { return host_sixel_; }

private:
    RemoteTerminalSubsession& ensure_terminal(std::uint64_t id);
    // Asks for every terminal whole, at most once until the answer arrives.
    // The heal path and the delta-for-an-unknown-terminal path share it so
    // that "how many times has this client had to ask?" counts requests that
    // were actually sent — which is what `resnapshots()` claims to say.
    void request_snapshot();

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
    // Which terminals the window layer has been told about. A subset of
    // `terminals_`, cleared by `windows_forgotten()`; the difference between
    // the two is exactly the set a snapshot has to announce.
    std::set<std::uint64_t> announced_;
    std::map<std::uint64_t, PendingImage> pending_images_;
    std::map<std::uint64_t, std::shared_ptr<const ckv::Image>> completed_images_;
    ckv::term::TerminalCapabilityProfile profile_;
    std::size_t history_limit_ = 0;
    std::uint64_t session_ = 0;
    ckv::Size desktop_{0, 0};
    ckv::Size cell_pixels_{0, 0};
    // The arrangement this client last put on the wire, so that reporting the
    // same one again costs nothing (WP-29). Forgotten whenever what it was
    // describing stops being true: an `attach` to another session, a detach, a
    // connection that has gone. Not forgotten by a heal's re-`attach` for the
    // session already held — the server kept the layout it was told, so
    // restating it would be a message that changes nothing.
    std::vector<proto::LayoutEntry> layout_sent_;
    std::uint64_t resnapshots_ = 0;
    std::uint64_t attachments_ = 0;
    // Whether a snapshot this client asked for is still on its way. Cleared by
    // the `Attached` that answers it, by any explicit `attach()`, and by the
    // connection going.
    bool snapshot_requested_ = false;
    bool attached_ = false;
    bool host_sixel_ = false;
    // The scene identity a remote terminal's rasters carry. Minted per
    // terminal, well away from Application's own adopted-subsession counter
    // (which starts at 1'000'000), because the compositor keys picture
    // placements by it and two terminals must never share one.
    int next_raster_identity_ = 2'000'000;
};

}  // namespace ckm::client
