// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/socket.hpp"
#include "platform/paths.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <utility>

namespace ckm::platform {
namespace {

bool endpoint_for(const std::filesystem::path& path, WindowsPipeEndpoint& endpoint,
                  std::string& problem) {
    WindowsPipeEndpoint identity;
    if (!windows_pipe_endpoint("default", identity, problem)) return false;
    const auto prefix = L"\\\\.\\pipe\\ckmux-" + identity.user_sid + L"-";
    std::wstring label = path.native();
    if (label.size() >= prefix.size() && ::CompareStringOrdinal(label.data(),
        static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL)
        label.erase(0, prefix.size());
    std::string instance;
    for (const wchar_t c : label) {
        if (c > 127) { problem = "the IPC instance label must be ASCII"; return false; }
        instance.push_back(static_cast<char>(c));
    }
    return windows_pipe_endpoint(instance, endpoint, problem);
}

} // namespace

std::filesystem::path socket_path() {
    const auto override = environment_path("CKMUX_SOCKET");
    if (!override.empty()) return override;
    WindowsPipeEndpoint endpoint;
    std::string problem;
    if (!windows_pipe_endpoint("default", endpoint, problem)) return {};
    return std::filesystem::path(endpoint.pipe);
}
bool socket_path_fits(const std::filesystem::path& path) {
    WindowsPipeEndpoint endpoint;
    std::string problem;
    return endpoint_for(path, endpoint, problem);
}
bool prepare_socket_directory(const std::filesystem::path& path, std::string& problem) {
    WindowsPipeEndpoint endpoint;
    // Native IPC has no directory. Its namespace is protected by a SID ACL.
    return endpoint_for(path, endpoint, problem);
}

ConnectResult connect_to_server(const std::filesystem::path& path) {
    ConnectResult result;
    WindowsPipeEndpoint endpoint;
    if (!endpoint_for(path, endpoint, result.problem)) return result;
    auto native = WindowsPipeConnect::open(endpoint);
    result.problem = std::move(native.problem);
    using Status = WindowsPipeConnect::Status;
    switch (native.status) {
        case Status::Connected:
            result.status = ConnectStatus::Connected;
            result.connection = std::move(native.connection);
            break;
        case Status::NoServer: result.status = ConnectStatus::NoServer; break;
        case Status::Busy: result.status = ConnectStatus::Busy; break;
        case Status::Denied: result.status = ConnectStatus::Denied; break;
        case Status::Failed: result.status = ConnectStatus::Unusable; break;
    }
    return result;
}
Stream ConnectResult::take_stream() { return Stream(std::move(connection)); }
Stream Listener::AcceptResult::take_stream() { return Stream(std::move(connection)); }

Listener::~Listener() { close(); }
Listener::Status Listener::listen(const std::filesystem::path& path) {
    close();
    WindowsPipeEndpoint endpoint;
    if (!endpoint_for(path, endpoint, problem_)) return Status::Failed;
    // The backend canonicalizes and authenticates the current-user namespace.
    const auto prefix = L"\\\\.\\pipe\\ckmux-" + endpoint.user_sid + L"-";
    const auto suffix = endpoint.pipe.substr(prefix.size());
    std::string instance;
    for (const wchar_t c : suffix) instance.push_back(static_cast<char>(c));
    switch (native_.listen(instance)) {
        case WindowsPipeListener::Status::Listening:
            path_ = std::filesystem::path(endpoint.pipe);
            return Status::Listening;
        case WindowsPipeListener::Status::Racing: return Status::Racing;
        case WindowsPipeListener::Status::Failed:
            problem_ = native_.problem();
            return Status::Failed;
    }
    return Status::Failed;
}
Listener::AcceptResult Listener::accept_one() {
    AcceptResult result;
    auto native = native_.accept_one();
    result.problem = std::move(native.problem);
    switch (native.status) {
        case WindowsPipeListener::AcceptStatus::Accepted:
            result.status = AcceptStatus::Accepted;
            result.connection = std::move(native.connection);
            break;
        case WindowsPipeListener::AcceptStatus::Idle: result.status = AcceptStatus::Idle; break;
        case WindowsPipeListener::AcceptStatus::Refused: result.status = AcceptStatus::Refused; break;
        case WindowsPipeListener::AcceptStatus::Failed: result.status = AcceptStatus::Failed; break;
    }
    return result;
}
ckv::term::WaitHandle Listener::wait_handle() const noexcept { return native_.wait_handle(); }
void Listener::close() noexcept { native_.close(); path_.clear(); }

Stream::Stream() = default;
Stream::Stream(WindowsPipe connection) : native_(std::make_unique<WindowsPipeStream>(std::move(connection))) {}
Stream::~Stream() = default;
Stream::Stream(Stream&& other) noexcept = default;
Stream& Stream::operator=(Stream&& other) noexcept = default;
bool Stream::send(std::string_view bytes) { return native_ && native_->send(bytes); }
bool Stream::flush() { return native_ && native_->flush(); }
bool Stream::receive(std::string& into, std::size_t budget) { return native_ && native_->receive(into, budget); }
std::size_t Stream::queued() const noexcept { return native_ ? native_->queued() : 0; }
void Stream::close() noexcept { if (native_) native_->close(); }
std::vector<WaitSource> Stream::wait_sources() const {
    if (!native_) return {};
    std::vector<WaitSource> sources;
    const auto handles = native_->wait_handles();
    for (std::size_t i = 0; i < handles.size(); ++i)
        sources.push_back({handles[i], i == 0 ? Interest::Read : Interest::Write});
    return sources;
}

} // namespace ckm::platform
