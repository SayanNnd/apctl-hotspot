#include "process.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace apctl::core {

ProcessResult run_process(const std::vector<std::string>& args) {
    if (args.empty()) return {-1, {}, "No command supplied."};
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0) return {-1, {}, std::strerror(errno)};
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return {-1, {}, std::strerror(errno)};
    }

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return {-1, {}, "Couldn't prepare the command."};
    }
    (void)posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    (void)posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
    (void)posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    (void)posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    (void)posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
    (void)posix_spawn_file_actions_addclose(&actions, err_pipe[1]);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawn_error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    (void)posix_spawn_file_actions_destroy(&actions);

    close(out_pipe[1]);
    close(err_pipe[1]);
    if (spawn_error != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        return {-1, {}, std::strerror(spawn_error)};
    }

    ProcessResult result;
    std::array<char, 4096> buffer{};
    pollfd fds[2]{{out_pipe[0], POLLIN | POLLHUP, 0}, {err_pipe[0], POLLIN | POLLHUP, 0}};
    bool open[2]{true, true};
    while (open[0] || open[1]) {
        const int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (!open[i] || (fds[i].revents & (POLLIN | POLLHUP)) == 0) continue;
            const ssize_t count = read(fds[i].fd, buffer.data(), buffer.size());
            if (count > 0) {
                (i == 0 ? result.out : result.err).append(buffer.data(), static_cast<size_t>(count));
            } else {
                close(fds[i].fd);
                open[i] = false;
            }
        }
    }
    for (int i = 0; i < 2; ++i) if (open[i]) close(fds[i].fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return {-1, std::move(result.out), std::strerror(errno)};
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return result;
}

}  // namespace apctl::core
