// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/poller.hpp"
#include "cvision/term/windows_clock.hpp"
#include "cvision/term/windows_wait_set.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <limits>
#include <system_error>

namespace ckm::platform {

struct Poller::State {
    ckv::term::WindowsClock clock;
    ckv::term::WindowsWaitSet waits{clock};
    std::vector<WaitSource> watching;
};

Poller::Poller() : state_(std::make_unique<State>()) {}
Poller::~Poller() = default;
void Poller::clear() { state_->waits.clear(); state_->watching.clear(); }
void Poller::watch(ckv::term::WaitHandle handle, Interest interest) {
    if (handle.kind != ckv::term::WaitHandleKind::WindowsHandle || handle.value == 0) return;
    const auto found = std::find_if(state_->watching.begin(), state_->watching.end(),
        [handle](const WaitSource& source) { return source.handle == handle; });
    if (found == state_->watching.end()) state_->watching.push_back({handle, interest});
    else found->interest = found->interest | interest;
}
std::size_t Poller::watched() const noexcept { return state_->watching.size(); }

const std::vector<Ready>& Poller::wait(int timeout_ms) {
    ready_.clear();
    if (state_->watching.empty()) {
        ::Sleep(timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
        outcome_ = Outcome::TimedOut;
        return ready_;
    }
    std::vector<ckv::term::WaitHandle> handles;
    for (const auto& source : state_->watching) handles.push_back(source.handle);
    const auto now = state_->clock.now_nanos();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto duration = static_cast<std::int64_t>(std::max(0, timeout_ms)) * 1'000'000;
    const auto deadline = timeout_ms < 0 || now > maximum - duration ? maximum : now + duration;
    try {
        for (const auto handle : state_->waits.wait(deadline, handles)) {
            const auto found = std::find_if(state_->watching.begin(), state_->watching.end(),
                [handle](const WaitSource& source) { return source.handle == handle; });
            if (found == state_->watching.end()) continue;
            Ready result;
            result.source = handle;
            result.readable = has(found->interest, Interest::Read);
            result.writable = has(found->interest, Interest::Write);
            ready_.push_back(result);
        }
        // No borrowed registration escapes the wait. Accept/reconnect/teardown
        // may close and replace native events before the next loop iteration.
        state_->waits.clear();
        outcome_ = ready_.empty() ? Outcome::TimedOut : Outcome::Ready;
    } catch (const std::system_error&) {
        state_->waits.clear();
        outcome_ = Outcome::Failed;
    }
    return ready_;
}

} // namespace ckm::platform
