// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/windows_pipe.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/term/windows_clock.hpp"
#include "cvision/term/windows_wait_set.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <array>
#include <cstring>
#include <set>
#include <vector>
#include <filesystem>

namespace {

std::string label() {
    static unsigned serial = 0;
    return "native-test-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(++serial);
}

void wait_accept(ckm::platform::WindowsPipeListener& listener) {
    CK_CHECK(::WaitForSingleObject(reinterpret_cast<HANDLE>(listener.wait_handle().value), 2000) == WAIT_OBJECT_0);
}

DWORD thread_count() {
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    CK_CHECK(snapshot != INVALID_HANDLE_VALUE);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    DWORD count = 0;
    if (::Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID == ::GetCurrentProcessId()) ++count;
    } while (::Thread32Next(snapshot, &entry));
    ::CloseHandle(snapshot);
    return count;
}

} // namespace

CK_TEST(windows_pipe_names_are_current_user_scoped_and_invalid_labels_fail_closed) {
    ckm::platform::WindowsPipeEndpoint endpoint;
    std::string problem;
    CK_CHECK(ckm::platform::windows_pipe_endpoint("default", endpoint, problem));
    CK_CHECK(endpoint.user_sid.starts_with(L"S-1-"));
    CK_CHECK(endpoint.pipe == L"\\\\.\\pipe\\ckmux-" + endpoint.user_sid + L"-default");
    CK_CHECK(endpoint.mutex == L"Global\\ckmux-" + endpoint.user_sid + L"-default-startup");
    ckm::platform::WindowsPipeEndpoint uppercase;
    CK_CHECK(ckm::platform::windows_pipe_endpoint("DEFAULT", uppercase, problem));
    CK_CHECK(uppercase.pipe == endpoint.pipe);
    CK_CHECK(uppercase.mutex == endpoint.mutex);
    for (const auto& bad : std::array<std::string, 5>{"", "../other", "bad\\name", std::string(65, 'a'), std::string("a\0b", 3)}) {
        CK_CHECK(!ckm::platform::windows_pipe_endpoint(bad, endpoint, problem));
        CK_CHECK(endpoint.pipe.empty());
        CK_CHECK(!problem.empty());
    }
}

CK_TEST(windows_pipe_accept_is_nonblocking_authenticated_and_rearmed) {
    using Listener = ckm::platform::WindowsPipeListener;
    using Connect = ckm::platform::WindowsPipeConnect;
    Listener listener;
    CK_CHECK(listener.listen(label()) == Listener::Status::Listening);
    CK_CHECK(listener.accept_one().status == Listener::AcceptStatus::Idle);
    CK_CHECK(::WaitForSingleObject(reinterpret_cast<HANDLE>(listener.wait_handle().value), 0) == WAIT_TIMEOUT);
    for (unsigned i = 0; i != 3; ++i) {
        auto client = Connect::open(listener.endpoint());
        CK_CHECK(client.status == Connect::Status::Connected);
        wait_accept(listener);
        auto server = listener.accept_one();
        CK_CHECK(server.status == Listener::AcceptStatus::Accepted);
        CK_CHECK(server.connection.valid());
        if (!server.connection.valid() || !client.connection.valid()) return;
        DWORD flags = 0;
        CK_CHECK(::GetHandleInformation(server.connection.native_handle(), &flags) != 0);
        CK_CHECK((flags & HANDLE_FLAG_INHERIT) == 0);
        DWORD pipe_flags = 0;
        CK_CHECK(::GetNamedPipeInfo(server.connection.native_handle(), &pipe_flags, nullptr, nullptr, nullptr) != 0);
        CK_CHECK((pipe_flags & PIPE_TYPE_MESSAGE) == 0);
        CK_CHECK(listener.accept_one().status == Listener::AcceptStatus::Idle);
    }
}

CK_TEST(windows_pipe_startup_has_one_winner_and_shutdown_allows_a_fresh_server) {
    using Listener = ckm::platform::WindowsPipeListener;
    const auto instance = label();
    Listener first;
    Listener second;
    CK_CHECK(first.listen(instance) == Listener::Status::Listening);
    CK_CHECK(second.listen(instance) == Listener::Status::Racing);
    first.close();
    CK_CHECK(second.listen(instance) == Listener::Status::Listening);
    second.close();
    second.close();
}

