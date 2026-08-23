// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The fd-wait seam (the architecture spec). `poll()` is enough for the
// target scale — tens of descriptors — and it lives behind these few lines so
// that epoll or kqueue can replace it later without a line of loop logic
// changing.
//
// Deliberately not a general reactor: no callbacks, no ownership of
// descriptors, no lifetime rules. The loop asks what is ready and decides what
// that means, which keeps the interesting logic in one readable function
// instead of scattered across handlers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <poll.h>

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
    // Why the last wait returned the set it did. Three of these produce an
    // empty set, and a caller that cannot tell them apart cannot behave: a
    // descriptor the system refuses to poll — one that was closed while still
    // being watched — makes every wait return instantly with nothing in it,
    // which is indistinguishable from a timeout that took its whole timeout.
    // That is a loop at 100 % CPU for the life of the process.
    enum class Outcome {
        Ready,        // something in the set is ready
        TimedOut,     // the timeout expired with nothing ready
        Interrupted,  // a signal arrived; look again next pass
        Failed,       // poll() itself failed, and will fail the same way again
    };

    void clear();
    void watch(int fd, Interest interest);

    // Waits up to `timeout_ms` for a watched descriptor and returns what is
    // ready — empty for any of the three reasons above, with `outcome()`
    // saying which. An interrupted wait is not an error: the caller's next pass
    // around its loop will look again, which is what a signal arriving during
    // a wait should cost.
    //
    // A negative timeout waits indefinitely, with no descriptors watched as
    // with any number of them: a caller that asked to sleep until something
    // happens is not served by being handed back an empty set at once.
    const std::vector<Ready>& wait(int timeout_ms);

    Outcome outcome() const noexcept { return outcome_; }

    std::size_t watched() const noexcept;

private:
    // What poll() wants, filled in by watch() and kept between waits so that a
    // wait allocates nothing once the set has reached its usual size. A kqueue
    // or epoll implementation would replace this member and nothing else: no
    // caller can see it.
    std::vector<pollfd> watching_;
    std::vector<Ready> ready_;
    Outcome outcome_ = Outcome::TimedOut;
};

}  // namespace ckm::platform
