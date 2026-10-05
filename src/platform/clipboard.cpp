// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/clipboard.hpp"

#include <algorithm>
#include <cstdint>

namespace ckm::platform {
namespace {
constexpr std::size_t kDiagnosticsCap = 4096;

void append_diagnostic(std::string& destination, std::string_view bytes) {
    if (bytes.empty() || destination.size() >= kDiagnosticsCap) return;
    if (!destination.empty()) destination.push_back('\n');
    destination.append(bytes.substr(0, kDiagnosticsCap - destination.size()));
}
}  // namespace

bool write_to_command(ckv::core::ProcessRunner& runner, const ckv::core::ProcessLaunchSpec& launch,
                      std::string_view text, std::string* diagnostics, int idle_budget_ms) {
    if (diagnostics != nullptr) diagnostics->clear();
    ckv::core::ProcessRunRequest request;
    request.launch = launch;
    request.input = text;
    // Clipboard holders (for example xclip) can legitimately outlive their
    // successful root. Failed/idle helpers never get the release policy.
    request.descendants = ckv::core::ProcessDescendantPolicy::ReleaseOnSuccess;
    request.idle_budget_nanos = static_cast<std::int64_t>(idle_budget_ms) * 1'000'000;
    request.max_stdout_bytes = kDiagnosticsCap;
    request.max_stderr_bytes = kDiagnosticsCap;
    const auto result = runner.run(request);
    const bool successful = result.successful();
    if (diagnostics != nullptr) {
        // Prefer the problem stream, but retain stdout too when there is room.
        // Byte capture, native lifecycle and cap draining belong to ckVision.
        append_diagnostic(*diagnostics, result.stderr_capture.bytes);
        append_diagnostic(*diagnostics, result.stdout_capture.bytes);
        if (!successful) append_diagnostic(*diagnostics, result.diagnostic);
        if (!successful && diagnostics->empty() && result.exit) {
            const char* kind = result.exit->kind == ckv::core::ProcessExitKind::Signal ? "signal " : "status ";
            append_diagnostic(*diagnostics, std::string("helper exited with ") + kind + std::to_string(result.exit->code));
        }
        if (!successful && diagnostics->empty()) append_diagnostic(*diagnostics, "helper did not complete the copy");
    }
    return successful;
}

}  // namespace ckm::platform
