// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <cvision/term/terminal.hpp>

namespace ckm::platform {

// User-private native IPC names, not paths or integer file descriptors.
struct WindowsPipeEndpoint {
    std::wstring pipe;
    std::wstring mutex;
    std::wstring user_sid;
};

// An instance label is 1..64 ASCII letters, digits, '.', '-' or '_'.
// The host's current token supplies the SID; callers cannot select another user.
bool windows_pipe_endpoint(std::string_view instance, WindowsPipeEndpoint& endpoint,
                           std::string& problem);

// Owning pipe handle. Native I/O and readiness never narrow HANDLE to int.
class WindowsPipe {
public:
    WindowsPipe() = default;
    ~WindowsPipe();
    WindowsPipe(WindowsPipe&& other) noexcept;
    WindowsPipe& operator=(WindowsPipe&& other) noexcept;
    WindowsPipe(const WindowsPipe&) = delete;
    WindowsPipe& operator=(const WindowsPipe&) = delete;
    bool valid() const noexcept { return handle_ != nullptr; }
    void* native_handle() const noexcept { return handle_; }
    void close() noexcept;
private:
    explicit WindowsPipe(void* handle) : handle_(handle) {}
    void* handle_ = nullptr;
    friend class WindowsPipeListener;
    friend struct WindowsPipeConnect;
};

struct WindowsPipeConnect {
    enum class Status { Connected, NoServer, Busy, Denied, Failed };
    Status status = Status::Failed;
    WindowsPipe connection;
    std::string problem;
    // One nonblocking attempt. A Busy answer lets the loop retry at its deadline.
    static WindowsPipeConnect open(const WindowsPipeEndpoint& endpoint);
};

// One overlapped pending accept, rearmed before an accepted connection escapes.
// The protected startup mutex and first-instance check are both held for life.
class WindowsPipeListener {
public:
    enum class Status { Listening, Racing, Failed };
    enum class AcceptStatus { Accepted, Idle, Refused, Failed };
    struct Accepted {
        AcceptStatus status = AcceptStatus::Idle;
        WindowsPipe connection;
        std::string problem;
    };
    WindowsPipeListener();
    ~WindowsPipeListener();
    WindowsPipeListener(const WindowsPipeListener&) = delete;
    WindowsPipeListener& operator=(const WindowsPipeListener&) = delete;
    Status listen(std::string_view instance);
    Accepted accept_one();
    ckv::term::WaitHandle wait_handle() const noexcept;
    const WindowsPipeEndpoint& endpoint() const noexcept;
    const std::string& problem() const noexcept;
    void close() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

// Bounded, one-thread byte stream. Each direction has one stable overlapped
// buffer and one borrowed completion event; send/receive/flush never wait.
// Remove borrowed sources from the wait set before destroying this stream.
class WindowsPipeStream {
public:
    static constexpr std::size_t kHighWaterBytes = 4u * 1024u * 1024u;
    static constexpr std::size_t kDeltaBacklogBytes = 256u * 1024u;
    static constexpr std::size_t kHardLimitBytes = 32u * 1024u * 1024u;
    explicit WindowsPipeStream(WindowsPipe connection);
    ~WindowsPipeStream();
    WindowsPipeStream(const WindowsPipeStream&) = delete;
    WindowsPipeStream& operator=(const WindowsPipeStream&) = delete;
    bool open() const noexcept;
    bool send(std::string_view bytes);
    bool flush();
    bool receive(std::string& into, std::size_t byte_budget = 1024u * 1024u);
    std::size_t queued() const noexcept;
    // Owned chunks, bounded independently of the number of send() calls.
    std::size_t buffered_blocks() const noexcept;
    const std::string& problem() const noexcept;
    std::vector<ckv::term::WaitHandle> wait_handles() const;
    void close() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ckm::platform
