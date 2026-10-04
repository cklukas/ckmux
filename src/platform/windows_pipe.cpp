// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/windows_pipe.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <utility>
#include <vector>
#include <array>
#include <algorithm>
#include <deque>
#include <cstring>

namespace ckm::platform {
namespace {

struct NativeHandle {
    HANDLE value = nullptr;
    ~NativeHandle() { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
};

std::string native_problem(std::string_view operation, DWORD error = ::GetLastError()) {
    return std::string(operation) + " failed (Windows error " + std::to_string(error) + ")";
}

bool process_sid(HANDLE process, std::wstring& result) {
    NativeHandle token;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
    DWORD size = 0;
    (void)::GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (size == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<unsigned char> buffer(size);
    if (!::GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size)) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(user->User.Sid, &text)) return false;
    result = text;
    ::LocalFree(text);
    return true;
}

bool peer_is_user(HANDLE pipe, const std::wstring& expected, bool server_end) {
    ULONG pid = 0;
    const BOOL queried = server_end ? ::GetNamedPipeClientProcessId(pipe, &pid)
                                    : ::GetNamedPipeServerProcessId(pipe, &pid);
    if (!queried || pid == 0) return false;
    NativeHandle process{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process.value) return false;
    std::wstring actual;
    return process_sid(process.value, actual) && actual == expected;
}

} // namespace

bool windows_pipe_endpoint(std::string_view instance, WindowsPipeEndpoint& endpoint,
                           std::string& problem) {
    endpoint = {};
    problem.clear();
    if (instance.empty() || instance.size() > 64) {
        problem = "the IPC instance label must contain 1 to 64 characters";
        return false;
    }
    for (const char c : instance) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
            problem = "the IPC instance label contains an invalid character";
            return false;
        }
    }
    if (!process_sid(::GetCurrentProcess(), endpoint.user_sid)) {
        problem = native_problem("reading the current user SID");
        return false;
    }
    // Pipe names are case-insensitive, mutex object names are case-sensitive.
    // Canonicalize the label once so those two locks protect the same name.
    std::wstring label;
    for (const char c : instance)
        label.push_back(static_cast<wchar_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    const std::wstring name = L"ckmux-" + endpoint.user_sid + L"-" + label;
    endpoint.pipe = L"\\\\.\\pipe\\" + name;
    endpoint.mutex = L"Global\\" + name + L"-startup";
    return true;
}

WindowsPipe::~WindowsPipe() { close(); }
WindowsPipe::WindowsPipe(WindowsPipe&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}
WindowsPipe& WindowsPipe::operator=(WindowsPipe&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}
void WindowsPipe::close() noexcept {
    if (handle_) {
        (void)::CancelIoEx(handle_, nullptr);
        ::CloseHandle(handle_);
        handle_ = nullptr;
    }
}

WindowsPipeConnect WindowsPipeConnect::open(const WindowsPipeEndpoint& endpoint) {
    WindowsPipeConnect result;
    // Validate against the host token again, not a caller-supplied SID assertion.
    WindowsPipeEndpoint actual;
    std::string problem;
    if (!windows_pipe_endpoint("default", actual, problem) ||
        endpoint.user_sid != actual.user_sid ||
        !endpoint.pipe.starts_with(L"\\\\.\\pipe\\ckmux-" + actual.user_sid + L"-")) {
        result.status = Status::Denied;
        result.problem = "the IPC endpoint is not scoped to this user";
        return result;
    }
    HANDLE handle = ::CreateFileW(endpoint.pipe.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
        SECURITY_IDENTIFICATION, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        result.status = error == ERROR_FILE_NOT_FOUND ? Status::NoServer
                      : error == ERROR_PIPE_BUSY ? Status::Busy
                      : error == ERROR_ACCESS_DENIED ? Status::Denied : Status::Failed;
        if (result.status == Status::Denied || result.status == Status::Failed)
            result.problem = native_problem("opening the server pipe", error);
        return result;
    }
    if (!peer_is_user(handle, actual.user_sid, false)) {
        ::CloseHandle(handle);
        result.status = Status::Denied;
        result.problem = "the pipe server's token does not identify this user";
        return result;
    }
    result.connection = WindowsPipe(handle);
    result.status = Status::Connected;
    return result;
}

