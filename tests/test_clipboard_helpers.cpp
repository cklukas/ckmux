// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "platform/clipboard.hpp"
#include "platform/shell_host.hpp"
#include "cvision/term/process_runner.hpp"
#include "cvision/testing/cktest.hpp"
#if defined(_WIN32)
#include "cvision/term/windows_clock.hpp"
using HelperClock = ckv::term::WindowsClock;
#else
#include "cvision/term/posix_clock.hpp"
using HelperClock = ckv::term::PosixClock;
#endif

namespace {
using namespace ckv::core;
class ObservedHelper final : public ProcessRunner {
public:
    ProcessRunResult answer;
    ProcessLaunchSpec launch;
    std::string input;
    ProcessDescendantPolicy descendants = ProcessDescendantPolicy::Unspecified;
    std::int64_t budget = 0;
    std::size_t stdout_limit = 0, stderr_limit = 0;
    unsigned calls = 0;
    ProcessRunResult run(const ProcessRunRequest& request) override {
        ++calls;
        launch = request.launch;
        input = request.input;
        descendants = request.descendants;
        budget = request.idle_budget_nanos;
        stdout_limit = request.max_stdout_bytes;
        stderr_limit = request.max_stderr_bytes;
        auto result = answer;
        result.input_bytes_total = request.input.size();
        if (complete_input) result.input_bytes_written = request.input.size();
        return result;
    }
    bool complete_input = true;
};
}

CK_TEST(clipboard_helper_delegates_binary_input_and_explicit_holder_policy_to_the_library) {
    ObservedHelper runner;
    runner.answer.state = ProcessRunState::Completed;
    runner.answer.exit = ProcessExitStatus{ProcessExitKind::Normal, 0};
    auto launch = ProcessLaunchSpec::program("explicit-program", {"trusted-command"});
    launch.working_directory = "explicit-directory";
    const std::string selected("private\0\xff\r\n", 11);
    std::string diagnostic = "old failure";
    CK_CHECK(ckm::platform::write_to_command(runner, launch, selected, &diagnostic));
    CK_CHECK(runner.calls == 1 && runner.input == selected);
    CK_CHECK(runner.launch.executable == launch.executable && runner.launch.arguments == launch.arguments);
    CK_CHECK(runner.launch.working_directory == "explicit-directory");
    CK_CHECK(runner.descendants == ProcessDescendantPolicy::ReleaseOnSuccess);
    CK_CHECK(runner.budget == 2'000'000'000);
    CK_CHECK(runner.stdout_limit == 4096 && runner.stderr_limit == 4096);
    CK_CHECK(diagnostic.empty());
}

CK_TEST(clipboard_helper_never_accepts_unknown_exit_partial_input_or_timeout_as_success) {
    ObservedHelper runner;
    const auto launch = ProcessLaunchSpec::program("explicit-program");
    runner.answer.state = ProcessRunState::Completed;
    std::string diagnostic;
    CK_CHECK(!ckm::platform::write_to_command(runner, launch, "selection", &diagnostic));
    CK_CHECK(!diagnostic.empty());
    runner.answer.exit = ProcessExitStatus{ProcessExitKind::Normal, 0};
    runner.complete_input = false;
    CK_CHECK(!ckm::platform::write_to_command(runner, launch, "selection", &diagnostic));
    runner.complete_input = true;
    runner.answer.state = ProcessRunState::IdleTimeout;
    runner.answer.diagnostic = "helper idle timeout";
    CK_CHECK(!ckm::platform::write_to_command(runner, launch, "selection", &diagnostic, 150));
    CK_CHECK(diagnostic == "helper idle timeout" && runner.budget == 150'000'000);
}

CK_TEST(clipboard_helper_preserves_observed_output_even_when_the_root_succeeds) {
    ObservedHelper runner;
    runner.answer.state = ProcessRunState::Completed;
    runner.answer.exit = ProcessExitStatus{ProcessExitKind::Normal, 0};
    runner.answer.stderr_capture.bytes = "observed stderr";
    runner.answer.stdout_capture.bytes = "observed stdout";
    std::string diagnostic;
    CK_CHECK(ckm::platform::write_to_command(runner, ProcessLaunchSpec::program("explicit-program"), "", &diagnostic));
    CK_CHECK(diagnostic == "observed stderr\nobserved stdout");
}

CK_TEST(clipboard_helper_bounds_private_diagnostics_and_prefers_the_problem_stream) {
    ObservedHelper runner;
    runner.answer.state = ProcessRunState::Completed;
    runner.answer.exit = ProcessExitStatus{ProcessExitKind::Normal, 7};
    runner.answer.stderr_capture.bytes = std::string(4096, 'E');
    runner.answer.stdout_capture.bytes = std::string(4096, 'O');
    runner.answer.diagnostic = "native failure";
    std::string diagnostic;
    CK_CHECK(!ckm::platform::write_to_command(runner, ProcessLaunchSpec::program("explicit-program"), "", &diagnostic));
    CK_CHECK(diagnostic == std::string(4096, 'E'));
    runner.answer.stderr_capture.bytes = "problem";
    runner.answer.stdout_capture.bytes = "context";
    CK_CHECK(!ckm::platform::write_to_command(runner, ProcessLaunchSpec::program("explicit-program"), "", &diagnostic));
    CK_CHECK(diagnostic == "problem\ncontext\nnative failure");
}

CK_TEST(clipboard_helper_native_system_command_processor_has_private_success_and_failure) {
    HelperClock clock;
    ckv::term::NativeProcessRunner runner(clock);
    const auto host = ckm::platform::shell_host();
    CK_CHECK(!host.system_shell.empty());
    auto invocation = [&](const std::string& command) {
#if defined(_WIN32)
        auto launch = ProcessLaunchSpec::windows_command_processor(host.system_shell, command);
        launch.working_directory = "C:/";
        return launch;
#else
        return ProcessLaunchSpec::program(host.system_shell, {"-c", command});
#endif
    };
    std::string diagnostic = "old failure";
#if defined(_WIN32)
    CK_CHECK(ckm::platform::write_to_command(runner, invocation("exit /b 0"), "", &diagnostic));
    CK_CHECK(diagnostic.empty());
    CK_CHECK(!ckm::platform::write_to_command(runner, invocation("echo helper-out & echo helper-error 1>&2 & exit /b 7"), "", &diagnostic));
#else
    CK_CHECK(ckm::platform::write_to_command(runner, invocation("exit 0"), "", &diagnostic));
    CK_CHECK(diagnostic.empty());
    CK_CHECK(!ckm::platform::write_to_command(runner, invocation("echo helper-out; echo helper-error >&2; exit 7"), "", &diagnostic));
#endif
    CK_CHECK(diagnostic.find("helper-out") != std::string::npos);
    CK_CHECK(diagnostic.find("helper-error") != std::string::npos);
}