CK_TEST(windows_pipe_client_reports_missing_server_and_rejects_foreign_sid) {
    using Connect = ckm::platform::WindowsPipeConnect;
    ckm::platform::WindowsPipeEndpoint endpoint;
    std::string problem;
    CK_CHECK(ckm::platform::windows_pipe_endpoint(label(), endpoint, problem));
    CK_CHECK(Connect::open(endpoint).status == Connect::Status::NoServer);
    endpoint.user_sid = L"S-1-5-18";
    CK_CHECK(Connect::open(endpoint).status == Connect::Status::Denied);
}

CK_TEST(windows_pipe_acl_is_protected_and_grants_only_this_user_and_system) {
    using Listener = ckm::platform::WindowsPipeListener;
    Listener listener;
    CK_CHECK(listener.listen(label()) == Listener::Status::Listening);
    auto client = ckm::platform::WindowsPipeConnect::open(listener.endpoint());
    wait_accept(listener);
    auto accepted = listener.accept_one();
    CK_CHECK(accepted.connection.valid());
    if (!accepted.connection.valid()) return;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    CK_CHECK(::GetSecurityInfo(accepted.connection.native_handle(), SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS);
    if (!descriptor) return;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    CK_CHECK(::GetSecurityDescriptorControl(descriptor, &control, &revision) != 0);
    CK_CHECK((control & SE_DACL_PROTECTED) != 0);
    CK_CHECK(acl != nullptr && acl->AceCount == 2);
    std::set<std::wstring> trustees;
    if (acl) for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* raw = nullptr;
        CK_CHECK(::GetAce(acl, i, &raw) != 0);
        if (!raw) continue;
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        CK_CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
        LPWSTR sid = nullptr;
        CK_CHECK(::ConvertSidToStringSidW(const_cast<DWORD*>(&ace->SidStart), &sid) != 0);
        if (sid) { trustees.insert(sid); ::LocalFree(sid); }
    }
    CK_CHECK(trustees == std::set<std::wstring>({L"S-1-5-18", listener.endpoint().user_sid}));
    ::LocalFree(descriptor);
}

CK_TEST(windows_pipe_ready_sources_exceed_64_without_transport_threads) {
    using Listener = ckm::platform::WindowsPipeListener;
    const DWORD before = thread_count();
    std::vector<std::unique_ptr<Listener>> listeners;
    std::vector<ckm::platform::WindowsPipe> clients;
    std::vector<ckv::term::WaitHandle> sources;
    for (unsigned i = 0; i != 130; ++i) {
        auto listener = std::make_unique<Listener>();
        CK_CHECK(listener->listen(label()) == Listener::Status::Listening);
        auto client = ckm::platform::WindowsPipeConnect::open(listener->endpoint());
        CK_CHECK(client.connection.valid());
        sources.push_back(listener->wait_handle());
        clients.push_back(std::move(client.connection));
        listeners.push_back(std::move(listener));
    }
    ckv::term::WindowsClock clock;
    ckv::term::WindowsWaitSet waits(clock);
    std::set<std::uintptr_t> observed;
    const auto deadline = clock.now_nanos() + 2'000'000'000;
    while (observed.size() != sources.size() && clock.now_nanos() < deadline) {
        for (const auto source : waits.wait(deadline, sources)) observed.insert(source.value);
    }
    CK_CHECK(observed.size() == 130);
    waits.clear(); // unregister before accept replaces the borrowed events
    CK_CHECK(thread_count() == before);
    for (auto& listener : listeners)
        CK_CHECK(listener->accept_one().status == Listener::AcceptStatus::Accepted);
}

CK_TEST(windows_pipe_stream_delivers_binary_bytes_in_both_directions_with_small_read_budgets) {
    using Listener = ckm::platform::WindowsPipeListener;
    Listener listener;
    CK_CHECK(listener.listen(label()) == Listener::Status::Listening);
    auto connected = ckm::platform::WindowsPipeConnect::open(listener.endpoint());
    wait_accept(listener);
    auto accepted = listener.accept_one();
    ckm::platform::WindowsPipeStream client(std::move(connected.connection));
    ckm::platform::WindowsPipeStream server(std::move(accepted.connection));
    std::string bytes(150000, '\0');
    for (std::size_t i = 0; i != bytes.size(); ++i) bytes[i] = static_cast<char>(i % 251);
    CK_CHECK(client.send(bytes));
    CK_CHECK(server.send("ack"));
    ckv::term::WindowsClock clock;
    const auto deadline = clock.now_nanos() + 2'000'000'000;
    std::string received;
    std::string acknowledgement;
    while ((received.size() != bytes.size() || acknowledgement.size() != 3) && clock.now_nanos() < deadline) {
        const auto old_size = received.size();
        CK_CHECK(server.receive(received, 137));
        CK_CHECK(received.size() - old_size <= 137);
        CK_CHECK(client.receive(acknowledgement));
        CK_CHECK(client.flush());
        CK_CHECK(server.flush());
    }
    CK_CHECK(received == bytes);
    CK_CHECK(acknowledgement == "ack");
    CK_CHECK(client.queued() == 0);
}

