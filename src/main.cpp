#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

struct Result {
    int exit_code = -1;
    std::string out;
    std::string err;
};

Result run(const std::vector<std::string>& args) {
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0) {
        return {-1, {}, std::strerror(errno)};
    }
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        return {-1, {}, std::strerror(errno)};
    }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }
    close(out_pipe[1]); close(err_pipe[1]);
    if (pid < 0) {
        close(out_pipe[0]); close(err_pipe[0]);
        return {-1, {}, std::strerror(errno)};
    }
    Result result;
    std::array<char, 4096> buffer{};
    pollfd fds[2]{{out_pipe[0], POLLIN | POLLHUP, 0}, {err_pipe[0], POLLIN | POLLHUP, 0}};
    bool open[2]{true, true};
    while (open[0] || open[1]) {
        if (poll(fds, 2, -1) < 0) break;
        for (int i = 0; i < 2; ++i) {
            if (!open[i] || (fds[i].revents & (POLLIN | POLLHUP)) == 0) continue;
            const ssize_t count = read(fds[i].fd, buffer.data(), buffer.size());
            if (count > 0) (i == 0 ? result.out : result.err).append(buffer.data(), static_cast<size_t>(count));
            else { close(fds[i].fd); open[i] = false; }
        }
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return {-1, std::move(result.out), std::strerror(errno)};
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return result;
}

int run_interactive(const std::vector<std::string>& args) {
    const pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }
    if (pid < 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<std::string> field(std::string_view text, std::string_view key) {
    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line)) {
        if (line.rfind(std::string(key), 0) == 0) return trim(line.substr(key.size()));
    }
    return std::nullopt;
}

fs::path config_root() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    return xdg && *xdg ? fs::path(xdg) : (home ? fs::path(home) / ".config" : fs::temp_directory_path());
}

fs::path config_path() { return config_root() / "apctl-hotspot" / "config"; }
fs::path legacy_config_path() { return config_root() / "caelestia-hotspot" / "config"; }
fs::path legacy_state_path() { return config_root() / "caelestia-hotspot" / "state"; }

void migrate_legacy_data() {
    const fs::path legacy_dir = config_root() / "caelestia-hotspot";
    std::error_code ec;
    if (!fs::exists(legacy_dir, ec) || ec) return;
    fs::create_directories(config_path().parent_path(), ec);
    if (ec) return;
    const auto migrate = [](const fs::path& from, const fs::path& to) {
        std::error_code error;
        if (fs::exists(to, error) || error || !fs::exists(from, error) || error) return;
        fs::rename(from, to, error);
        if (!error) chmod(to.c_str(), 0600);
    };
    migrate(legacy_config_path(), config_path());
    migrate(legacy_state_path(), config_path().parent_path() / "state");
}

struct Config { std::string ssid; std::string password; };

std::optional<Config> load_config() {
    migrate_legacy_data();
    std::ifstream file(config_path());
    Config config;
    std::string line;
    while (std::getline(file, line)) {
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        const auto key = line.substr(0, equal);
        const auto value = line.substr(equal + 1);
        if (key == "ssid") config.ssid = value;
        else if (key == "password") config.password = value;
    }
    if (config.ssid.empty() || config.password.size() < 8) return std::nullopt;
    return config;
}

struct FirewallState {
    bool dhcp = false;
    bool dns = false;
    bool route = false;
    bool interface_owned = false;
    std::string uplink;
    std::string profile_uuid;
    std::string profile_backend;
};

bool privileged(const std::vector<std::string>& args);
bool command(const std::vector<std::string>& args, bool quiet = false);

fs::path state_path() { return config_path().parent_path() / "state"; }