struct WindowsPipeListener::State {
    WindowsPipeEndpoint endpoint;
    std::string problem;
    HANDLE mutex = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED accept{};
    PSECURITY_DESCRIPTOR security = nullptr;
    bool pending = false;
    bool connected = false;

    bool arm(bool first) {
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security, FALSE};
        pipe = ::CreateNamedPipeW(endpoint.pipe.c_str(), PIPE_ACCESS_DUPLEX |
            FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &attributes);
        if (pipe == INVALID_HANDLE_VALUE) {
            problem = native_problem("creating the server pipe");
            return false;
        }
        accept = {};
        accept.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!accept.hEvent) {
            problem = native_problem("creating the accept event");
            return false;
        }
        const BOOL done = ::ConnectNamedPipe(pipe, &accept);
        const DWORD error = done ? ERROR_SUCCESS : ::GetLastError();
        pending = error == ERROR_IO_PENDING;
        connected = done || error == ERROR_PIPE_CONNECTED;
        if (connected) (void)::SetEvent(accept.hEvent);
        if (!pending && !connected) {
            problem = native_problem("waiting for a pipe client", error);
            return false;
        }
        return true;
    }

    void clear_accept() noexcept {
        if (pipe != INVALID_HANDLE_VALUE) {
            if (pending) {
                (void)::CancelIoEx(pipe, &accept);
                DWORD ignored = 0;
                (void)::GetOverlappedResult(pipe, &accept, &ignored, TRUE);
            }
            ::CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (accept.hEvent) ::CloseHandle(accept.hEvent);
        accept = {};
        pending = false;
        connected = false;
    }
};

WindowsPipeListener::WindowsPipeListener() : state_(std::make_unique<State>()) {}
WindowsPipeListener::~WindowsPipeListener() { close(); }
WindowsPipeListener::Status WindowsPipeListener::listen(std::string_view instance) {
    close();
    if (!windows_pipe_endpoint(instance, state_->endpoint, state_->problem)) return Status::Failed;
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + state_->endpoint.user_sid + L")";
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
            &state_->security, nullptr)) {
        state_->problem = native_problem("creating the private IPC ACL");
        return Status::Failed;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), state_->security, FALSE};
    state_->mutex = ::CreateMutexW(&attributes, TRUE, state_->endpoint.mutex.c_str());
    const DWORD error = ::GetLastError();
    if (!state_->mutex) {
        state_->problem = native_problem("creating the startup mutex", error);
        return Status::Failed;
    }
    if (error == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(state_->mutex);
        state_->mutex = nullptr;
        return Status::Racing;
    }
    if (state_->arm(true)) return Status::Listening;
    close();
    return Status::Failed;
}

WindowsPipeListener::Accepted WindowsPipeListener::accept_one() {
    Accepted result;
    if (state_->pipe == INVALID_HANDLE_VALUE || !state_->accept.hEvent) {
        result.status = AcceptStatus::Failed;
        result.problem = "the pipe listener is not open";
        return result;
    }
    if (state_->pending) {
        DWORD ignored = 0;
        if (!::GetOverlappedResult(state_->pipe, &state_->accept, &ignored, FALSE)) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_IO_INCOMPLETE) return result;
            result.status = AcceptStatus::Failed;
            result.problem = native_problem("accepting a pipe client", error);
            return result;
        }
        state_->pending = false;
        state_->connected = true;
    }
    if (!state_->connected) return result;
    WindowsPipe connection(state_->pipe);
    state_->pipe = INVALID_HANDLE_VALUE;
    ::CloseHandle(state_->accept.hEvent);
    state_->accept = {};
    state_->connected = false;
    const bool authenticated = peer_is_user(connection.native_handle(), state_->endpoint.user_sid, true);
    // Keep an instance alive throughout rearming, including rejected clients.
    if (!state_->arm(false)) {
        result.status = AcceptStatus::Failed;
        result.problem = state_->problem;
        return result;
    }
    if (!authenticated) {
        result.status = AcceptStatus::Refused;
        result.problem = "the pipe client's token does not identify this user";
        return result;
    }
    result.status = AcceptStatus::Accepted;
    result.connection = std::move(connection);
    return result;
}