CK_TEST(windows_pipe_stream_backpressure_preserves_buffer_identity_and_all_queued_bytes) {
    using Listener = ckm::platform::WindowsPipeListener;
    Listener listener;
    CK_CHECK(listener.listen(label()) == Listener::Status::Listening);
    auto connected = ckm::platform::WindowsPipeConnect::open(listener.endpoint());
    wait_accept(listener);
    auto accepted = listener.accept_one();
    ckm::platform::WindowsPipeStream client(std::move(connected.connection));
    ckm::platform::WindowsPipeStream server(std::move(accepted.connection));
    const std::string first(5u * 1024u * 1024u, 'a');
    const std::string second(512u * 1024u, 'b');
    const std::string tiny(4096, 'c');
    CK_CHECK(!client.send(first));
    CK_CHECK(client.open());
    CK_CHECK(client.queued() > ckm::platform::WindowsPipeStream::kHighWaterBytes);
    CK_CHECK(!client.send(second));
    for (const char c : tiny) CK_CHECK(!client.send(std::string_view(&c, 1)));
    CK_CHECK(client.buffered_blocks() <= client.queued() / 65536u + 2u);
    CK_CHECK(client.queued() <= ckm::platform::WindowsPipeStream::kHardLimitBytes);
    const auto before = thread_count();
    ckv::term::WindowsClock clock;
    const auto deadline = clock.now_nanos() + 5'000'000'000;
    std::string received;
    while (received.size() != first.size() + second.size() + tiny.size() && clock.now_nanos() < deadline) {
        CK_CHECK(server.receive(received, 65536));
        CK_CHECK(client.flush());
    }
    CK_CHECK(received == first + second + tiny);
    CK_CHECK(client.queued() == 0);
    CK_CHECK(thread_count() == before);
    CK_CHECK(!client.send(std::string(ckm::platform::WindowsPipeStream::kHardLimitBytes + 1, 'x')));
    CK_CHECK(!client.open());
    CK_CHECK(!client.problem().empty());
}

CK_TEST(windows_pipe_stream_pending_read_signals_disconnect_and_closes) {
    using Listener = ckm::platform::WindowsPipeListener;
    Listener listener;
    CK_CHECK(listener.listen(label()) == Listener::Status::Listening);
    auto connected = ckm::platform::WindowsPipeConnect::open(listener.endpoint());
    wait_accept(listener);
    auto accepted = listener.accept_one();
    ckm::platform::WindowsPipeStream client(std::move(connected.connection));
    ckm::platform::WindowsPipeStream server(std::move(accepted.connection));
    const auto sources = server.wait_handles();
    CK_CHECK(sources.size() == 1);
    client.close();
    CK_CHECK(::WaitForSingleObject(reinterpret_cast<HANDLE>(sources.front().value), 2000) == WAIT_OBJECT_0);
    std::string bytes;
    CK_CHECK(!server.receive(bytes));
    CK_CHECK(!server.open());
    CK_CHECK(server.wait_handles().empty());
    server.close();
}

CK_TEST(windows_pipe_failed_write_preserves_unread_completed_reply) {
    using Listener = ckm::platform::WindowsPipeListener;
    Listener listener;
    CK_CHECK(listener.listen(label()) == Listener::Status::Listening);
    auto connected = ckm::platform::WindowsPipeConnect::open(listener.endpoint());
    wait_accept(listener);
    auto accepted = listener.accept_one();
    ckm::platform::WindowsPipeStream client(std::move(connected.connection));
    ckm::platform::WindowsPipeStream server(std::move(accepted.connection));
    CK_CHECK(server.send("first"));
    CK_CHECK(server.send("second"));
    ckv::term::WindowsClock clock;
    const auto deadline = clock.now_nanos() + 2'000'000'000;
    while (server.queued() != 0 && clock.now_nanos() < deadline) CK_CHECK(server.flush());
    CK_CHECK(server.queued() == 0);
    server.close();
    CK_CHECK(!client.send("a request to the closed sender"));
    CK_CHECK(client.open());
    CK_CHECK(client.queued() == 0);
    CK_CHECK(client.buffered_blocks() == 0);
    CK_CHECK(client.wait_handles().size() == 1);
    CK_CHECK(!client.flush());
    CK_CHECK(!client.send("another doomed request"));
    CK_CHECK(client.queued() == 0);
    std::string received;
    while (client.open() && clock.now_nanos() < deadline) (void)client.receive(received, 3);
    CK_CHECK(received == "firstsecond");
    CK_CHECK(!client.open());
}