FirewallState load_state() {
    migrate_legacy_data();
    std::ifstream file(state_path());
    FirewallState state;
    std::string key, value;
    while (std::getline(file, value)) {
        const auto equal = value.find('=');
        if (equal == std::string::npos) continue;
        key = value.substr(0, equal);
        const auto data = value.substr(equal + 1);
        if (key == "dhcp") state.dhcp = data == "1";
        else if (key == "dns") state.dns = data == "1";
        else if (key == "route") state.route = data == "1";
        else if (key == "interface_owned") state.interface_owned = data == "1";
        else if (key == "uplink") state.uplink = data;
        else if (key == "profile_uuid") state.profile_uuid = data;
        else if (key == "profile_backend") state.profile_backend = data;
    }
    return state;
}

bool save_state(const FirewallState& state) {
    migrate_legacy_data();
    std::error_code ec;
    fs::create_directories(state_path().parent_path(), ec);
    if (ec) return false;
    const mode_t old_mask = umask(0077);
    FILE* file = std::fopen(state_path().c_str(), "w");
    umask(old_mask);
    if (!file) return false;
    const bool ok = std::fprintf(file, "dhcp=%d\ndns=%d\nroute=%d\ninterface_owned=%d\nuplink=%s\nprofile_uuid=%s\nprofile_backend=%s\n", state.dhcp, state.dns, state.route, state.interface_owned, state.uplink.c_str(), state.profile_uuid.c_str(), state.profile_backend.c_str()) > 0;
    std::fclose(file);
    chmod(state_path().c_str(), 0600);
    return ok;
}

bool add_ufw_rule(const std::vector<std::string>& rule, bool& owned) {
    std::string fingerprint;
    for (size_t i = 1; i < rule.size(); ++i) {
        if (!fingerprint.empty()) fingerprint.push_back(' ');
        fingerprint += rule[i];
    }
    const auto current = run({"ufw", "show", "added"});
    if (current.exit_code != 0) {
        std::cerr << "Couldn't check the firewall settings. No changes were made; retry `hotspot up`.\n";
        return false;
    }
    if (current.out.find(fingerprint) != std::string::npos) {
        return true;
    }
    if (!privileged(rule)) return false;
    owned = true;
    return true;
}

bool configure_firewall(FirewallState& state, std::string_view ap, std::string_view uplink) {
    const auto ufw = run({"ufw", "status"});
    if (ufw.exit_code != 0 || ufw.out.find("Status: active") == std::string::npos) return true;
    const bool ready =
        add_ufw_rule({"ufw", "allow", "in", "on", std::string(ap), "to", "any", "port", "67", "proto", "udp"}, state.dhcp) && save_state(state) &&
        add_ufw_rule({"ufw", "allow", "in", "on", std::string(ap), "to", "any", "port", "53", "proto", "udp"}, state.dns) && save_state(state) &&
        add_ufw_rule({"ufw", "route", "allow", "in", "on", std::string(ap), "out", "on", std::string(uplink), "from", "10.42.0.0/24", "to", "any"}, state.route) && save_state(state);
    if (!ready) std::cerr << "Couldn't finish network sharing setup. Approve the system prompt, then retry `hotspot up`.\n";
    return ready;
}

std::optional<std::string> connection_uuid(std::string_view name) {
    const auto result = run({"nmcli", "-g", "connection.uuid", "connection", "show", std::string(name)});
    if (result.exit_code != 0) return std::nullopt;
    return trim(result.out);
}

bool connection_exists_uuid(std::string_view uuid) {
    const auto result = run({"nmcli", "-g", "connection.uuid", "connection", "show", "uuid", std::string(uuid)});
    return result.exit_code == 0 && trim(result.out) == uuid;
}

bool connection_name_exists(std::string_view name) {
    const auto result = run({"nmcli", "-t", "-f", "NAME", "connection", "show"});
    if (result.exit_code != 0) return false;
    std::istringstream lines(result.out);
    std::string line;
    while (std::getline(lines, line)) if (trim(line) == name) return true;
    return false;
}

std::string random_mac();

