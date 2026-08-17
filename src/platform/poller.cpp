// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "platform/poller.hpp"

#include <cerrno>
#include <vector>

#include <poll.h>

namespace ckm::platform {

void Poller::clear() {
    fds_.clear();
    interests_.clear();
}

void Poller::watch(int fd, Interest interest) {
    if (fd < 0) return;
    fds_.push_back(fd);
    interests_.push_back(interest);
}

std::size_t Poller::watched() const noexcept { return fds_.size(); }

const std::vector<Ready>& Poller::wait(int timeout_ms) {
    ready_.clear();
    if (fds_.empty()) {
        // Nothing to wait on. A sleep is still the right answer — the caller
        // has a tick deadline — and a poll with no descriptors is exactly that,
        // so this does not need a special case beyond not touching the array.
        if (timeout_ms > 0) (void)::poll(nullptr, 0, timeout_ms);
        return ready_;
    }
    std::vector<pollfd> waiting(fds_.size());
    for (std::size_t index = 0; index < fds_.size(); ++index) {
        waiting[index].fd = fds_[index];
        waiting[index].events = 0;
        if (has(interests_[index], Interest::Read)) waiting[index].events |= POLLIN;
        if (has(interests_[index], Interest::Write)) waiting[index].events |= POLLOUT;
        waiting[index].revents = 0;
    }
    const int count = ::poll(waiting.data(), static_cast<nfds_t>(waiting.size()), timeout_ms);
    if (count <= 0) return ready_;  // a timeout, or a signal: look again next pass
    for (const pollfd& entry : waiting) {
        if (entry.revents == 0) continue;
        Ready result;
        result.fd = entry.fd;
        result.readable = (entry.revents & POLLIN) != 0;
        result.writable = (entry.revents & POLLOUT) != 0;
        result.hangup = (entry.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
        ready_.push_back(result);
    }
    return ready_;
}

}  // namespace ckm::platform
