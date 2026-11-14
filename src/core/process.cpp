#include "process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace apctl::core {

namespace {

bool valid_flag(const char* value) {
    return value && (std::strcmp(value, "0") == 0 || std::strcmp(value, "1") == 0);
}

bool flag(const char* value) { return std::strcmp(value, "1") == 0; }

bool valid_interface_name(const char* value) {
    if (!value || !*value || std::strlen(value) > 15) return false;
    for (const unsigned char ch : std::string(value))
        if (!std::isalnum(ch) && ch != '_' && ch != '-' && ch != '.') return false;
    return true;
}

bool valid_phy_index(const char* value) {
    if (!value || !*value) return false;
    for (const unsigned char ch : std::string(value)) if (!std::isdigit(ch)) return false;
    return true;
}

bool helper_step(std::string_view name, const std::vector<std::string>& args, bool rule_add = false) {
    const auto result = run_process(args);
    std::string output = result.out + result.err;
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (rule_add && output.find("skipping adding existing rule") != std::string::npos) {
        std::cout << "APCTL_SKIP " << name << '\n';
        return false;
    }
    if (result.exit_code != 0) {
        std::cout << "APCTL_FAIL " << name << '\n';
        return false;
    }
    std::cout << "APCTL_OK " << name << '\n';
    return true;
}

}  // namespace

int run_privileged_helper(int argc, char* const argv[]) {
    if (geteuid() != 0) {
        std::cerr << "This helper must be started through system authorization.\n";
        return 1;
    }
    if (argc < 3 || std::strcmp(argv[1], "--privileged-helper") != 0) return 2;
    const std::string_view operation(argv[2]);

    if (operation == "start") {
        if (argc != 9 || !valid_flag(argv[3]) || !valid_phy_index(argv[4]) ||
            !valid_flag(argv[5]) || !valid_flag(argv[6]) || !valid_flag(argv[7]) ||
            !valid_interface_name(argv[8])) return 2;
        const std::string uplink(argv[8]);
        if (flag(argv[3]) &&
            !helper_step("interface-create", {"iw", "phy", "phy" + std::string(argv[4]),
                                               "interface", "add", "ap0", "type", "__ap"})) return 1;
        if (!helper_step("networkmanager-unmanage", {"nmcli", "device", "set", "ap0", "managed", "no"})) return 1;
        const bool interface_ready =
            helper_step("interface-down", {"ip", "link", "set", "dev", "ap0", "down"}) &&
            helper_step("interface-mode", {"iw", "dev", "ap0", "set", "type", "__ap"}) &&
            helper_step("interface-address", {"ip", "link", "set", "dev", "ap0", "address", "02:88:cd:08:b8:c8"});
        const bool managed = helper_step("networkmanager-manage", {"nmcli", "device", "set", "ap0", "managed", "yes"});
        if (!interface_ready || !managed) return 1;
        if (flag(argv[5]) && !helper_step("dhcp", {"ufw", "allow", "in", "on", "ap0", "to", "any", "port", "67", "proto", "udp"}, true)) return 1;
        if (flag(argv[6]) && !helper_step("dns", {"ufw", "allow", "in", "on", "ap0", "to", "any", "port", "53", "proto", "udp"}, true)) return 1;
        if (flag(argv[7]) && !helper_step("route", {"ufw", "route", "allow", "in", "on", "ap0", "out", "on", uplink,
                                                       "from", "10.42.0.0/24", "to", "any"}, true)) return 1;
        return 0;
    }

    if (operation == "firewall-add") {
        if (argc != 7 || !valid_flag(argv[3]) || !valid_flag(argv[4]) ||
            !valid_flag(argv[5]) || !valid_interface_name(argv[6])) return 2;
        const std::string uplink(argv[6]);
        if (flag(argv[3]) && !helper_step("dhcp", {"ufw", "allow", "in", "on", "ap0", "to", "any", "port", "67", "proto", "udp"}, true)) return 1;
        if (flag(argv[4]) && !helper_step("dns", {"ufw", "allow", "in", "on", "ap0", "to", "any", "port", "53", "proto", "udp"}, true)) return 1;
        if (flag(argv[5]) && !helper_step("route", {"ufw", "route", "allow", "in", "on", "ap0", "out", "on", uplink,
                                                     "from", "10.42.0.0/24", "to", "any"}, true)) return 1;
        return 0;
    }

    if (operation == "interface-prepare") {
        if (argc != 3) return 2;
        if (!helper_step("networkmanager-unmanage", {"nmcli", "device", "set", "ap0", "managed", "no"})) return 1;
        const bool interface_ready =
            helper_step("interface-down", {"ip", "link", "set", "dev", "ap0", "down"}) &&
            helper_step("interface-mode", {"iw", "dev", "ap0", "set", "type", "__ap"}) &&
            helper_step("interface-address", {"ip", "link", "set", "dev", "ap0", "address", "02:88:cd:08:b8:c8"});
        const bool managed = helper_step("networkmanager-manage", {"nmcli", "device", "set", "ap0", "managed", "yes"});
        return interface_ready && managed ? 0 : 1;
    }

    if (operation == "stop") {
        if (argc != 8 || !valid_flag(argv[3]) || !valid_flag(argv[4]) ||
            !valid_flag(argv[5]) || !valid_interface_name(argv[6]) || !valid_flag(argv[7])) return 2;
        const std::string uplink(argv[6]);
        bool ok = true;
        if (flag(argv[3]) && !helper_step("dhcp-delete", {"ufw", "delete", "allow", "in", "on", "ap0", "to", "any", "port", "67", "proto", "udp"})) ok = false;
        if (flag(argv[4]) && !helper_step("dns-delete", {"ufw", "delete", "allow", "in", "on", "ap0", "to", "any", "port", "53", "proto", "udp"})) ok = false;
        if (flag(argv[5]) && !helper_step("route-delete", {"ufw", "route", "delete", "allow", "in", "on", "ap0", "out", "on", uplink,
                                                            "from", "10.42.0.0/24", "to", "any"})) ok = false;
        if (flag(argv[7])) {
            if (!helper_step("interface-down", {"ip", "link", "set", "dev", "ap0", "down"})) ok = false;
            if (!helper_step("interface-delete", {"iw", "dev", "ap0", "del"})) ok = false;
        }
        return ok ? 0 : 1;
    }
    return 2;
}

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
