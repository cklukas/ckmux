// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "client/server_session.hpp"

#include <algorithm>
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
    send_(request);
}

RemoteTerminalSubsession& ServerSession::ensure_terminal(std::uint64_t id) {
    const auto found = terminals_.find(id);
    if (found != terminals_.end()) return *found->second;
    auto terminal = std::make_unique<RemoteTerminalSubsession>(id, profile_, send_);
    terminal->mirror().set_history_limit(history_limit_);
    RemoteTerminalSubsession& reference = *terminal;
    terminals_.emplace(id, std::move(terminal));
    if (on_terminal_opened) on_terminal_opened(reference);
    return reference;
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
        RemoteTerminalSubsession& terminal = ensure_terminal(opened->term);
        (void)terminal;
        return true;
    }
    if (const auto* closed = std::get_if<proto::TermClosed>(&message)) {
        if (RemoteTerminalSubsession* terminal = this->terminal(closed->term)) {
            terminal->mirror().apply(*closed);
            // The window stays until a reader closes it, banner and all
            // (plans/03 on-exit), so the mirror is kept: what exited is the
            // child, not the terminal's last screen.
            if (closed->hold == 0 && on_terminal_closed) on_terminal_closed(closed->term);
        }
        return true;
    }
    if (const auto* meta = std::get_if<proto::TermMeta>(&message)) {
        if (RemoteTerminalSubsession* terminal = this->terminal(meta->term))
            terminal->mirror().apply(*meta);
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

std::vector<std::uint64_t> ServerSession::terminal_ids() const {
    std::vector<std::uint64_t> ids;
    ids.reserve(terminals_.size());
    for (const auto& entry : terminals_) ids.push_back(entry.first);
    return ids;
}

}  // namespace ckm::client