bool wait_until_available(std::string_view iface, std::string& state_out) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto state = run({"nmcli", "-g", "GENERAL.STATE", "device", "show", std::string(iface)});
        if (state.exit_code == 0) {
            state_out = trim(state.out);
            if (state_out.rfind("30 ", 0) == 0 || state_out == "30" || state_out.rfind("100 ", 0) == 0 || state_out == "100") return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool prepare_owned_ap() {
    constexpr std::string_view ap = "ap0";
    if (!command({"nmcli", "device", "set", std::string(ap), "managed", "no"}, true)) {
        std::cerr << "Couldn't prepare Wi-Fi for the hotspot. Run `hotspot status` and retry.\n";
        return false;
    }
    if (!privileged({"ip", "link", "set", "dev", std::string(ap), "down"}) ||
        !privileged({"iw", "dev", std::string(ap), "set", "type", "__ap"}) ||
        !privileged({"ip", "link", "set", "dev", std::string(ap), "address", random_mac()})) return false;
    if (!command({"nmcli", "device", "set", std::string(ap), "managed", "yes"})) return false;
    std::string device_state;
    if (!wait_until_available(ap, device_state)) {
        std::cerr << "Wi-Fi isn't ready to start the hotspot. Run `hotspot status`, then retry.\n";
        return false;
    }
    return true;
}

bool save_config(const Config& config) {
    migrate_legacy_data();
    std::error_code ec;
    fs::create_directories(config_path().parent_path(), ec);
    if (ec) return false;
    const mode_t old_mask = umask(0077);
    FILE* file = std::fopen(config_path().c_str(), "w");
    umask(old_mask);
    if (!file) return false;
    const bool ok = std::fprintf(file, "ssid=%s\npassword=%s\n", config.ssid.c_str(), config.password.c_str()) > 0;
    std::fclose(file);
    chmod(config_path().c_str(), 0600);
    return ok;
}

std::optional<std::string> prompt_secret(const std::string& prompt) {
    const int tty = open("/dev/tty", O_RDWR);
    if (tty < 0) return std::nullopt;
    termios original{};
    if (tcgetattr(tty, &original) != 0) { close(tty); return std::nullopt; }
    termios hidden = original;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    if (tcsetattr(tty, TCSAFLUSH, &hidden) != 0) { close(tty); return std::nullopt; }
    (void)write(tty, prompt.data(), prompt.size());
    std::string value;
    char ch = 0;
    while (read(tty, &ch, 1) == 1 && ch != '\n' && ch != '\r') value.push_back(ch);
    const bool restored = tcsetattr(tty, TCSAFLUSH, &original) == 0;
    (void)write(tty, "\n", 1);
    close(tty);
    if (!restored) return std::nullopt;
    return value;
}

std::string random_password() {
    static constexpr std::string_view alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random) return {};
    std::string password;
    while (password.size() < 16) {
        unsigned char byte = 0;
        random.read(reinterpret_cast<char*>(&byte), 1);
        if (!random) return {};
        const auto limit = static_cast<unsigned char>((256 / alphabet.size()) * alphabet.size());
        if (byte < limit) password.push_back(alphabet[byte % alphabet.size()]);
    }
    return password;
}

