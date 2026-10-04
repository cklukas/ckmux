// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/poller.hpp"

#include <cerrno>
#include <limits>

namespace ckm::platform {

Poller::Poller() = default;
Poller::~Poller() = default;

void Poller::clear() {
    // The capacity stays, which is what makes the promise in the header true:
    // the loop rebuilds this set on every pass, and a set that reallocated
    // every time would allocate once per tick for the life of the server.
    watching_.clear();
}

void Poller::watch(int fd, Interest interest) {
    if (fd < 0) return;
    pollfd entry{};
    entry.fd = fd;
    entry.events = 0;
    if (has(interest, Interest::Read)) entry.events |= POLLIN;
    if (has(interest, Interest::Write)) entry.events |= POLLOUT;
    entry.revents = 0;
    watching_.push_back(entry);
}

void Poller::watch(ckv::term::WaitHandle handle, Interest interest) {
    if (handle.kind != ckv::term::WaitHandleKind::PosixFileDescriptor ||
        handle.value > static_cast<std::uintptr_t>(std::numeric_limits<int>::max())) return;
    watch(static_cast<int>(handle.value), interest);
}

std::size_t Poller::watched() const noexcept { return watching_.size(); }

const std::vector<Ready>& Poller::wait(int timeout_ms) {
    ready_.clear();
    if (watching_.empty()) {
        // Nothing to wait on. A sleep is still the right answer — the caller
        // has a tick deadline — and a poll with no descriptors is exactly that,
        // including when the caller asked to wait indefinitely.
        if (timeout_ms != 0 && ::poll(nullptr, 0, timeout_ms) < 0)
            outcome_ = errno == EINTR ? Outcome::Interrupted : Outcome::Failed;
        else
            outcome_ = Outcome::TimedOut;
        return ready_;
    }
    for (pollfd& entry : watching_) entry.revents = 0;
    const int count = ::poll(watching_.data(), static_cast<nfds_t>(watching_.size()), timeout_ms);
    if (count < 0) {
        outcome_ = errno == EINTR ? Outcome::Interrupted : Outcome::Failed;
        return ready_;
    }
    if (count == 0) {
        outcome_ = Outcome::TimedOut;
        return ready_;
    }
    for (const pollfd& entry : watching_) {
        if (entry.revents == 0) continue;
        Ready result;
        result.fd = entry.fd;
        result.source = {ckv::term::WaitHandleKind::PosixFileDescriptor,
                         static_cast<std::uintptr_t>(entry.fd)};
        result.readable = (entry.revents & POLLIN) != 0;
        result.writable = (entry.revents & POLLOUT) != 0;
        result.hangup = (entry.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
        ready_.push_back(result);
    }
    outcome_ = ready_.empty() ? Outcome::TimedOut : Outcome::Ready;
    return ready_;
}

}  // namespace ckm::platform