CK_TEST(windows_pipe_startup_race_uses_eight_real_processes) {
    const auto instance = label();
    const auto event_base = L"Local\\" + std::wstring(instance.begin(), instance.end());
    const auto go_name = event_base + L"-go";
    const auto stop_name = event_base + L"-stop";
    const HANDLE go = ::CreateEventW(nullptr, TRUE, FALSE, go_name.c_str());
    const HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
    std::array<HANDLE, 8> ready{};
    std::array<HANDLE, 8> processes{};
    std::wstring executable(32768, L'\0');
    executable.resize(::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    executable = (std::filesystem::path(executable).parent_path() / L"ckmux_ipc_race_probe.exe").wstring();
    for (std::size_t i = 0; i != ready.size(); ++i) {
        const auto ready_name = event_base + L"-ready-" + std::to_wstring(i);
        ready[i] = ::CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
        std::wstring command = L"\"" + executable + L"\" " + std::wstring(instance.begin(), instance.end()) +
            L" " + go_name + L" " + ready_name + L" " + stop_name;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        CK_CHECK(::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0);
        processes[i] = process.hProcess;
        if (process.hThread) ::CloseHandle(process.hThread);
    }
    CK_CHECK(::SetEvent(go) != 0);
    for (const auto event : ready) CK_CHECK(::WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
    CK_CHECK(::SetEvent(stop) != 0);
    unsigned winners = 0;
    unsigned racers = 0;
    for (const auto process : processes) {
        CK_CHECK(::WaitForSingleObject(process, 5000) == WAIT_OBJECT_0);
        DWORD code = 0;
        CK_CHECK(::GetExitCodeProcess(process, &code) != 0);
        winners += code == 10 ? 1u : 0u;
        racers += code == 11 ? 1u : 0u;
        ::CloseHandle(process);
    }
    CK_CHECK(winners == 1);
    CK_CHECK(racers == 7);
    for (const auto event : ready) ::CloseHandle(event);
    ::CloseHandle(go);
    ::CloseHandle(stop);
}

CK_TEST(windows_pipe_first_instance_guard_refuses_an_existing_uncoordinated_pipe) {
    const auto instance = label();
    ckm::platform::WindowsPipeEndpoint endpoint;
    std::string problem;
    CK_CHECK(ckm::platform::windows_pipe_endpoint(instance, endpoint, problem));
    const HANDLE squatter = ::CreateNamedPipeW(endpoint.pipe.c_str(), PIPE_ACCESS_DUPLEX |
        FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    CK_CHECK(squatter != INVALID_HANDLE_VALUE);
    ckm::platform::WindowsPipeListener listener;
    CK_CHECK(listener.listen(instance) == ckm::platform::WindowsPipeListener::Status::Failed);
    CK_CHECK(!listener.problem().empty());
    CK_CHECK(listener.wait_handle().value == 0);
    ::CloseHandle(squatter);
    CK_CHECK(listener.listen(instance) == ckm::platform::WindowsPipeListener::Status::Listening);
}

CK_TEST(windows_pipe_pending_accept_and_both_io_directions_retire_all_owned_handles) {
    DWORD before = 0;
    CK_CHECK(::GetProcessHandleCount(::GetCurrentProcess(), &before) != 0);
    for (unsigned i = 0; i != 50; ++i) {
        ckm::platform::WindowsPipeListener listener;
        CK_CHECK(listener.listen(label()) == ckm::platform::WindowsPipeListener::Status::Listening);
        auto connected = ckm::platform::WindowsPipeConnect::open(listener.endpoint());
        wait_accept(listener);
        auto accepted = listener.accept_one();
        ckm::platform::WindowsPipeStream client(std::move(connected.connection));
        ckm::platform::WindowsPipeStream server(std::move(accepted.connection));
        CK_CHECK(client.send(std::string(128u * 1024u, 'x')));
        CK_CHECK(server.send(std::string(128u * 1024u, 'y')));
        // Neither reads; destruction cancels pending accept/read/write requests.
    }
    DWORD after = 0;
    CK_CHECK(::GetProcessHandleCount(::GetCurrentProcess(), &after) != 0);
    CK_CHECK(after == before);
}
