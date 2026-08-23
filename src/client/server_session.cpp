// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "client/server_session.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <variant>

namespace ckm::client {

ServerSession::ServerSession(Send send) : send_(std::move(send)) {
    profile_ = ckv::term::embedded_xterm_sixel_profile();
}

void ServerSession::attach(std::uint64_t session, ckv::Size desktop, ckv::Size cell_pixels) {
    // What was reported described the arrangement of the session being left, so
    // it stops being an answer to "has this changed?" the moment another one is
    // asked for. A re-`attach` to the SAME session is a heal (see
    // `request_snapshot`), and the server still holds what it was told, so that
    // one deliberately keeps its memory rather than restating an arrangement
    // nothing has moved.
    if (session != session_) layout_sent_.clear();
    session_ = session;
    desktop_ = desktop;
    cell_pixels_ = cell_pixels;
    // Whatever heal was in flight is answered by this attach, or superseded by
    // it: either way the guard starts over, so a reader who switches sessions
    // while a mirror is asking for a snapshot is not left unable to ask again.
    snapshot_requested_ = false;
    if (!send_) return;
    proto::Attach request;
    request.session = session;
    request.columns = static_cast<std::uint16_t>(std::max(0, desktop.width));
    request.rows = static_cast<std::uint16_t>(std::max(0, desktop.height));
    // The whole text area, because that is what a client can measure; the server
    // divides it back down to one cell (WP-3, where confusing the two was
    // silent).
    request.pixel_width =
        static_cast<std::uint16_t>(std::max(0, desktop.width) * std::max(0, cell_pixels.width));
    request.pixel_height =
        static_cast<std::uint16_t>(std::max(0, desktop.height) * std::max(0, cell_pixels.height));
    request.host_sixel = host_sixel_ ? 1 : 0;
    request.share = share_ ? 1 : 0;
    send_(request);
}

void ServerSession::set_stats_watched(bool on) {
    if (stats_watched_ == on) return;
    stats_watched_ = on;
    if (send_) send_(proto::WatchStats{static_cast<std::uint8_t>(on ? 1 : 0)});
}

void ServerSession::adopt_session_desktop(std::uint16_t columns, std::uint16_t rows) {
    // Zeros are a server that has not said — one from before WP-40, or a
    // field nothing filled — and silence does not shrink a world.
    if (columns == 0 || rows == 0) return;
    const ckv::Size world{static_cast<int>(columns), static_cast<int>(rows)};
    if (world == session_desktop_) return;
    session_desktop_ = world;
    if (on_session_desktop) on_session_desktop(world);
}

void ServerSession::desktop_resized(ckv::Size desktop, ckv::Size cell_pixels) {
    if (desktop.width <= 0 || desktop.height <= 0) return;
    if (desktop == desktop_ && cell_pixels == cell_pixels_) return;
    desktop_ = desktop;
    if (cell_pixels.width > 0 && cell_pixels.height > 0) cell_pixels_ = cell_pixels;
    // Remembered whether or not it can be sent, because the next `Attach` — a
    // heal, a switch, a reconnection — declares the desktop, and one that
    // declared a size the reader no longer has would undo this at the worst
    // moment.
    if (!attached_ || !send_) return;
    proto::ClientResize resize;
    resize.columns = static_cast<std::uint16_t>(desktop_.width);
    resize.rows = static_cast<std::uint16_t>(desktop_.height);
    // The whole text area, as `Attach` carries it: the server divides it back
    // down to one cell (WP-3, where confusing the two was silent).
    resize.pixel_width = static_cast<std::uint16_t>(desktop_.width * std::max(0, cell_pixels_.width));
    resize.pixel_height =
        static_cast<std::uint16_t>(desktop_.height * std::max(0, cell_pixels_.height));
    send_(resize);
}

void ServerSession::report_layout(std::vector<proto::LayoutEntry> entries) {
    // Nothing to say. A client with no windows — one that has just left a
    // session, one whose connection went — is not reporting an empty
    // arrangement, it is reporting nothing at all, and the two are different:
    // an empty `SetLayout` names no terminal and so changes nothing the server
    // holds, which makes it a message whose only effect is to have been sent.
    if (entries.empty()) return;
    // A layout belongs to a session. Unattached, the server would answer
    // `Error{NoSuchSession, "SetLayout"}` — correctly, because it has nowhere
    // to file one — and the error would reach a reader who asked for nothing.
    if (!attached_ || !send_) return;
    if (entries == layout_sent_) return;
    layout_sent_ = std::move(entries);
    proto::SetLayout report;
    report.entries = layout_sent_;
    send_(report);
}

RemoteTerminalSubsession& ServerSession::ensure_terminal(std::uint64_t id) {
    const auto found = terminals_.find(id);
    if (found != terminals_.end()) return *found->second;
    auto terminal = std::make_unique<RemoteTerminalSubsession>(id, profile_, send_);
    terminal->mirror().set_history_limit(history_limit_);
    // The scene identity its pictures will carry. Assigned here because a
    // mirror is never adopted by the Application (whose counter serves the
    // subsessions it launches itself), and a raster whose id stayed 0 would
    // be dropped by TerminalView as not-yet-identified (WP-16).
    terminal->set_raster_identity(next_raster_identity_++);
    RemoteTerminalSubsession& reference = *terminal;
    terminals_.emplace(id, std::move(terminal));
    // Told to the window layer, and remembered as told. The set is what keeps
    // a reattach from announcing the terminals a client is already showing,
    // and what makes it announce the ones it is not (see `handle`).
    announced_.insert(id);
    if (on_terminal_opened) on_terminal_opened(reference);
    return reference;
}

void ServerSession::paste(std::uint64_t term, std::string bytes) {
    if (bytes.empty()) return;
    // Cut first, send later. The whole paste is turned into chunks up front so
    // that the reader's clipboard is captured at the moment they asked for it:
    // a paste that read its source as it drained would be a different paste if
    // the clipboard changed halfway through.
    for (std::size_t at = 0; at < bytes.size(); at += kPasteChunkBytes) {
        const std::size_t take = std::min(kPasteChunkBytes, bytes.size() - at);
        PasteWaiting waiting;
        waiting.term = term;
        waiting.seq = next_paste_seq_++;
        waiting.last = at + take >= bytes.size();
        waiting.bytes = bytes.substr(at, take);
        paste_queue_.push_back(std::move(waiting));
    }
    pump_paste();
}

void ServerSession::pump_paste() {
    while (paste_in_flight_ < kPasteCredit && !paste_queue_.empty()) {
        PasteWaiting waiting = std::move(paste_queue_.front());
        paste_queue_.pop_front();
        proto::PasteChunk chunk;
        chunk.term = waiting.term;
        chunk.seq = waiting.seq;
        chunk.final_chunk = waiting.last ? 1 : 0;
        chunk.bytes = std::move(waiting.bytes);
        request(chunk);
        ++paste_in_flight_;
    }
}

void ServerSession::abandon_paste() {
    paste_queue_.clear();
    // The credit goes back too. Whatever was on the wire is either already
    // written or will be dropped with the connection; either way no ack for it
    // will ever reach this object, and a credit left spent would silently
    // shorten every later paste by that much.
    paste_in_flight_ = 0;
}

void ServerSession::connection_lost() {
    attached_ = false;
    session_ = 0;
    snapshot_requested_ = false;
    // The world went with the session. No callback: the client is tearing its
    // desktop down anyway, and resets its own extent where it does that.
    session_desktop_ = ckv::Size{0, 0};
    // No `on_terminal_closed`: the client takes its own windows down when it is
    // told the connection went, and calling back from here would have it doing
    // it twice for terminals nobody can close anyway.
    terminals_.clear();
    announced_.clear();
    // Including what this client had reported about where its windows were: it
    // was reported to a server that no longer exists, and the next one has
    // never been told anything.
    layout_sent_.clear();
    abandon_paste();
}

void ServerSession::windows_forgotten() {
    // A paste in progress goes with them (WP-18). The rest of a reader's
    // clipboard is aimed at one terminal in the session they were watching,
    // and this is the moment they stopped watching it — detached, or taken
    // over. The server routes a chunk by terminal id and asks nothing about
    // who is attached, so finishing the paste would type into a window that
    // now belongs to whoever took the session.
    abandon_paste();
    // The windows went; the attachment and its mirrors did not. Said out loud
    // rather than inferred, because the two states that look identical from
    // here — a client healing mid-session and a client that has just taken its
    // windows down — need opposite answers from the next snapshot.
    announced_.clear();
}

void ServerSession::request_snapshot() {
    // One request at a time. `needs_snapshot()` stays true until the snapshot
    // arrives and `heal_if_needed()` runs on every pass of the client's poll
    // loop, so an unguarded heal asked the server for every terminal whole
    // twenty times a second — and each of those answers is every screen and
    // every scrollback in the session, composed and queued again.
    if (snapshot_requested_) return;
    ++resnapshots_;
    attach(session_, desktop_, cell_pixels_);
    // After the attach, which clears it: this is the one caller that wants the
    // guard set rather than reset.
    snapshot_requested_ = true;
}

bool ServerSession::handle(const proto::Message& message) {
    if (const auto* attached = std::get_if<proto::Attached>(&message)) {
        attached_ = true;
        ++attachments_;
        snapshot_requested_ = false;  // whatever was asked for has arrived
        session_ = attached->session;
        // The world, before anything is placed in it (WP-43): a window whose
        // rect lies beyond this client's own screen must find the extent
        // already grown when it arrives, or the view-sized desktop clamps it
        // on adoption and a rect the session owns has moved because somebody
        // small looked at it.
        adopt_session_desktop(attached->snapshot.desktop_columns,
                              attached->snapshot.desktop_rows);
        // The reader's standing wish, restated on the connection this attach
        // rode in on: the server's subscription is per connection and this
        // may be a fresh one. Idempotent when it is not.
        if (stats_watched_ && send_) send_(proto::WatchStats{1});
        // Every terminal in the snapshot, whole. This is both the first attach
        // and every healing after one, and it is the same code because it is the
        // same thing: the server saying "here is what you should be holding".
        for (const proto::TerminalState& state : attached->snapshot.terminals)
            ensure_terminal(state.term).mirror().adopt(state);
        // A terminal that was in the last snapshot and is not in this one has
        // gone while this client was not listening. Closing it here rather than
        // waiting for a `TermClosed` that will never come is what makes a
        // reattach after a disconnection show the truth.
        std::vector<std::uint64_t> vanished;
        for (const auto& entry : terminals_) {
            const std::uint64_t held = entry.first;
            const bool present = std::any_of(attached->snapshot.terminals.begin(),
                                             attached->snapshot.terminals.end(),
                                             [held](const proto::TerminalState& state) {
                                                 return state.term == held;
                                             });
            if (!present) vanished.push_back(held);
        }
        for (const std::uint64_t id : vanished) {
            if (on_terminal_closed) on_terminal_closed(id);
            terminals_.erase(id);
            announced_.erase(id);
        }
        // Every terminal the window layer is not showing, announced now that
        // its mirror holds the snapshot.
        //
        // `ensure_terminal` announces a terminal when it CREATES its mirror,
        // and the mirrors outlive a detach that is not a disconnection: after a
        // takeover, or a switch away and back, the client still holds them. So
        // a reattach created nothing, announced nothing, and left the reader
        // looking at an empty desktop over a session full of running programs
        // (C4). Reconciled against what the window layer says it has rather
        // than announced wholesale, because a mid-session heal arrives down
        // this same path with every window already on screen — and a second
        // window per terminal is the other way to get this wrong.
        //
        // The ids are collected first: opening a window is the client's work,
        // and it must not run while this map is being walked.
        std::vector<std::uint64_t> unannounced;
        for (const auto& entry : terminals_)
            if (announced_.find(entry.first) == announced_.end())
                unannounced.push_back(entry.first);
        for (const std::uint64_t id : unannounced) {
            announced_.insert(id);
            const auto held = terminals_.find(id);
            if (held != terminals_.end() && on_terminal_opened) on_terminal_opened(*held->second);
        }
        // And where all of it belongs (WP-30). Last, because every window it
        // names has to exist before there is anything to place: the loop above
        // is what opens them, and a layout stated before it would be describing
        // a desktop that is still empty.
        //
        // From the snapshot's own per-terminal fields rather than from a
        // message of its own — the arrangement is part of the state a
        // reattaching client is handed, not news about it (the session model: window
        // layout is session state, owned by the server).
        if (on_layout) {
            std::vector<proto::LayoutEntry> arrangement;
            arrangement.reserve(attached->snapshot.terminals.size());
            for (const proto::TerminalState& state : attached->snapshot.terminals)
                arrangement.push_back(proto::LayoutEntry{state.term, state.rect, state.z_order,
                                                         state.zoomed, state.tile});
            on_layout(arrangement, LayoutStatement::Snapshot);
        }
        if (on_attached) on_attached(session_);
        return true;
    }
    if (const auto* layout = std::get_if<proto::LayoutDelta>(&message)) {
        // The session's arrangement, restated on the server's tick — and passed
        // on as `Delta`, which is the whole of what makes it safe. Nothing is
        // laid down from one (see `LayoutStatement`, and `run_client.cpp`'s
        // handler): there is at most one watcher per session and it is the
        // client that reported this very arrangement, so a statement here is
        // normally its own report coming home.
        //
        // Acting on one cost three regressions, and the shape of them is worth
        // recording. A terminal opened AFTER the snapshot — which is every
        // terminal of a freshly made session — first has its place stated by a
        // delta, roughly a settle interval plus a tick after its window
        // appeared. Restoring "the first place the server states" therefore
        // ran while the reader was part-way through a menu or reading a
        // dialog: the raise put a terminal window over the modal asking about
        // it, and the focus move ate the keystrokes opening it. A layout has no
        // opinion about where somebody is typing.
        //
        // The session is checked because the message names one and this client
        // watches exactly one: an arrangement for a session it is not showing
        // describes windows it does not have.
        if (layout->session == session_) {
            // The world may have changed shape — a reader resized it, WP-40's
            // resize-session — and the statement rides the same message the
            // arrangement does.
            adopt_session_desktop(layout->desktop_columns, layout->desktop_rows);
            if (on_layout) on_layout(layout->entries, LayoutStatement::Delta);
        }
        return true;
    }
    if (const auto* ack = std::get_if<proto::PasteAck>(&message)) {
        // One chunk is written to the child, so one more may go (WP-18). The
        // seq is not checked against what was sent: the credit is a count, and
        // the server acks each chunk exactly once in the order it wrote them,
        // so an ack that named a chunk this client never sent would be a
        // protocol error — and protocol errors here drop the connection rather
        // than being repaired (the protocol spec, invariant 2), which is a decision
        // taken above this object.
        (void)ack;
        if (paste_in_flight_ > 0) --paste_in_flight_;
        pump_paste();
        return true;
    }
    if (const auto* detached = std::get_if<proto::Detached>(&message)) {
        attached_ = false;
        // Whatever was reported was about a session this client is no longer
        // watching, and the windows that made up that arrangement are about to
        // be taken down. Attaching again — here or elsewhere — reports afresh.
        layout_sent_.clear();
        if (on_detached) on_detached(detached->reason, detached->text);
        return true;
    }
    if (const auto* delta = std::get_if<proto::GridDelta>(&message)) {
        RemoteTerminalSubsession* terminal = this->terminal(delta->term);
        // A delta for a terminal this client has never heard of is not a reason
        // to invent one: the snapshot is what says which terminals exist, so
        // this asks for one.
        if (terminal == nullptr) {
            request_snapshot();
            return true;
        }
        (void)terminal->mirror().apply(*delta);
        return true;
    }
    if (const auto* opened = std::get_if<proto::TermOpened>(&message)) {
        // Sized from the announcement, so the full repaint that follows it fits.
        // A mirror left at no size would refuse that delta, ask for a snapshot,
        // and take a round trip to learn something it had already been told.
        ensure_terminal(opened->term)
            .mirror()
            .open(ckv::Size{opened->columns, opened->rows});
        return true;
    }
    if (const auto* closed = std::get_if<proto::TermClosed>(&message)) {
        if (RemoteTerminalSubsession* terminal = this->terminal(closed->term)) {
            terminal->mirror().apply(*closed);
            // The window stays until a reader closes it, banner and all
            // (the session model on-exit), so the mirror is kept: what exited is the
            // child, not the terminal's last screen.
            if (closed->hold == 0) {
                if (on_terminal_closed) on_terminal_closed(closed->term);
                // And the mirror goes with the window — the same erasure the
                // Attached prune does. A terminal that merely LEFT (a move,
                // exited=0) can come back under the same id on a later
                // attach, and a stale entry here would swallow the
                // on_terminal_opened that builds its window.
                terminals_.erase(closed->term);
                announced_.erase(closed->term);
            }
        }
        return true;
    }
    if (const auto* meta = std::get_if<proto::TermMeta>(&message)) {
        if (RemoteTerminalSubsession* terminal = this->terminal(meta->term))
            terminal->mirror().apply(*meta);
        return true;
    }
    if (const auto* stats = std::get_if<proto::TermStats>(&message)) {
        // Straight through to the display: stats are a stream, not state — a
        // number a second old is not worth holding, and the mirror deliberately
        // learns nothing (no snapshot carries stats, so a mirror that held
        // them would disagree with its own rehydration).
        if (RemoteTerminalSubsession* terminal = this->terminal(stats->term))
            if (on_stats) on_stats(*terminal, *stats);
        return true;
    }
    if (const auto* clipboard = std::get_if<proto::ClipboardSet>(&message)) {
        // Into the terminal that asked. What happens next is the client's:
        // the mirror's serial moves, the view over it notices on the next
        // repaint and hands the text to `on_clipboard_write`, which is the same
        // path a terminal in this process would take (the ckVision integration spec seam 2).
        if (RemoteTerminalSubsession* terminal = this->terminal(clipboard->term))
            terminal->mirror().apply(*clipboard);
        return true;
    }
    if (const auto* printing = std::get_if<proto::PrintState>(&message)) {
        if (RemoteTerminalSubsession* terminal = this->terminal(printing->term))
            terminal->mirror().apply(*printing);
        return true;
    }
    // The jobs themselves, which `PrintState` does not carry: it reports the
    // COUNT a terminal holds, and these two report WHICH ones and what is in
    // them. Both were sent by the server and both had an `apply` waiting on
    // the mirror; neither was ever dispatched, so `TerminalMirror::print_jobs()`
    // stayed empty however much a child printed. The reader saw the count on
    // the frame button and an empty Print Output dialog beside it — the button
    // reading `PRINT · 2 · 0 B` against "Nothing has been captured from this
    // terminal", on one screen at one moment.
    if (const auto* added = std::get_if<proto::PrintJobAdded>(&message)) {
        if (RemoteTerminalSubsession* terminal = this->terminal(added->term))
            terminal->mirror().apply(*added);
        return true;
    }
    // One job's text, in chunks, and only ever after the reader asked for it:
    // the payload never travels unasked, so this arrives in answer to a
    // `PrintJobFetch` and completes on the chunk marked final.
    if (const auto* chunk = std::get_if<proto::PrintJobData>(&message)) {
        if (RemoteTerminalSubsession* terminal = this->terminal(chunk->term))
            (void)terminal->mirror().apply(*chunk);
        return true;
    }
    if (const auto* said = std::get_if<proto::TermDiagnostic>(&message)) {
        if (RemoteTerminalSubsession* terminal = this->terminal(said->term))
            terminal->mirror().apply(*said);
        return true;
    }
    if (const auto* error = std::get_if<proto::Error>(&message)) {
        // A request the server could not honour. It is always ANSWERED — that
        // is the rule this message exists for — and until now the answer
        // stopped at the client's front door: nothing claimed it here and the
        // host discarded it, so a reader who asked for something impossible was
        // told nothing at all and watched a ckmux that looked hung (the protocol spec).
        if (on_error) on_error(*error);
        return true;
    }
    if (const auto* sessions = std::get_if<proto::SessionList>(&message)) {
        if (on_sessions) on_sessions(sessions->sessions);
        return true;
    }
    // The pictures (WP-16). Pixels arrive by global id — begin, chunks in
    // order, end — and only a Place binds them to a terminal. A restated id
    // (every snapshot restates the pictures) simply starts the assembly over.
    if (const auto* begin = std::get_if<proto::ImageAddBegin>(&message)) {
        PendingImage pending;
        pending.width = begin->width;
        pending.height = begin->height;
        // The final size is already known; reserving it up front means the
        // chunk loop below never reallocates and copies what it already
        // holds. A picture the size of a dialog is chunked into single-digit
        // pieces at kMaxChunkPayloadBytes, so unreserved growth was paying to
        // copy several megabytes multiple times over for what should be one
        // append each.
        pending.bytes.reserve(static_cast<std::size_t>(pending.width) *
                              static_cast<std::size_t>(pending.height) * 4U);
        pending_images_[begin->id] = std::move(pending);
        return true;
    }
    if (const auto* chunk = std::get_if<proto::ImageChunk>(&message)) {
        const auto pending = pending_images_.find(chunk->id);
        if (pending == pending_images_.end()) return true;  // never begun: stale
        // Out of order, or more bytes than the announced size can hold: the
        // assembly is wrong and the honest answer is no picture rather than a
        // scrambled one. The snapshot path restates what was lost.
        const std::size_t total = static_cast<std::size_t>(pending->second.width) *
                                  static_cast<std::size_t>(pending->second.height) * 4U;
        if (chunk->seq != pending->second.next_seq ||
            pending->second.bytes.size() + chunk->bytes.size() > total) {
            pending_images_.erase(pending);
            return true;
        }
        pending->second.bytes += chunk->bytes;
        ++pending->second.next_seq;
        return true;
    }
    if (const auto* end = std::get_if<proto::ImageEnd>(&message)) {
        const auto pending = pending_images_.find(end->id);
        if (pending == pending_images_.end()) return true;
        const std::size_t total = static_cast<std::size_t>(pending->second.width) *
                                  static_cast<std::size_t>(pending->second.height) * 4U;
        if (total != 0 && pending->second.bytes.size() == total) {
            auto image = std::make_shared<ckv::Image>(pending->second.width,
                                                      pending->second.height);
            std::memcpy(image->data(), pending->second.bytes.data(), total);
            completed_images_[end->id] = std::move(image);
        }
        pending_images_.erase(pending);
        return true;
    }
    if (const auto* place = std::get_if<proto::ImagePlace>(&message)) {
        RemoteTerminalSubsession* terminal = this->terminal(place->term);
        std::shared_ptr<const ckv::Image> image;
        if (const auto completed = completed_images_.find(place->id);
            completed != completed_images_.end()) {
            image = std::move(completed->second);
            // One id is one placed picture (the server mints per placement),
            // so the store hands the pixels over exactly once and stays small.
            completed_images_.erase(completed);
        }
        if (terminal != nullptr)
            terminal->mirror().place_image(
                place->id, std::move(image), ckv::Point{place->cells.x, place->cells.y},
                ckv::Size{place->cells.width, place->cells.height});
        return true;
    }
    if (const auto* remove = std::get_if<proto::ImageRemove>(&message)) {
        // Pixels that arrived but were never placed go too. The ordinary
        // order is Begin/chunks/End and then the Place that takes them, so
        // this is usually empty — but a picture the server dropped between
        // the two leaves an entry nothing will ever come back for, and wire
        // ids are never reused, so nothing would ever overwrite it either.
        pending_images_.erase(remove->id);
        completed_images_.erase(remove->id);
        if (RemoteTerminalSubsession* terminal = this->terminal(remove->term))
            terminal->mirror().remove_image(remove->id);
        return true;
    }
    if (std::holds_alternative<proto::Pong>(message)) return true;
    return false;
}

void ServerSession::heal_if_needed() {
    bool needed = false;
    for (const auto& entry : terminals_)
        if (entry.second->mirror().needs_snapshot()) needed = true;
    if (!needed) return;
    // One request for the whole attachment rather than one per terminal: what a
    // client has lost track of is the stream, and the stream is per attachment.
    request_snapshot();
}

RemoteTerminalSubsession* ServerSession::terminal(std::uint64_t id) {
    const auto found = terminals_.find(id);
    return found == terminals_.end() ? nullptr : found->second.get();
}

std::optional<std::uint64_t> ServerSession::id_of(const ckv::term::TerminalSubsession& terminal) const {
    for (const auto& [id, remote] : terminals_)
        if (remote.get() == &terminal) return id;
    return std::nullopt;
}

std::vector<std::uint64_t> ServerSession::terminal_ids() const {
    std::vector<std::uint64_t> ids;
    ids.reserve(terminals_.size());
    for (const auto& entry : terminals_) ids.push_back(entry.first);
    return ids;
}

}  // namespace ckm::client