ckv::term::WaitHandle WindowsPipeListener::wait_handle() const noexcept {
    return {ckv::term::WaitHandleKind::WindowsHandle,
            reinterpret_cast<std::uintptr_t>(state_->accept.hEvent)};
}
const WindowsPipeEndpoint& WindowsPipeListener::endpoint() const noexcept { return state_->endpoint; }
const std::string& WindowsPipeListener::problem() const noexcept { return state_->problem; }
void WindowsPipeListener::close() noexcept {
    state_->clear_accept();
    if (state_->mutex) {
        (void)::ReleaseMutex(state_->mutex);
        ::CloseHandle(state_->mutex);
        state_->mutex = nullptr;
    }
    if (state_->security) {
        ::LocalFree(state_->security);
        state_->security = nullptr;
    }
}

struct WindowsPipeStream::State {
    WindowsPipe pipe;
    OVERLAPPED read{};
    OVERLAPPED write{};
    std::array<char, 65536> read_buffer{};
    std::array<char, 65536> write_buffer{};
    std::deque<std::string> outgoing;
    std::string problem;
    std::size_t outgoing_offset = 0;
    std::size_t queued_bytes = 0;
    std::size_t read_count = 0;
    std::size_t read_offset = 0;
    bool reading = false;
    bool writing = false;

    void stop() noexcept {
        if (pipe.valid()) {
            (void)::CancelIoEx(pipe.native_handle(), nullptr);
            DWORD ignored = 0;
            if (reading) (void)::GetOverlappedResult(pipe.native_handle(), &read, &ignored, TRUE);
            if (writing) (void)::GetOverlappedResult(pipe.native_handle(), &write, &ignored, TRUE);
            pipe.close();
        }
        if (read.hEvent) ::CloseHandle(read.hEvent);
        if (write.hEvent) ::CloseHandle(write.hEvent);
        read = {};
        write = {};
        reading = false;
        writing = false;
        outgoing.clear();
        queued_bytes = 0;
        read_count = 0;
        read_offset = 0;
    }

    bool fail(std::string_view operation, DWORD error = ::GetLastError()) {
        problem = native_problem(operation, error);
        stop();
        return false;
    }

    bool arm_read() {
        if (!pipe.valid()) return false;
        (void)::ResetEvent(read.hEvent);
        const BOOL done = ::ReadFile(pipe.native_handle(), read_buffer.data(),
            static_cast<DWORD>(read_buffer.size()), nullptr, &read);
        if (!done && ::GetLastError() != ERROR_IO_PENDING) return fail("reading the pipe");
        reading = true;
        return true;
    }
};

WindowsPipeStream::WindowsPipeStream(WindowsPipe connection) : state_(std::make_unique<State>()) {
    state_->pipe = std::move(connection);
    if (!state_->pipe.valid()) { state_->problem = "the pipe connection is not open"; return; }
    state_->read.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state_->write.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state_->read.hEvent || !state_->write.hEvent) {
        (void)state_->fail("creating the pipe I/O events");
        return;
    }
    (void)state_->arm_read();
}
WindowsPipeStream::~WindowsPipeStream() { close(); }
bool WindowsPipeStream::open() const noexcept { return state_->pipe.valid(); }
std::size_t WindowsPipeStream::queued() const noexcept { return state_->queued_bytes; }
std::size_t WindowsPipeStream::buffered_blocks() const noexcept { return state_->outgoing.size(); }
const std::string& WindowsPipeStream::problem() const noexcept { return state_->problem; }
void WindowsPipeStream::close() noexcept { state_->stop(); }