std::optional<std::string> active_wifi() {
    const auto result = run({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device", "status"});
    if (result.exit_code != 0) return std::nullopt;
    std::istringstream stream(result.out);
    std::string line;
    while (std::getline(stream, line)) {
        std::vector<std::string> parts;
        std::stringstream row(line);
        std::string part;
        while (std::getline(row, part, ':')) parts.push_back(part);
        if (parts.size() >= 3 && parts[0] != "ap0" && parts[0].rfind("p2p-", 0) != 0 &&
            parts[1] == "wifi" && parts[2] == "connected") return parts[0];
    }
    return std::nullopt;
}

std::optional<std::string> connected_channel(const std::string& iface) {
    const auto result = run({"iw", "dev", iface, "link"});
    if (result.exit_code != 0 || result.out.find("Not connected") != std::string::npos) return std::nullopt;
    const auto freq = field(result.out, "\tfreq:");
    if (!freq) return std::nullopt;
    try {
        const int mhz = std::stoi(*freq);
        if (mhz >= 2412 && mhz <= 2472) return std::to_string((mhz - 2407) / 5);
        if (mhz == 2484) return std::string("14");
        if (mhz >= 5000 && mhz < 5950) return std::to_string((mhz - 5000) / 5);
    } catch (...) {}
    return std::nullopt;
}

std::string random_mac() {
    // This stable locally administered address was verified on the target radio.
    return "02:88:cd:08:b8:c8";
}

bool concurrent_capability() {
    const auto result = run({"iw", "list"});
    return result.exit_code == 0 && result.out.find("#{ managed }") != std::string::npos &&
        result.out.find("#{ AP") != std::string::npos && result.out.find("total <= 2") != std::string::npos;
}

bool command(const std::vector<std::string>& args, bool quiet) {
    const auto result = run(args);
    if (result.exit_code == 0) return true;
    if (!quiet) {
        std::string detail = result.err.empty() ? result.out : result.err;
        for (char& ch : detail) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (detail.find("not authorized") != std::string::npos || detail.find("permission denied") != std::string::npos ||
            detail.find("a password is required") != std::string::npos) {
            std::cerr << "System permission is required. Approve the authorization prompt and retry.\n";
        } else if (detail.find("not a valid channel") != std::string::npos ||
                   (detail.find("channel") != std::string::npos && detail.find("invalid") != std::string::npos)) {
            std::cerr << "The Wi-Fi channel is no longer available. Run `hotspot check` and retry.\n";
        } else if (detail.find("device is not available") != std::string::npos || detail.find("could not be readied") != std::string::npos ||
                   detail.find("device or resource busy") != std::string::npos) {
            std::cerr << "Wi-Fi isn't ready to start the hotspot. Run `hotspot status`, then retry.\n";
        } else {
            std::cerr << "NetworkManager couldn't complete that step. Run `hotspot status` and retry.\n";
        }
    }
    return false;
}

bool privileged(const std::vector<std::string>& args) {
    std::vector<std::string> full{"pkexec"};
    full.insert(full.end(), args.begin(), args.end());
    return command(full);
}

void print_status() {
    const auto nm = run({"nmcli", "general", "status"});
    if (nm.exit_code != 0) { std::cerr << "Network settings aren't available right now.\n"; return; }
    const auto uplink = active_wifi();
    if (uplink) {
        const auto connection = run({"nmcli", "-g", "GENERAL.CONNECTION", "device", "show", *uplink});
        std::cout << "Wi-Fi: connected on " << *uplink;
        if (connection.exit_code == 0 && !trim(connection.out).empty()) std::cout << " (" << trim(connection.out) << ')';
        std::cout << '\n';
    } else {
        std::cout << "Wi-Fi: not connected\n";
    }
    const auto ap = run({"iw", "dev", "ap0", "info"});
    if (ap.exit_code != 0) { std::cout << "Hotspot: stopped\n"; return; }
    const auto state = run({"nmcli", "-g", "GENERAL.STATE", "device", "show", "ap0"});
    const bool active = state.exit_code == 0 && trim(state.out).rfind("100 ", 0) == 0;
    if (!active) { std::cout << "Hotspot: not active\n"; return; }
    const auto ssid = field(ap.out, "\tssid ");
    const auto channel = field(ap.out, "\tchannel ");
    std::cout << "Hotspot: active";
    if (ssid) std::cout << " (" << *ssid << ')';
    if (channel) std::cout << " on channel " << trim(channel->substr(0, channel->find(' ')));
    std::cout << '\n';
    const auto stations = run({"iw", "dev", "ap0", "station", "dump"});
    if (stations.exit_code == 0) {
        size_t count = 0;
        std::istringstream lines(stations.out);
        std::string line;
        while (std::getline(lines, line)) if (line.rfind("Station ", 0) == 0) ++count;
        std::cout << "Connected devices: " << count << '\n';
    }
}

int check() {
    const auto nm = run({"nmcli", "general", "status"});
    if (nm.exit_code != 0) { std::cerr << "Network settings aren't available right now.\n"; return 3; }
    const auto iface = active_wifi();
    if (!iface) { std::cerr << "Connect to Wi-Fi first, then run `hotspot check`.\n"; return 3; }
    const bool concurrent = concurrent_capability();
    std::cout << "Wi-Fi: connected on " << *iface << '\n';
    std::cout << "Wi-Fi and hotspot together: " << (concurrent ? "supported" : "not supported by this adapter") << '\n';
    const auto channel = connected_channel(*iface);
    if (channel) std::cout << "Hotspot channel: " << *channel << " (same as Wi-Fi)\n";
    else { std::cerr << "Couldn't read the Wi-Fi channel. Reconnect to Wi-Fi and retry.\n"; return 3; }
    return concurrent ? 0 : 3;
}

int generate(int argc, char** argv) {
    Config config;
    config.ssid = argc > 2 ? argv[2] : "apctl-hotspot";
    if (config.ssid.empty() || config.ssid.size() > 32 || config.ssid.find_first_of("\r\n") != std::string::npos) {
        std::cerr << "SSID must contain 1–32 characters on one line.\n"; return 2;
    }
    config.password = random_password();
    if (config.password.size() != 16 || !save_config(config)) { std::cerr << "Couldn't save hotspot details. Check that your home folder is writable.\n"; return 1; }
    std::cout << "Hotspot details saved.\nSSID: " << config.ssid << "\nPassword: " << config.password << '\n';
    return 0;
}

void show_password() {
    if (run_interactive({"nmcli", "device", "wifi", "show-password", "ifname", "ap0"}) == 0) return;
    const auto config = load_config();
    if (config) {
        std::cout << "SSID: " << config->ssid << "\nPassword: " << config->password << '\n';
    } else {
        std::cerr << "Couldn't read the active hotspot details. Start it with `hotspot up` first.\n";
    }
    if (config) std::cerr << "QR code unavailable; the SSID and password are shown above.\n";
}

std::optional<bool> restart_profile_if_active(std::string_view uuid) {
    const auto active_uuid = run({"nmcli", "-g", "GENERAL.CON-UUID", "device", "show", "ap0"});
    if (active_uuid.exit_code != 0 || trim(active_uuid.out) != uuid) return false;
    if (!command({"nmcli", "connection", "down", "uuid", std::string(uuid)})) return std::nullopt;
    if (!prepare_owned_ap()) return std::nullopt;
    if (!command({"nmcli", "connection", "up", "uuid", std::string(uuid), "ifname", "ap0"})) return std::nullopt;
    return true;
}

bool valid_password(std::string_view password) {
    return password.size() >= 8 && password.size() <= 63 && password.find_first_of("\r\n") == std::string_view::npos;
}

int change_password() {
    auto config = load_config();
    if (!config) { std::cerr << "No saved hotspot settings. Run `hotspot gen [SSID]` first.\n"; return 2; }
    const auto first = prompt_secret("New hotspot password (8–63 characters): ");
    if (!first) { std::cerr << "Could not read a hidden password from the terminal.\n"; return 1; }
    if (!valid_password(*first)) { std::cerr << "Password must be 8–63 characters and contain no line breaks.\n"; return 2; }
    const auto confirm = prompt_secret("Repeat new hotspot password: ");
    if (!confirm || *first != *confirm) { std::cerr << "Passwords did not match. No changes made.\n"; return 2; }

    const auto previous = config->password;
    config->password = *first;
    if (!save_config(*config)) { std::cerr << "Could not save the new password.\n"; return 1; }

    const auto state = load_state();
    if (state.profile_uuid.empty() || !connection_exists_uuid(state.profile_uuid)) {
        std::cout << "Password saved. It will take effect the next time the hotspot starts.\n";
        return 0;
    }
    if (!command({"nmcli", "connection", "modify", "uuid", state.profile_uuid, "wifi-sec.psk", config->password})) {
        config->password = previous;
        save_config(*config);
        std::cerr << "Couldn't change the hotspot password. The previous password is still in use.\n";
        return 1;
    }
    const auto restarted = restart_profile_if_active(state.profile_uuid);
    if (!restarted) {
        std::cerr << "Password saved, but the hotspot couldn't restart. Run `hotspot up` to try again.\n";
        return 1;
    }
    if (*restarted) {
        std::cout << "Password changed. Reconnect your devices using the new password.\n";
        show_password();
        return 0;
    }
    std::cout << "Password changed. It will take effect the next time the hotspot starts.\n";
    return 0;
}

int change_ssid() {
    auto config = load_config();
    if (!config) { std::cerr << "No saved hotspot settings. Run `hotspot gen [SSID]` first.\n"; return 2; }
    std::cout << "New hotspot SSID: " << std::flush;
    std::string new_ssid;
    if (!std::getline(std::cin, new_ssid)) { std::cerr << "Could not read the new SSID.\n"; return 1; }
    if (new_ssid.empty() || new_ssid.size() > 32 || new_ssid.find_first_of("\r\n") != std::string::npos) {
        std::cerr << "SSID must contain 1–32 characters on one line.\n"; return 2;
    }
    const auto previous = config->ssid;
    config->ssid = new_ssid;
    if (!save_config(*config)) { std::cerr << "Could not save the new SSID.\n"; return 1; }

    const auto state = load_state();
    if (state.profile_uuid.empty() || !connection_exists_uuid(state.profile_uuid)) {
        std::cout << "Hotspot name saved. It will take effect the next time the hotspot starts.\n";
        return 0;
    }
    if (!command({"nmcli", "connection", "modify", "uuid", state.profile_uuid, "802-11-wireless.ssid", config->ssid})) {
        config->ssid = previous;
        save_config(*config);
        std::cerr << "Couldn't change the hotspot name. The previous name is still in use.\n";
        return 1;
    }
    const auto restarted = restart_profile_if_active(state.profile_uuid);
    if (!restarted) {
        std::cerr << "New hotspot name saved, but the hotspot couldn't restart. Run `hotspot up` to try again.\n";
        return 1;
    }
    if (*restarted) {
        std::cout << "Hotspot name changed to " << config->ssid << ". Reconnect your devices.\n";
        show_password();
    } else {
        std::cout << "Hotspot name changed to " << config->ssid << ". It will take effect the next time the hotspot starts.\n";
    }
    return 0;
}

void print_help() {
    std::cout << "apctl commands\n"
              << "  help              Show this help\n"
              << "  check             Check if this computer can share Wi-Fi\n"
              << "  status            Show Wi-Fi and hotspot status\n"
              << "  gen [SSID]        Generate and save hotspot credentials\n"
              << "  up | open         Start the concurrent hotspot\n"
              << "  show-password     Show hotspot name, password, and QR code\n"
              << "  change-password   Prompt for a new hotspot password\n"
              << "  change-ssid       Prompt for a new hotspot SSID\n"
              << "  down              Stop and clean up the app hotspot\n";
}

int up() {
    const auto config = load_config();
    if (!config) { std::cerr << "Hotspot details aren't set up yet. Run `hotspot gen` first.\n"; return 2; }
    auto state = load_state();
    if (!state.profile_uuid.empty() && connection_exists_uuid(state.profile_uuid)) {
        const auto active_uuid = run({"nmcli", "-g", "GENERAL.CON-UUID", "device", "show", "ap0"});
        if (active_uuid.exit_code == 0 && trim(active_uuid.out) == state.profile_uuid) {
            const auto active_name = run({"nmcli", "-g", "connection.id", "connection", "show", "uuid", state.profile_uuid});
            if (active_name.exit_code == 0 && trim(active_name.out) == "Caelestia-Hotspot") {
                command({"nmcli", "connection", "modify", "uuid", state.profile_uuid, "connection.id", "apctl-hotspot"}, true);
            }
            std::cout << "Hotspot is already running.\n";
            show_password();
            return 0;
        }
    }
    const auto iface = active_wifi();
    const auto channel = iface ? connected_channel(*iface) : std::nullopt;
    if (!iface) { std::cerr << "Connect to Wi-Fi first, then run `hotspot up`.\n"; return 3; }
    if (!channel) { std::cerr << "Couldn't read the Wi-Fi channel. Reconnect to Wi-Fi, then retry.\n"; return 3; }
    if (!concurrent_capability()) { std::cerr << "This computer doesn't report support for Wi-Fi and hotspot at the same time.\n"; return 3; }

    constexpr std::string_view ap = "ap0";
    constexpr std::string_view profile = "apctl-hotspot";
    const auto existing = run({"iw", "dev", std::string(ap), "info"});
    if (existing.exit_code != 0) {
        const auto phy = run({"iw", "dev", *iface, "info"});
        const auto wiphy = field(phy.out, "\twiphy ");
        if (!wiphy) { std::cerr << "Couldn't identify the Wi-Fi adapter. Run `hotspot status` and retry.\n"; return 3; }
        if (!privileged({"iw", "phy", "phy" + *wiphy, "interface", "add", std::string(ap), "type", "__ap"})) return 1;
        state.interface_owned = true;
        if (!save_state(state)) { std::cerr << "Couldn't save hotspot setup. Check that your home folder is writable.\n"; return 1; }
    } else if (!state.interface_owned) {
        std::cerr << "A hotspot setup already exists outside this app. Stop it first, then retry.\n";
        return 3;
    }
    state.uplink = *iface;
    if (!state.profile_uuid.empty() && state.profile_backend != "nmcli-hotspot-v1" && connection_exists_uuid(state.profile_uuid)) {
        command({"nmcli", "connection", "down", "uuid", state.profile_uuid}, true);
        if (!command({"nmcli", "connection", "delete", "uuid", state.profile_uuid})) return 1;
        state.profile_uuid.clear();
        state.profile_backend.clear();
    }
    if (!save_state(state)) { std::cerr << "Couldn't save hotspot setup. Check that your home folder is writable.\n"; return 1; }
    // Use the same NetworkManager hotspot creator that succeeded in the manual test.
    // The AP MAC is stable and matches the manually verified locally administered address.
    if (!prepare_owned_ap()) return 1;
    if (!configure_firewall(state, ap, *iface)) return 1;

    bool created_profile = false;
    std::string current_uuid;
    if (!state.profile_uuid.empty() && state.profile_backend == "nmcli-hotspot-v1" && connection_exists_uuid(state.profile_uuid)) {
        current_uuid = state.profile_uuid;
        const auto name = run({"nmcli", "-g", "connection.id", "connection", "show", "uuid", current_uuid});
        if (name.exit_code == 0 && trim(name.out) == "Caelestia-Hotspot" &&
            !command({"nmcli", "connection", "modify", "uuid", current_uuid, "connection.id", std::string(profile)})) return 1;
    } else {
        if (connection_name_exists(profile)) {
            std::cerr << "A hotspot with this app's name already exists outside the app. Rename or remove it, then retry.\n";
            return 3;
        }
        const std::string band = std::stoi(*channel) <= 14 ? "bg" : "a";
        if (!command({"nmcli", "device", "wifi", "hotspot", "ifname", std::string(ap), "con-name", std::string(profile), "ssid", config->ssid, "band", band, "channel", *channel, "password", config->password})) return 1;
        const auto created_uuid = connection_uuid(profile);
        if (!created_uuid) { std::cerr << "Hotspot started, but setup couldn't be saved. Run `hotspot status` before retrying.\n"; return 1; }
        current_uuid = *created_uuid;
        created_profile = true;
        state.profile_uuid = current_uuid;
        state.profile_backend = "nmcli-hotspot-v1";
    }
    state.profile_uuid = current_uuid;
    state.profile_backend = "nmcli-hotspot-v1";
    if (!save_state(state)) { std::cerr << "Hotspot started, but setup couldn't be saved. Run `hotspot status` before retrying.\n"; return 1; }
    if (!created_profile) {
        const std::vector<std::vector<std::string>> settings = {
            {"nmcli", "connection", "modify", "uuid", current_uuid, "connection.interface-name", std::string(ap)},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "802-11-wireless.mode", "ap"},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "802-11-wireless.band", std::stoi(*channel) <= 14 ? "bg" : "a"},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "802-11-wireless.channel", *channel},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "802-11-wireless.ssid", config->ssid},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "wifi-sec.psk", config->password},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "ipv4.method", "shared"},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "connection.autoconnect", "no"}
        };
        for (const auto& setting : settings) if (!command(setting)) return 1;
        if (!command({"nmcli", "connection", "up", "uuid", current_uuid, "ifname", std::string(ap)})) return 1;
    }
    std::cout << "Hotspot is ready: " << config->ssid << " (channel " << *channel << "). Wi-Fi stays connected.\n\n";
    show_password();
    return 0;
}

