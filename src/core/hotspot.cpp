#include "hotspot.hpp"

#include "process.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace apctl::core {
namespace {

struct Config { std::string ssid; std::string password; };

struct FirewallState {
    bool dhcp = false;
    bool dns = false;
    bool route = false;
    bool interface_owned = false;
    std::string uplink;
    std::string profile_uuid;
    std::string profile_backend;
};

OperationResult result(Severity severity, std::string message, int code = 0) {
    return {severity, code, std::move(message), std::nullopt};
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
fs::path state_path() { return config_path().parent_path() / "state"; }
fs::path legacy_config_path() { return config_root() / "caelestia-hotspot" / "config"; }
fs::path legacy_state_path() { return config_root() / "caelestia-hotspot" / "state"; }

void migrate_legacy_data() {
    std::error_code ec;
    if (!fs::exists(config_root() / "caelestia-hotspot", ec) || ec) return;
    fs::create_directories(config_path().parent_path(), ec);
    if (ec) return;
    const auto migrate = [](const fs::path& from, const fs::path& to) {
        std::error_code error;
        if (fs::exists(to, error) || error || !fs::exists(from, error) || error) return;
        fs::rename(from, to, error);
        if (!error) chmod(to.c_str(), 0600);
    };
    migrate(legacy_config_path(), config_path());
    migrate(legacy_state_path(), state_path());
}

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

FirewallState load_state() {
    migrate_legacy_data();
    std::ifstream file(state_path());
    FirewallState state;
    std::string line;
    while (std::getline(file, line)) {
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        const auto key = line.substr(0, equal);
        const auto value = line.substr(equal + 1);
        if (key == "dhcp") state.dhcp = value == "1";
        else if (key == "dns") state.dns = value == "1";
        else if (key == "route") state.route = value == "1";
        else if (key == "interface_owned") state.interface_owned = value == "1";
        else if (key == "uplink") state.uplink = value;
        else if (key == "profile_uuid") state.profile_uuid = value;
        else if (key == "profile_backend") state.profile_backend = value;
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
    const bool ok = std::fprintf(file,
        "dhcp=%d\ndns=%d\nroute=%d\ninterface_owned=%d\nuplink=%s\nprofile_uuid=%s\nprofile_backend=%s\n",
        state.dhcp, state.dns, state.route, state.interface_owned, state.uplink.c_str(),
        state.profile_uuid.c_str(), state.profile_backend.c_str()) > 0;
    std::fclose(file);
    chmod(state_path().c_str(), 0600);
    return ok;
}

bool command(const std::vector<std::string>& args, std::string& error, bool quiet = false) {
    const auto child = run_process(args);
    if (child.exit_code == 0) return true;
    if (quiet) return false;
    std::string detail = child.err.empty() ? child.out : child.err;
    for (char& ch : detail) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (detail.find("not authorized") != std::string::npos || detail.find("permission denied") != std::string::npos ||
        detail.find("a password is required") != std::string::npos) {
        error = "System permission is required. Approve the authorization prompt and retry.";
    } else if (detail.find("not a valid channel") != std::string::npos ||
               (detail.find("channel") != std::string::npos && detail.find("invalid") != std::string::npos)) {
        error = "The Wi-Fi channel is no longer available. Check Wi-Fi and retry.";
    } else if (detail.find("device is not available") != std::string::npos ||
               detail.find("could not be readied") != std::string::npos ||
               detail.find("device or resource busy") != std::string::npos) {
        error = "Wi-Fi isn't ready to start the hotspot. Refresh status and retry.";
    } else {
        error = "NetworkManager couldn't complete that step. Refresh status and retry.";
    }
    return false;
}

bool privileged(const std::vector<std::string>& args, std::string& error) {
    std::vector<std::string> full{"pkexec"};
    full.insert(full.end(), args.begin(), args.end());
    return command(full, error);
}

bool add_ufw_rule(const std::vector<std::string>& rule, bool& owned, std::string& error) {
    std::string fingerprint;
    for (size_t i = 1; i < rule.size(); ++i) {
        if (!fingerprint.empty()) fingerprint.push_back(' ');
        fingerprint += rule[i];
    }
    const auto existing = run_process({"ufw", "show", "added"});
    if (existing.exit_code != 0) {
        error = "Couldn't check the firewall settings. No changes were made.";
        return false;
    }
    if (existing.out.find(fingerprint) != std::string::npos) return true;
    if (!privileged(rule, error)) return false;
    owned = true;
    return true;
}

bool configure_firewall(FirewallState& state, std::string_view ap, std::string_view uplink, std::string& error) {
    const auto ufw = run_process({"ufw", "status"});
    if (ufw.exit_code != 0 || ufw.out.find("Status: active") == std::string::npos) return true;
    return add_ufw_rule({"ufw", "allow", "in", "on", std::string(ap), "to", "any", "port", "67", "proto", "udp"}, state.dhcp, error) && save_state(state) &&
           add_ufw_rule({"ufw", "allow", "in", "on", std::string(ap), "to", "any", "port", "53", "proto", "udp"}, state.dns, error) && save_state(state) &&
           add_ufw_rule({"ufw", "route", "allow", "in", "on", std::string(ap), "out", "on", std::string(uplink), "from", "10.42.0.0/24", "to", "any"}, state.route, error) && save_state(state);
}

std::optional<std::string> connection_uuid(std::string_view name) {
    const auto child = run_process({"nmcli", "-g", "connection.uuid", "connection", "show", std::string(name)});
    if (child.exit_code != 0) return std::nullopt;
    return trim(child.out);
}

bool connection_exists_uuid(std::string_view uuid) {
    const auto child = run_process({"nmcli", "-g", "connection.uuid", "connection", "show", "uuid", std::string(uuid)});
    return child.exit_code == 0 && trim(child.out) == uuid;
}

bool connection_name_exists(std::string_view name) {
    const auto child = run_process({"nmcli", "-t", "-f", "NAME", "connection", "show"});
    if (child.exit_code != 0) return false;
    std::istringstream lines(child.out);
    std::string line;
    while (std::getline(lines, line)) if (trim(line) == name) return true;
    return false;
}

bool wait_until_available(std::string_view iface) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto child = run_process({"nmcli", "-g", "GENERAL.STATE", "device", "show", std::string(iface)});
        if (child.exit_code == 0) {
            const auto state = trim(child.out);
            if (state.rfind("30 ", 0) == 0 || state == "30" || state.rfind("100 ", 0) == 0 || state == "100") return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

std::string random_mac() { return "02:88:cd:08:b8:c8"; }

bool prepare_owned_ap(std::string& error) {
    constexpr std::string_view ap = "ap0";
    if (!command({"nmcli", "device", "set", std::string(ap), "managed", "no"}, error, true)) {
        error = "Couldn't prepare Wi-Fi for the hotspot. Refresh status and retry.";
        return false;
    }
    if (!privileged({"ip", "link", "set", "dev", std::string(ap), "down"}, error) ||
        !privileged({"iw", "dev", std::string(ap), "set", "type", "__ap"}, error) ||
        !privileged({"ip", "link", "set", "dev", std::string(ap), "address", random_mac()}, error)) return false;
    if (!command({"nmcli", "device", "set", std::string(ap), "managed", "yes"}, error)) return false;
    if (!wait_until_available(ap)) {
        error = "Wi-Fi isn't ready to start the hotspot. Refresh status and retry.";
        return false;
    }
    return true;
}

std::optional<std::string> active_wifi() {
    const auto child = run_process({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device", "status"});
    if (child.exit_code != 0) return std::nullopt;
    std::istringstream stream(child.out);
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
    const auto child = run_process({"iw", "dev", iface, "link"});
    if (child.exit_code != 0 || child.out.find("Not connected") != std::string::npos) return std::nullopt;
    const auto freq = field(child.out, "\tfreq:");
    if (!freq) return std::nullopt;
    try {
        const int mhz = std::stoi(*freq);
        if (mhz >= 2412 && mhz <= 2472) return std::to_string((mhz - 2407) / 5);
        if (mhz == 2484) return std::string("14");
        if (mhz >= 5000 && mhz < 5950) return std::to_string((mhz - 5000) / 5);
    } catch (...) {}
    return std::nullopt;
}

bool concurrent_capability() {
    const auto child = run_process({"iw", "list"});
    return child.exit_code == 0 && child.out.find("#{ managed }") != std::string::npos &&
           child.out.find("#{ AP") != std::string::npos && child.out.find("total <= 2") != std::string::npos;
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

bool valid_ssid(std::string_view ssid) {
    return !ssid.empty() && ssid.size() <= 32 && ssid.find_first_of("\r\n") == std::string_view::npos;
}

bool valid_password(std::string_view password) {
    return password.size() >= 8 && password.size() <= 63 && password.find_first_of("\r\n") == std::string_view::npos;
}

std::optional<bool> restart_profile_if_active(std::string_view uuid, std::string& error) {
    const auto active_uuid = run_process({"nmcli", "-g", "GENERAL.CON-UUID", "device", "show", "ap0"});
    if (active_uuid.exit_code != 0 || trim(active_uuid.out) != uuid) return false;
    if (!command({"nmcli", "connection", "down", "uuid", std::string(uuid)}, error)) return std::nullopt;
    if (!prepare_owned_ap(error)) return std::nullopt;
    if (!command({"nmcli", "connection", "up", "uuid", std::string(uuid), "ifname", "ap0"}, error)) return std::nullopt;
    return true;
}

std::string qr_from_nmcli_output(const std::string& output) {
    std::istringstream lines(output);
    std::string line;
    std::string qr;
    bool reading_qr = false;
    while (std::getline(lines, line)) {
        if (!reading_qr) {
            if (line.find("\x1b[37;40") == std::string::npos) continue;
            reading_qr = true;
        }

        const bool light_border_row = line.find("\x1b[0;37m") != std::string::npos;
        std::string plain;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
                i += 2;
                while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) ++i;
            } else if (line[i] != '\x0e' && line[i] != '\x0f' && line[i] != '\r') {
                plain.push_back(line[i]);
            }
        }
        if (plain.size() >= 2 && plain[0] == ' ' && plain[1] == ' ')
            plain.erase(0, 2);

        std::string row;
        if (light_border_row) {
            size_t cells = 0;
            for (unsigned char byte : plain) {
                if ((byte & 0xC0) != 0x80) ++cells;
            }
            row.assign(cells, ' ');
        } else {
            for (size_t i = 0; i < plain.size();) {
                if (plain.compare(i, 3, "▄") == 0) {
                    row += "▀";
                    i += 3;
                } else if (plain.compare(i, 3, "▀") == 0) {
                    row += "▄";
                    i += 3;
                } else if (plain.compare(i, 3, "█") == 0) {
                    row.push_back(' ');
                    i += 3;
                } else if (plain[i] == ' ') {
                    row += "█";
                    ++i;
                } else {
                    row.push_back(plain[i++]);
                }
            }
        }
        if (!qr.empty()) qr.push_back('\n');
        qr += row;
    }
    return qr;
}

std::optional<Credentials> load_credentials(bool with_qr) {
    const auto config = load_config();
    if (!config) return std::nullopt;
    Credentials credentials{config->ssid, config->password, {}};
    if (with_qr) {
        const auto child = run_process({"nmcli", "--colors", "yes", "device", "wifi", "show-password", "ifname", "ap0"});
        if (child.exit_code == 0) credentials.qr_text = qr_from_nmcli_output(child.out);
    }
    return credentials;
}

}  // namespace

Status HotspotService::status() const {
    Status current;
    current.network_manager_available = run_process({"nmcli", "general", "status"}).exit_code == 0;
    if (!current.network_manager_available) return current;

    const auto wifi = active_wifi();
    if (wifi) {
        current.wifi_device = *wifi;
        const auto connection = run_process({"nmcli", "-g", "GENERAL.CONNECTION", "device", "show", *wifi});
        if (connection.exit_code == 0) current.wifi_connection = trim(connection.out);
    }

    const auto ap = run_process({"iw", "dev", "ap0", "info"});
    const auto state = run_process({"nmcli", "-g", "GENERAL.STATE", "device", "show", "ap0"});
    current.hotspot_active = ap.exit_code == 0 && state.exit_code == 0 && trim(state.out).rfind("100 ", 0) == 0;
    if (!current.hotspot_active) return current;
    if (const auto ssid = field(ap.out, "\tssid ")) current.hotspot_ssid = *ssid;
    if (const auto channel = field(ap.out, "\tchannel ")) current.hotspot_channel = trim(channel->substr(0, channel->find(' ')));
    const auto stations = run_process({"iw", "dev", "ap0", "station", "dump"});
    if (stations.exit_code == 0) {
        std::istringstream lines(stations.out);
        std::string line;
        while (std::getline(lines, line)) if (line.rfind("Station ", 0) == 0) ++current.connected_devices;
    }
    return current;
}

OperationResult HotspotService::check() const {
    if (run_process({"nmcli", "general", "status"}).exit_code != 0)
        return result(Severity::error, "Network settings aren't available right now.", 3);
    const auto iface = active_wifi();
    if (!iface) return result(Severity::error, "Connect to Wi-Fi first, then check again.", 3);
    const bool supported = concurrent_capability();
    const auto channel = connected_channel(*iface);
    if (!channel) return result(Severity::error, "Couldn't read the Wi-Fi channel. Reconnect and retry.", 3);
    if (!supported) return result(Severity::warning,
        "This adapter doesn't report support for Wi-Fi and hotspot at the same time. Runtime support can still vary.", 3);
    return result(Severity::success, "Wi-Fi and hotspot concurrency is advertised on channel " + *channel + ".", 0);
}

OperationResult HotspotService::generate(std::string ssid) const {
    if (!valid_ssid(ssid)) return result(Severity::error, "The SSID must be 1–32 characters on one line.", 2);
    Config config{std::move(ssid), random_password()};
    if (config.password.size() != 16 || !save_config(config))
        return result(Severity::error, "Couldn't save hotspot details. Check that your home folder is writable.", 1);
    auto generated = result(Severity::success, "Hotspot details saved.");
    generated.credentials = Credentials{config.ssid, config.password, {}};
    return generated;
}

OperationResult HotspotService::show_credentials() const {
    auto credentials = load_credentials(true);
    if (!credentials) return result(Severity::error, "No saved hotspot details. Generate them before starting the hotspot.", 2);
    auto shown = result(credentials->qr_text.empty() ? Severity::warning : Severity::info,
        credentials->qr_text.empty() ? "QR code unavailable; use the saved name and password below." : "Hotspot credentials");
    shown.credentials = std::move(credentials);
    return shown;
}

OperationResult HotspotService::change_password(std::string password) const {
    if (!valid_password(password)) return result(Severity::error, "Password must be 8–63 characters with no line breaks.", 2);
    auto config = load_config();
    if (!config) return result(Severity::error, "No saved hotspot details. Generate them first.", 2);
    const auto previous = config->password;
    config->password = std::move(password);
    if (!save_config(*config)) return result(Severity::error, "Couldn't save the new password.", 1);

    const auto state = load_state();
    if (state.profile_uuid.empty() || !connection_exists_uuid(state.profile_uuid))
        return result(Severity::success, "Password saved. It will take effect the next time the hotspot starts.");
    std::string error;
    if (!command({"nmcli", "connection", "modify", "uuid", state.profile_uuid, "wifi-sec.psk", config->password}, error)) {
        config->password = previous;
        (void)save_config(*config);
        return result(Severity::error, "Couldn't change the hotspot password. The previous password is still in use.", 1);
    }
    const auto restarted = restart_profile_if_active(state.profile_uuid, error);
    if (!restarted) return result(Severity::error, "Password saved, but the hotspot couldn't restart. " + error, 1);
    if (*restarted) {
        auto changed = result(Severity::success, "Password changed. Reconnect your devices using the new password.");
        changed.credentials = load_credentials(true);
        return changed;
    }
    return result(Severity::success, "Password changed. It will take effect the next time the hotspot starts.");
}

OperationResult HotspotService::change_ssid(std::string ssid) const {
    if (!valid_ssid(ssid)) return result(Severity::error, "The SSID must be 1–32 characters on one line.", 2);
    auto config = load_config();
    if (!config) return result(Severity::error, "No saved hotspot details. Generate them first.", 2);
    const auto previous = config->ssid;
    config->ssid = std::move(ssid);
    if (!save_config(*config)) return result(Severity::error, "Couldn't save the new SSID.", 1);

    const auto state = load_state();
    if (state.profile_uuid.empty() || !connection_exists_uuid(state.profile_uuid))
        return result(Severity::success, "SSID saved. It will take effect the next time the hotspot starts.");
    std::string error;
    if (!command({"nmcli", "connection", "modify", "uuid", state.profile_uuid, "802-11-wireless.ssid", config->ssid}, error)) {
        config->ssid = previous;
        (void)save_config(*config);
        return result(Severity::error, "Couldn't change the SSID. The previous SSID is still in use.", 1);
    }
    const auto restarted = restart_profile_if_active(state.profile_uuid, error);
    if (!restarted) return result(Severity::error, "New SSID saved, but the hotspot couldn't restart. " + error, 1);
    if (*restarted) {
        auto changed = result(Severity::success, "SSID changed. Reconnect your devices.");
        changed.credentials = load_credentials(true);
        return changed;
    }
    return result(Severity::success, "SSID changed. It will take effect the next time the hotspot starts.");
}

OperationResult HotspotService::start() const {
    const auto config = load_config();
    if (!config) return result(Severity::error, "Hotspot details aren't set up yet. Generate them first.", 2);
    auto state = load_state();
    if (!state.profile_uuid.empty() && connection_exists_uuid(state.profile_uuid)) {
        const auto active_uuid = run_process({"nmcli", "-g", "GENERAL.CON-UUID", "device", "show", "ap0"});
        if (active_uuid.exit_code == 0 && trim(active_uuid.out) == state.profile_uuid) {
            const auto active_name = run_process({"nmcli", "-g", "connection.id", "connection", "show", "uuid", state.profile_uuid});
            if (active_name.exit_code == 0 && trim(active_name.out) == "Caelestia-Hotspot") {
                std::string ignored;
                (void)command({"nmcli", "connection", "modify", "uuid", state.profile_uuid, "connection.id", "apctl-hotspot"}, ignored, true);
            }
            auto already = result(Severity::info, "Hotspot is already running.");
            already.credentials = load_credentials(true);
            return already;
        }
    }

    const auto iface = active_wifi();
    if (!iface) return result(Severity::error, "Connect to Wi-Fi first, then start the hotspot.", 3);
    const auto channel = connected_channel(*iface);
    if (!channel) return result(Severity::error, "Couldn't read the Wi-Fi channel. Reconnect and retry.", 3);
    if (!concurrent_capability()) return result(Severity::error,
        "This computer doesn't report support for Wi-Fi and hotspot at the same time.", 3);

    constexpr std::string_view ap = "ap0";
    constexpr std::string_view profile = "apctl-hotspot";
    const auto existing = run_process({"iw", "dev", std::string(ap), "info"});
    if (existing.exit_code != 0) {
        const auto phy = run_process({"iw", "dev", *iface, "info"});
        const auto wiphy = field(phy.out, "\twiphy ");
        if (!wiphy) return result(Severity::error, "Couldn't identify the Wi-Fi adapter. Refresh status and retry.", 3);
        std::string error;
        if (!privileged({"iw", "phy", "phy" + *wiphy, "interface", "add", std::string(ap), "type", "__ap"}, error))
            return result(Severity::error, error, 1);
        state.interface_owned = true;
        if (!save_state(state)) return result(Severity::error, "Couldn't save hotspot setup.", 1);
    } else if (!state.interface_owned) {
        return result(Severity::error, "A hotspot setup already exists outside this app. Stop it first, then retry.", 3);
    }

    state.uplink = *iface;
    if (!state.profile_uuid.empty() && state.profile_backend != "nmcli-hotspot-v1" && connection_exists_uuid(state.profile_uuid)) {
        std::string error;
        (void)command({"nmcli", "connection", "down", "uuid", state.profile_uuid}, error, true);
        if (!command({"nmcli", "connection", "delete", "uuid", state.profile_uuid}, error)) return result(Severity::error, error, 1);
        state.profile_uuid.clear();
        state.profile_backend.clear();
    }
    if (!save_state(state)) return result(Severity::error, "Couldn't save hotspot setup.", 1);

    std::string error;
    if (!prepare_owned_ap(error)) return result(Severity::error, error, 1);
    if (!configure_firewall(state, ap, *iface, error))
        return result(Severity::error, error.empty() ? "Couldn't finish network sharing setup." : error, 1);

    bool created_profile = false;
    std::string current_uuid;
    if (!state.profile_uuid.empty() && state.profile_backend == "nmcli-hotspot-v1" && connection_exists_uuid(state.profile_uuid)) {
        current_uuid = state.profile_uuid;
        const auto name = run_process({"nmcli", "-g", "connection.id", "connection", "show", "uuid", current_uuid});
        if (name.exit_code == 0 && trim(name.out) == "Caelestia-Hotspot" &&
            !command({"nmcli", "connection", "modify", "uuid", current_uuid, "connection.id", std::string(profile)}, error))
            return result(Severity::error, error, 1);
    } else {
        if (connection_name_exists(profile))
            return result(Severity::error, "A hotspot with this app's name already exists outside the app. Rename or remove it, then retry.", 3);
        const std::string band = std::stoi(*channel) <= 14 ? "bg" : "a";
        if (!command({"nmcli", "device", "wifi", "hotspot", "ifname", std::string(ap), "con-name", std::string(profile),
                      "ssid", config->ssid, "band", band, "channel", *channel, "password", config->password}, error))
            return result(Severity::error, error, 1);
        const auto uuid = connection_uuid(profile);
        if (!uuid) return result(Severity::error, "Hotspot started, but setup couldn't be saved. Refresh status before retrying.", 1);
        current_uuid = *uuid;
        created_profile = true;
        state.profile_uuid = current_uuid;
        state.profile_backend = "nmcli-hotspot-v1";
    }
    state.profile_uuid = current_uuid;
    state.profile_backend = "nmcli-hotspot-v1";
    if (!save_state(state)) return result(Severity::error, "Hotspot started, but setup couldn't be saved. Refresh status before retrying.", 1);
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
        for (const auto& setting : settings) if (!command(setting, error)) return result(Severity::error, error, 1);
        if (!command({"nmcli", "connection", "up", "uuid", current_uuid, "ifname", std::string(ap)}, error))
            return result(Severity::error, error, 1);
    }
    auto started = result(Severity::success, "Hotspot is ready. Wi-Fi stays connected.");
    started.credentials = load_credentials(true);
    return started;
}

OperationResult HotspotService::stop() const {
    constexpr std::string_view ap = "ap0";
    const auto state = load_state();
    const bool owned_profile_exists = !state.profile_uuid.empty() && connection_exists_uuid(state.profile_uuid);
    if (!owned_profile_exists && connection_name_exists("apctl-hotspot"))
        return result(Severity::error, "An existing hotspot wasn't created by this app, so it was left unchanged.", 3);
    std::string error;
    if (owned_profile_exists) {
        (void)command({"nmcli", "connection", "down", "uuid", state.profile_uuid}, error, true);
        if (!command({"nmcli", "connection", "delete", "uuid", state.profile_uuid}, error)) return result(Severity::error, error, 1);
    }
    bool cleanup_ok = true;
    const auto ufw = run_process({"ufw", "status"});
    if (ufw.exit_code != 0 && (state.dhcp || state.dns || state.route)) cleanup_ok = false;
    if (ufw.exit_code == 0) {
        if (state.dhcp) cleanup_ok = privileged({"ufw", "delete", "allow", "in", "on", std::string(ap), "to", "any", "port", "67", "proto", "udp"}, error) && cleanup_ok;
        if (state.dns) cleanup_ok = privileged({"ufw", "delete", "allow", "in", "on", std::string(ap), "to", "any", "port", "53", "proto", "udp"}, error) && cleanup_ok;
        if (state.route && !state.uplink.empty()) cleanup_ok = privileged({"ufw", "route", "delete", "allow", "in", "on", std::string(ap), "out", "on", state.uplink, "from", "10.42.0.0/24", "to", "any"}, error) && cleanup_ok;
    }
    if (state.interface_owned && run_process({"iw", "dev", std::string(ap), "info"}).exit_code == 0) {
        (void)command({"nmcli", "device", "set", std::string(ap), "managed", "no"}, error, true);
        if (!privileged({"ip", "link", "set", "dev", std::string(ap), "down"}, error)) cleanup_ok = false;
        if (!privileged({"iw", "dev", std::string(ap), "del"}, error)) cleanup_ok = false;
    }
    if (!cleanup_ok) return result(Severity::error, "Hotspot stopped, but some cleanup didn't finish. Run `apctl down` again. " + error, 1);
    std::error_code ignored;
    fs::remove(state_path(), ignored);
    return result(Severity::success, "Hotspot stopped.");
}

}  // namespace apctl::core
