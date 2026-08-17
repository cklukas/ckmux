// Copyright (c) 2026 C. Klukas. All rights reserved.
//
// The fd-wait seam (plans/02-architecture.md). `poll()` is enough for the
// target scale — tens of descriptors — and it lives behind these few lines so
// that epoll or kqueue can replace it later without a line of loop logic
// changing.
//
// Deliberately not a general reactor: no callbacks, no ownership of
// descriptors, no lifetime rules. The loop asks what is ready and decides what
// that means, which keeps the interesting logic in one readable function
// instead of scattered across handlers.
#pragma once

#include <cstdint>
#include <vector>

namespace ckm::platform {

enum class Interest : std::uint8_t {
    Read = 1u << 0,
    Write = 1u << 1,
};

constexpr Interest operator|(Interest a, Interest b) noexcept {
    return static_cast<Interest>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool has(Interest set, Interest flag) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(flag)) != 0;
}

struct Ready {
    int fd = -1;
    bool readable = false;
    bool writable = false;
    // The peer went away, or the descriptor is not one any more. Reported
    // rather than folded into `readable`, because a caller that reads on a
    // hangup gets zero bytes and has to work out why, while a caller told
    // "hung up" can say so.
    bool hangup = false;
};

class Poller {
public:
    void clear();
    void watch(int fd, Interest interest);

    // Waits up to `timeout_ms` (negative waits indefinitely) and returns what
    // is ready. An interrupted wait returns nothing rather than an error: the
    // caller's next pass around its loop will look again, which is what a
    // signal arriving during a wait should cost.
    const std::vector<Ready>& wait(int timeout_ms);

    std::size_t watched() const noexcept;

private:
    // Kept as parallel arrays of what poll() wants and what a caller asked
    // for, so a wait allocates nothing.
    std::vector<int> fds_;
    std::vector<Interest> interests_;
    std::vector<Ready> ready_;
};

}  // namespace ckm::platform