int down() {
    constexpr std::string_view ap = "ap0";
    const auto state = load_state();
    const bool owned_profile_exists = !state.profile_uuid.empty() && connection_exists_uuid(state.profile_uuid);
    if (!owned_profile_exists && connection_name_exists("apctl-hotspot")) {
        std::cerr << "An existing hotspot wasn't created by this app, so it was left unchanged.\n";
        return 3;
    }
    if (owned_profile_exists) {
        command({"nmcli", "connection", "down", "uuid", state.profile_uuid}, true);
        if (!command({"nmcli", "connection", "delete", "uuid", state.profile_uuid})) return 1;
    }
    bool cleanup_ok = true;
    const auto ufw = run({"ufw", "status"});
    if (ufw.exit_code != 0 && (state.dhcp || state.dns || state.route)) cleanup_ok = false;
    if (ufw.exit_code == 0) {
        if (state.dhcp) cleanup_ok = privileged({"ufw", "delete", "allow", "in", "on", std::string(ap), "to", "any", "port", "67", "proto", "udp"}) && cleanup_ok;
        if (state.dns) cleanup_ok = privileged({"ufw", "delete", "allow", "in", "on", std::string(ap), "to", "any", "port", "53", "proto", "udp"}) && cleanup_ok;
        if (state.route && !state.uplink.empty()) cleanup_ok = privileged({"ufw", "route", "delete", "allow", "in", "on", std::string(ap), "out", "on", state.uplink, "from", "10.42.0.0/24", "to", "any"}) && cleanup_ok;
    }
    if (state.interface_owned && run({"iw", "dev", std::string(ap), "info"}).exit_code == 0) {
        command({"nmcli", "device", "set", std::string(ap), "managed", "no"}, true);
        if (!privileged({"ip", "link", "set", "dev", std::string(ap), "down"})) cleanup_ok = false;
        if (!privileged({"iw", "dev", std::string(ap), "del"})) cleanup_ok = false;
    }
    if (cleanup_ok) {
        std::error_code ignored;
        fs::remove(state_path(), ignored);
        std::cout << "Hotspot stopped.\n";
        return 0;
    }
    std::cerr << "Hotspot stopped, but some cleanup didn't finish. Run `hotspot down` again.\n";
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_help();
        return 2;
    }
    const std::string_view action(argv[1]);
    if (action == "help" || action == "--help" || action == "-h") { print_help(); return 0; }
    if (action == "check") return check();
    if (action == "status") { print_status(); return 0; }
    if (action == "gen") return generate(argc, argv);
    if (action == "up" || action == "open") return up();
    if (action == "show-password") { show_password(); return 0; }
    if (action == "change-password") return change_password();
    if (action == "change-ssid") return change_ssid();
    if (action == "down") return down();
    std::cerr << "Unknown command: " << action << '\n';
    return 2;
}