bool WindowsPipeStream::send(std::string_view bytes) {
    if (!open()) return false;
    // Refuse the whole frame and disconnect rather than allocating an unbounded
    // queue or dropping a prefix while claiming the connection remains sound.
    if (bytes.size() > kHardLimitBytes - queued()) {
        state_->problem = "the pipe peer exceeded the outbound hard limit";
        close();
        return false;
    }
    const std::size_t added = bytes.size();
    while (!bytes.empty()) {
        if (state_->outgoing.empty() || state_->outgoing.back().size() == state_->write_buffer.size())
            state_->outgoing.emplace_back();
        auto& tail = state_->outgoing.back();
        const auto count = std::min(bytes.size(), state_->write_buffer.size() - tail.size());
        tail.append(bytes.data(), count);
        bytes.remove_prefix(count);
    }
    state_->queued_bytes += added;
    return flush() && queued() <= kHighWaterBytes;
}

bool WindowsPipeStream::flush() {
    if (!open()) return false;
    // Bounded work even when the pipe accepts data continuously.
    std::size_t budget = 1024u * 1024u;
    while (budget != 0) {
        if (state_->writing) {
            DWORD written = 0;
            if (!::GetOverlappedResult(state_->pipe.native_handle(), &state_->write, &written, FALSE)) {
                if (::GetLastError() == ERROR_IO_INCOMPLETE) return true;
                return state_->fail("writing the pipe");
            }
            state_->writing = false;
            if (written == 0 || written > state_->queued_bytes) return state_->fail("writing the pipe", ERROR_WRITE_FAULT);
            state_->outgoing_offset += written;
            state_->queued_bytes -= written;
            budget -= std::min(budget, static_cast<std::size_t>(written));
            if (state_->outgoing_offset == state_->outgoing.front().size()) {
                state_->outgoing.pop_front();
                state_->outgoing_offset = 0;
            }
        }
        if (state_->outgoing.empty()) {
            (void)::ResetEvent(state_->write.hEvent);
            return true;
        }
        // The queue may grow while this write is pending; the native buffer
        // cannot move with it and is untouched until its completion arrives.
        const auto& front = state_->outgoing.front();
        const auto count = std::min(state_->write_buffer.size(), front.size() - state_->outgoing_offset);
        std::memcpy(state_->write_buffer.data(), front.data() + state_->outgoing_offset, count);
        (void)::ResetEvent(state_->write.hEvent);
        const BOOL done = ::WriteFile(state_->pipe.native_handle(), state_->write_buffer.data(),
            static_cast<DWORD>(count), nullptr, &state_->write);
        if (!done && ::GetLastError() != ERROR_IO_PENDING) return state_->fail("writing the pipe");
        state_->writing = true;
    }
    return true;
}

bool WindowsPipeStream::receive(std::string& into, std::size_t byte_budget) {
    if (!open()) return false;
    while (byte_budget != 0) {
        if (state_->reading) {
            DWORD count = 0;
            if (!::GetOverlappedResult(state_->pipe.native_handle(), &state_->read, &count, FALSE)) {
                if (::GetLastError() == ERROR_IO_INCOMPLETE) return true;
                return state_->fail("reading the pipe");
            }
            state_->reading = false;
            if (count == 0) { close(); return false; }
            state_->read_count = count;
            state_->read_offset = 0;
        }
        const auto count = std::min(byte_budget, state_->read_count - state_->read_offset);
        into.append(state_->read_buffer.data() + state_->read_offset, count);
        state_->read_offset += count;
        byte_budget -= count;
        if (state_->read_offset == state_->read_count) {
            state_->read_count = 0;
            state_->read_offset = 0;
            if (!state_->arm_read()) return false;
        }
    }
    return true;
}

std::vector<ckv::term::WaitHandle> WindowsPipeStream::wait_handles() const {
    if (!open()) return {};
    std::vector<ckv::term::WaitHandle> sources{{ckv::term::WaitHandleKind::WindowsHandle,
        reinterpret_cast<std::uintptr_t>(state_->read.hEvent)}};
    if (state_->writing || queued() != 0) sources.push_back({ckv::term::WaitHandleKind::WindowsHandle,
        reinterpret_cast<std::uintptr_t>(state_->write.hEvent)});
    return sources;
}

} // namespace ckm::platform
