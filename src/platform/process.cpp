// Copyright (c) 2026 C. Klukas. All rights reserved.
#include "platform/process.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace ckm::platform {

bool daemonize(const std::filesystem::path& log) {
    const pid_t first = ::fork();
    if (first < 0) return false;
    if (first > 0) ::_exit(0);  // the starter's child: leaves the parent free

    if (::setsid() < 0) return false;

    // The second fork gives up session leadership, which is what makes it
    // impossible for this process to ever acquire a controlling terminal.
    const pid_t second = ::fork();
    if (second < 0) return false;
    if (second > 0) ::_exit(0);

    if (::chdir("/") != 0) return false;

    // Stdio to the log, and to /dev/null when even that cannot be opened: a
    // server whose descriptors 0, 1 and 2 are closed will hand those numbers to
    // the next thing it opens, and then a stray printf writes into a PTY.
    const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    int log_fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_fd < 0) log_fd = null_fd;
    if (null_fd >= 0) (void)::dup2(null_fd, STDIN_FILENO);
    if (log_fd >= 0) {
        (void)::dup2(log_fd, STDOUT_FILENO);
        (void)::dup2(log_fd, STDERR_FILENO);
    }
    if (null_fd >= 0 && null_fd > STDERR_FILENO) (void)::close(null_fd);
    if (log_fd >= 0 && log_fd > STDERR_FILENO && log_fd != null_fd) (void)::close(log_fd);
    return true;
}

bool start_server(const std::filesystem::path& executable, const std::filesystem::path& socket,
                  std::string& problem) {
    const pid_t child = ::fork();
    if (child < 0) {
        problem = "cannot fork to start a server: " + std::string(std::strerror(errno));
        return false;
    }
    if (child == 0) {
        const std::string program = executable.string();
        const std::string flag = "--server";
        const std::string path = socket.string();
        char* const argv[] = {const_cast<char*>(program.c_str()), const_cast<char*>(flag.c_str()),
                              const_cast<char*>(path.c_str()), nullptr};
        ::execv(program.c_str(), argv);
        // An exec that fails leaves a copy of the client running, which would
        // be worse than no server at all.
        ::_exit(127);
    }
    // The child is the first half of the double fork and exits at once; reaping
    // it here is what keeps a client from collecting zombies. Waiting for the
    // SERVER would be waiting for the whole session to end.
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        problem = "cannot run " + executable.string();
        return false;
    }
    return true;
}

std::filesystem::path executable_path(const char* argv0) {
#if defined(__APPLE__)
    std::vector<char> buffer(4096);
    std::uint32_t size = static_cast<std::uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        std::error_code error;
        const std::filesystem::path resolved =
            std::filesystem::canonical(std::filesystem::path(buffer.data()), error);
        if (!error) return resolved;
        return std::filesystem::path(buffer.data());
    }
#else
    std::error_code error;
    const std::filesystem::path resolved =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return resolved;
#endif
    return argv0 != nullptr ? std::filesystem::path(argv0) : std::filesystem::path();
}

std::filesystem::path server_log_path(const std::filesystem::path& socket) {
    std::filesystem::path log = socket;
    log += ".log";
    return log;
}

}  // namespace ckm::platform
