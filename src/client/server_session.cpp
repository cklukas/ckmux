// Copyright (c) 2026 C. Klukas. All rights reserved.
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
    session_ = session;
    desktop_ = desktop;
    cell_pixels_ = cell_pixels;
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
    send_(request);
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
    if (on_terminal_opened) on_terminal_opened(reference);
    return reference;
}

void ServerSession::connection_lost() {
    attached_ = false;
    session_ = 0;
    // No `on_terminal_closed`: the client takes its own windows down when it is
    // told the connection went, and calling back from here would have it doing
    // it twice for terminals nobody can close anyway.
    terminals_.clear();
}

bool ServerSession::handle(const proto::Message& message) {
    if (const auto* attached = std::get_if<proto::Attached>(&message)) {
        attached_ = true;
        ++attachments_;
        session_ = attached->session;
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
        }
        if (on_attached) on_attached(session_);
        return true;
    }
    if (const auto* detached = std::get_if<proto::Detached>(&message)) {
        attached_ = false;
        if (on_detached) on_detached(detached->reason, detached->text);
        return true;
    }
    if (const auto* delta = std::get_if<proto::GridDelta>(&message)) {
        RemoteTerminalSubsession* terminal = this->terminal(delta->term);
        // A delta for a terminal this client has never heard of is not a reason
        // to invent one: the snapshot is what says which terminals exist, so
        // this asks for one.
        if (terminal == nullptr) {
            ++resnapshots_;
            attach(session_, desktop_, cell_pixels_);
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
            // (plans/03 on-exit), so the mirror is kept: what exited is the
            // child, not the terminal's last screen.
            if (closed->hold == 0) {
                if (on_terminal_closed) on_terminal_closed(closed->term);
                // And the mirror goes with the window — the same erasure the
                // Attached prune does. A terminal that merely LEFT (a move,
                // exited=0) can come back under the same id on a later
                // attach, and a stale entry here would swallow the
                // on_terminal_opened that builds its window.
                terminals_.erase(closed->term);
            }
        }
        return true;
    }
    if (const auto* meta = std::get_if<proto::TermMeta>(&message)) {
        if (RemoteTerminalSubsession* terminal = this->terminal(meta->term))
            terminal->mirror().apply(*meta);
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
    ++resnapshots_;
    attach(session_, desktop_, cell_pixels_);
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
