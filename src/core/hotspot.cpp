#include "hotspot.hpp"

#include "process.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
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

struct Config {
    std::string ssid;
    std::string password;
    BandPreference band = BandPreference::automatic;
};

const char* band_config_value(BandPreference band) {
    switch (band) {
        case BandPreference::ghz_2_4: return "2.4";
        case BandPreference::ghz_5: return "5";
        case BandPreference::automatic: return "auto";
    }
    return "auto";
}

const char* network_manager_band(BandPreference band) {
    return band == BandPreference::ghz_2_4 ? "bg" : "a";
}

std::string channel_band_name(std::string_view channel) {
    try {
        return std::stoi(std::string(channel)) <= 14 ? "2.4 GHz" : "5 GHz";
    } catch (...) {
        return "unknown band";
    }
}

const char* band_preference_name(BandPreference band) {
    switch (band) {
        case BandPreference::ghz_2_4: return "2.4 GHz";
        case BandPreference::ghz_5: return "5 GHz";
        case BandPreference::automatic: return "Auto";
    }
    return "Auto";
}

BandPreference channel_band(std::string_view channel) {
    try {
        return std::stoi(std::string(channel)) <= 14 ? BandPreference::ghz_2_4 : BandPreference::ghz_5;
    } catch (...) {
        return BandPreference::automatic;
    }
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
        else if (key == "band") {
            if (value == "2.4") config.band = BandPreference::ghz_2_4;
            else if (value == "5") config.band = BandPreference::ghz_5;
            else config.band = BandPreference::automatic;
        }
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
    const bool ok = std::fprintf(file, "ssid=%s\npassword=%s\nband=%s\n", config.ssid.c_str(),
                                 config.password.c_str(), band_config_value(config.band)) > 0;
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

bool executable_available(std::string_view name) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::istringstream entries(path);
    std::string directory;
    while (std::getline(entries, directory, ':')) {
        if (directory.empty()) directory = ".";
        const auto candidate = fs::path(directory) / name;
        if (access(candidate.c_str(), X_OK) == 0) return true;
    }
    return false;
}

std::optional<std::pair<uint32_t, unsigned>> ipv4_cidr(std::string_view value) {
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const std::string address(value.substr(0, slash));
    unsigned prefix = 0;
    try {
        const std::string prefix_text(value.substr(slash + 1));
        size_t parsed = 0;
        const unsigned long converted = std::stoul(prefix_text, &parsed);
        if (parsed != prefix_text.size() || converted > 32) return std::nullopt;
        prefix = static_cast<unsigned>(converted);
    } catch (...) {
        return std::nullopt;
    }
    in_addr parsed_address{};
    if (inet_pton(AF_INET, address.c_str(), &parsed_address) != 1) return std::nullopt;
    return std::pair<uint32_t, unsigned>{ntohl(parsed_address.s_addr), prefix};
}

bool cidr_contains(std::string_view parent, std::string_view child) {
    const auto outer = ipv4_cidr(parent);
    const auto inner = ipv4_cidr(child);
    if (!outer || !inner || outer->second > inner->second) return false;
    const uint32_t mask = outer->second == 0 ? 0 : (0xffffffffu << (32 - outer->second));
    return (outer->first & mask) == (inner->first & mask);
}

bool hotspot_subnet_allowlisted(std::string_view settings) {
    static const std::regex cidr_pattern(R"((?:\d{1,3}\.){3}\d{1,3}/\d{1,2})");
    static constexpr std::string_view hotspot_subnet = "10.42.0.0/24";
    const std::string text(settings);
    for (std::sregex_iterator it(text.begin(), text.end(), cidr_pattern), end; it != end; ++it) {
        if (cidr_contains((*it).str(), hotspot_subnet)) return true;
    }
    return false;
}

struct NordVpnInfo {
    bool installed = false;
    bool settings_available = false;
    bool firewall_enabled = false;
    bool hotspot_subnet_allowlisted = false;
};

NordVpnInfo nordvpn_info() {
    NordVpnInfo info;
    info.installed = executable_available("nordvpn");
    if (!info.installed) return info;
    const auto settings = run_process({"nordvpn", "settings"});
    if (settings.exit_code != 0) return info;
    info.settings_available = true;
    std::string output = settings.out;
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    info.firewall_enabled = output.find("firewall: enabled") != std::string::npos;
    info.hotspot_subnet_allowlisted = hotspot_subnet_allowlisted(settings.out);
    return info;
}

bool polkit_agent_process_detected() {
    if (!executable_available("ps")) return false;
    const auto processes = run_process({"ps", "-e", "-o", "comm="});
    if (processes.exit_code != 0) return false;
    static const std::vector<std::string> agent_prefixes = {
        "polkit-gnome", "polkit-kde", "lxqt-policykit", "hyprpolkitagent",
        "mate-polkit", "xfce-polkit", "noctalia"
    };
    std::istringstream lines(processes.out);
    std::string process;
    while (std::getline(lines, process)) {
        process = trim(std::move(process));
        if (std::any_of(agent_prefixes.begin(), agent_prefixes.end(), [&](const std::string& prefix) {
                return process.rfind(prefix, 0) == 0;
            })) return true;
    }
    return false;
}

std::string saved_config_status() {
    const auto inspect = [](const fs::path& path) {
        std::ifstream file(path);
        if (!file.is_open()) return std::pair<bool, bool>{false, false};
        bool has_ssid = false;
        bool has_password = false;
        std::string line;
        while (std::getline(file, line)) {
            const auto equal = line.find('=');
            if (equal == std::string::npos) continue;
            const auto key = line.substr(0, equal);
            const auto value = line.substr(equal + 1);
            if (key == "ssid") has_ssid = !value.empty();
            else if (key == "password") has_password = value.size() >= 8;
        }
        return std::pair<bool, bool>{true, has_ssid && has_password && !file.bad()};
    };
    const auto current = inspect(config_path());
    if (current.first) return current.second ? "saved credentials are present (values hidden)"
                                              : "current config exists but is incomplete or unreadable";
    const auto legacy = inspect(legacy_config_path());
    if (legacy.first) return legacy.second ? "legacy Caelestia config found; migration is deferred"
                                           : "legacy config exists but is incomplete or unreadable";
    return "no saved credentials; run `apctl gen` first";
}

bool command(const std::vector<std::string>& args, std::string& error, bool quiet = false,
             ProcessResult* process_result = nullptr) {
    const auto child = run_process(args);
    if (process_result) *process_result = child;
    if (child.exit_code == 0) return true;
    if (quiet) return false;
    std::string detail = child.err.empty() ? child.out : child.err;
    for (char& ch : detail) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (detail.find("request dismissed") != std::string::npos && args.size() > 1 && args.front() == "pkexec") {
        std::string operation = args[1];
        if (args.size() > 2) operation += " " + args[2];
        error = "Authorization for `" + operation + "` was dismissed. Approve the authorization prompt and retry.";
    } else if (detail.find("not authorized") != std::string::npos || detail.find("permission denied") != std::string::npos ||
               detail.find("a password is required") != std::string::npos ||
               detail.find("request dismissed") != std::string::npos ||
               detail.find("authentication agent") != std::string::npos) {
        error = "System permission is required. Approve the authorization prompt and retry.";
    } else if (detail.find("not a valid channel") != std::string::npos ||
               (detail.find("channel") != std::string::npos && detail.find("invalid") != std::string::npos)) {
        error = "The Wi-Fi channel is no longer available. Check Wi-Fi and retry.";
    } else if (detail.find("device is not available") != std::string::npos ||
               detail.find("could not be readied") != std::string::npos ||
               detail.find("device or resource busy") != std::string::npos) {
        error = "Wi-Fi isn't ready to start the hotspot. Refresh status and retry.";
    } else if (!args.empty() && args.front() == "pkexec") {
        error = "A privileged system operation couldn't complete. Check the authorization prompt and system settings, then retry.";
    } else {
        error = "NetworkManager couldn't complete that step. Refresh status and retry.";
    }
    return false;
}

ProcessResult privileged_batch(const std::vector<std::string>& args, std::string& error) {
    std::error_code ec;
    const auto executable = fs::canonical("/proc/self/exe", ec);
    if (ec) {
        error = "Couldn't locate apctl to run its authorized setup helper.";
        return {-1, {}, error};
    }
    std::vector<std::string> full{"pkexec", executable.string(), "--privileged-helper"};
    full.insert(full.end(), args.begin(), args.end());
    const auto child = run_process(full);
    if (child.exit_code == 0) return child;

    std::string detail = child.err;
    std::transform(detail.begin(), detail.end(), detail.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    const std::string authorization_action = !args.empty() && args.front() == "stop"
        ? "hotspot cleanup" : ( !args.empty() && args.front() == "interface-prepare"
            ? "hotspot restart" : "hotspot setup");
    if (detail.find("request dismissed") != std::string::npos)
        error = "Authorization for " + authorization_action + " was dismissed. Approve the prompt and retry.";
    else if (child.out.find("APCTL_SKIP") != std::string::npos)
        error = "A matching firewall rule appeared during setup. It was left unowned; inspect UFW and retry.";
    else if (child.out.find("APCTL_FAIL interface-create") != std::string::npos)
        error = "Couldn't create the hotspot interface. Check Wi-Fi and retry.";
    else if (child.out.find("APCTL_FAIL dhcp") != std::string::npos ||
             child.out.find("APCTL_FAIL dns") != std::string::npos ||
             child.out.find("APCTL_FAIL route") != std::string::npos)
        error = "Couldn't add a required UFW rule. Check UFW and retry.";
    else if (child.out.find("APCTL_FAIL networkmanager") != std::string::npos)
        error = "Couldn't prepare the hotspot interface for NetworkManager.";
    else if (!detail.empty() && (detail.find("not authorized") != std::string::npos ||
                                 detail.find("authentication agent") != std::string::npos ||
                                 detail.find("password is required") != std::string::npos))
        error = "System authorization is required. Approve the prompt and retry.";
    else
        error = "A privileged hotspot setup step couldn't complete. Check system settings and retry.";
    return child;
}

bool helper_completed(const ProcessResult& child, std::string_view name) {
    return child.out.find("APCTL_OK " + std::string(name) + "\n") != std::string::npos;
}

enum class UfwRulePresence { missing, present, unreadable };

UfwRulePresence ufw_rule_presence(const std::vector<std::string>& rule) {
    const bool route = rule.size() > 1 && rule[1] == "route";
    const size_t offset = !route && rule.size() > 1 && rule[1] == "delete" ? 1 : 0;
    const size_t route_delete_offset = route && rule.size() > 2 && rule[2] == "delete" ? 1 : 0;
    const std::string ap = route ? rule[5 + route_delete_offset] : rule[4 + offset];
    const std::string tuple = route
        ? "### tuple ### route:allow any any 0.0.0.0/0 any 10.42.0.0/24 in_" + ap + "!out_" + rule[8 + route_delete_offset]
        : "### tuple ### allow udp " + *std::next(std::find(rule.begin(), rule.end(), "port")) +
              " 0.0.0.0/0 any 0.0.0.0/0 in_" + ap;

    std::ifstream rules("/etc/ufw/user.rules");
    if (!rules.is_open()) return UfwRulePresence::unreadable;
    std::string line;
    while (std::getline(rules, line)) {
        if (line == tuple) return UfwRulePresence::present;
    }
    return rules.bad() ? UfwRulePresence::unreadable : UfwRulePresence::missing;
}

enum class UfwState { enabled, disabled, unavailable };

UfwState ufw_state(std::string& error) {
    std::ifstream config("/etc/ufw/ufw.conf");
    if (!config.is_open()) {
        error = "Couldn't read UFW's configuration; its active state couldn't be confirmed.";
        return UfwState::unavailable;
    }
    std::string line;
    while (std::getline(config, line)) {
        line = trim(std::move(line));
        if (line.rfind("ENABLED=", 0) == 0)
            return line.substr(8) == "yes" ? UfwState::enabled : UfwState::disabled;
    }
    error = "Couldn't determine whether UFW is enabled.";
    return UfwState::unavailable;
}

struct FirewallAddPlan {
    bool dhcp = false;
    bool dns = false;
    bool route = false;
};

std::optional<FirewallAddPlan> firewall_add_plan(std::string_view ap, std::string_view uplink,
                                                  std::string& error) {
    error.clear();
    const auto version = run_process({"ufw", "--version"});
    if (version.exit_code != 0) return FirewallAddPlan{};  // Other firewalls are not managed here.
    if (ufw_state(error) == UfwState::disabled) return FirewallAddPlan{};
    if (!error.empty()) return std::nullopt;

    FirewallAddPlan plan;
    const auto inspect = [&](std::vector<std::string> rule, bool& add) {
        const auto presence = ufw_rule_presence(rule);
        if (presence == UfwRulePresence::unreadable) {
            error = "Couldn't inspect UFW's IPv4 rules. No firewall changes were made.";
            return false;
        }
        add = presence == UfwRulePresence::missing;
        return true;
    };
    if (!inspect({"ufw", "allow", "in", "on", std::string(ap), "to", "any", "port", "67", "proto", "udp"}, plan.dhcp) ||
        !inspect({"ufw", "allow", "in", "on", std::string(ap), "to", "any", "port", "53", "proto", "udp"}, plan.dns) ||
        !inspect({"ufw", "route", "allow", "in", "on", std::string(ap), "out", "on", std::string(uplink), "from", "10.42.0.0/24", "to", "any"}, plan.route))
        return std::nullopt;
    return plan;
}

std::string nordvpn_firewall_message(const NordVpnInfo& info) {
    if (!info.settings_available)
        return "Hotspot is running, but NordVPN's firewall state couldn't be checked. Verify it with `nordvpn settings`.";
    if (info.hotspot_subnet_allowlisted)
        return "Hotspot is running. NordVPN's firewall is active, and the hotspot subnet is allowlisted.";
    return "Hotspot is running, but NordVPN's firewall is active and may block clients. "
           "To allow hotspot traffic outside the VPN, run `nordvpn allowlist add subnet 10.42.0.0/24`. "
           "This changes NordVPN's persistent policy; apctl will not change it for you.";
}

bool nordvpn_needs_warning(const NordVpnInfo& info) {
    return info.installed && (!info.settings_available ||
                              (info.firewall_enabled && !info.hotspot_subnet_allowlisted));
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

bool prepare_owned_ap(std::string& error) {
    constexpr std::string_view ap = "ap0";
    const auto child = privileged_batch({"interface-prepare"}, error);
    if (child.exit_code != 0) return false;
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

std::optional<std::string> phy_name(const std::string& iface) {
    const auto child = run_process({"iw", "dev", iface, "info"});
    if (child.exit_code != 0) return std::nullopt;
    const auto index = field(child.out, "\twiphy ");
    if (!index) return std::nullopt;
    return "phy" + *index;
}

std::optional<std::string> phy_info(const std::string& iface) {
    const auto phy = phy_name(iface);
    if (!phy) return std::nullopt;
    const auto child = run_process({"iw", "phy", *phy, "info"});
    if (child.exit_code != 0) return std::nullopt;
    return child.out;
}

bool combination_supports_ap_sta(const std::string& info, int minimum_channels) {
    const std::regex channels_pattern(R"(#channels\s*<=\s*(\d+))");
    const std::regex total_pattern(R"(total\s*<=\s*(\d+))");
    const auto supports = [&](const std::string& block) {
        if (block.find("#{ managed") == std::string::npos || block.find("#{ AP") == std::string::npos)
            return false;
        std::smatch match;
        int channels = 1;
        int total = 0;
        if (std::regex_search(block, match, channels_pattern)) channels = std::stoi(match[1].str());
        if (std::regex_search(block, match, total_pattern)) total = std::stoi(match[1].str());
        return channels >= minimum_channels && total >= 2;
    };

    std::istringstream lines(info);
    std::string line;
    std::string block;
    bool reading = false;
    while (std::getline(lines, line)) {
        if (!reading) {
            if (line.find("valid interface combinations:") != std::string::npos) reading = true;
            continue;
        }
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.rfind("Band ", 0) == 0) {
            if (supports(block)) return true;
            if (trimmed.rfind("Band ", 0) == 0) return false;
            block.clear();
            continue;
        }
        if (trimmed.front() == '*') {
            if (supports(block)) return true;
            block = trimmed;
        } else if (!block.empty()) {
            block += ' ' + trimmed;
        }
    }
    return supports(block);
}

bool concurrent_capability(const std::string& iface) {
    const auto info = phy_info(iface);
    return info && combination_supports_ap_sta(*info, 1);
}

std::vector<std::string> usable_channels(const std::string& info, BandPreference band) {
    std::vector<std::pair<int, int>> channels;
    std::istringstream lines(info);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find(" MHz [") == std::string::npos || line.find("(disabled)") != std::string::npos ||
            line.find("(no IR)") != std::string::npos || line.find("(radar detection)") != std::string::npos)
            continue;
        std::istringstream values(line);
        std::string marker;
        int mhz = 0;
        if (!(values >> marker >> mhz) || marker != "*") continue;
        const auto open = line.find('[');
        const auto close = line.find(']', open == std::string::npos ? 0 : open + 1);
        if (open == std::string::npos || close == std::string::npos) continue;
        int channel = 0;
        try { channel = std::stoi(line.substr(open + 1, close - open - 1)); }
        catch (...) { continue; }
        const bool in_24 = mhz >= 2400 && mhz < 2500;
        const bool in_5 = mhz >= 5000 && mhz < 5925;
        if ((band == BandPreference::ghz_2_4 && in_24) || (band == BandPreference::ghz_5 && in_5))
            channels.emplace_back(channel, mhz);
    }
    std::sort(channels.begin(), channels.end());
    const std::vector<int> preferred = band == BandPreference::ghz_2_4
        ? std::vector<int>{1, 6, 11} : std::vector<int>{36, 40, 44, 48};
    std::vector<std::string> result;
    for (int wanted : preferred) {
        for (const auto& [channel, mhz] : channels) {
            (void)mhz;
            if (channel == wanted) result.push_back(std::to_string(channel));
        }
    }
    for (const auto& [channel, mhz] : channels) {
        (void)mhz;
        const auto value = std::to_string(channel);
        if (std::find(result.begin(), result.end(), value) == result.end()) result.push_back(value);
    }
    return result;
}

std::optional<std::string> select_hotspot_channel(const std::string& iface, const std::string& uplink_channel,
                                                  BandPreference preference, std::string& error) {
    const BandPreference target = preference == BandPreference::automatic ? channel_band(uplink_channel) : preference;
    if (target == channel_band(uplink_channel)) return uplink_channel;

    const auto info = phy_info(iface);
    if (!info || !combination_supports_ap_sta(*info, 2)) {
        error = "This adapter doesn't report support for keeping Wi-Fi connected while the hotspot uses a different channel. The requested band can't be used safely.";
        return std::nullopt;
    }
    const auto channels = usable_channels(*info, target);
    if (channels.empty()) {
        error = "This adapter reports no currently usable hotspot channels in the requested band.";
        return std::nullopt;
    }
    return channels.front();
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
    if (const auto channel = field(ap.out, "\tchannel ")) {
        current.hotspot_channel = trim(channel->substr(0, channel->find(' ')));
        current.hotspot_band = channel_band_name(current.hotspot_channel);
    }
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
    const bool supported = concurrent_capability(*iface);
    const auto channel = connected_channel(*iface);
    if (!channel) return result(Severity::error, "Couldn't read the Wi-Fi channel. Reconnect and retry.", 3);
    if (!supported) return result(Severity::warning,
        "This adapter doesn't report support for Wi-Fi and hotspot at the same time. Runtime support can still vary.", 3);
    const auto config = load_config();
    if (config && config->band != BandPreference::automatic) {
        std::string selection_error;
        const auto selected = select_hotspot_channel(*iface, *channel, config->band, selection_error);
        if (!selected) return result(Severity::warning, "Configured " +
            std::string(band_preference_name(config->band)) + " band is not currently available: " + selection_error, 3);
        return result(Severity::success, "Wi-Fi concurrency is advertised for " +
            std::string(band_preference_name(config->band)) + " on channel " + *selected + ".", 0);
    }
    return result(Severity::success, "Wi-Fi and hotspot concurrency is advertised on " +
                  channel_band_name(*channel) + " channel " + *channel + ".", 0);
}

OperationResult HotspotService::setup() const {
    std::ostringstream report;
    report << "Read-only setup diagnostics (no settings were changed)\n";

    const bool nmcli_found = executable_available("nmcli");
    const auto nm = nmcli_found ? run_process({"nmcli", "general", "status"}) : ProcessResult{};
    const bool nm_ready = nmcli_found && nm.exit_code == 0;
    report << "- NetworkManager / nmcli: "
           << (nm_ready ? "available" : nmcli_found ? "not responding" : "nmcli not found") << '\n';

    const bool iw_found = executable_available("iw");
    report << "- iw: " << (iw_found ? "available" : "not found") << '\n';
    report << "- pkexec: " << (executable_available("pkexec") ? "available" : "not found") << '\n';

    std::optional<std::string> iface;
    if (nm_ready && iw_found) iface = active_wifi();
    if (!iface) {
        report << "- Wi-Fi uplink: " << (nm_ready && iw_found ? "no connected Wi-Fi interface found" : "not checked") << '\n';
    } else {
        report << "- Wi-Fi uplink: connected on " << *iface << '\n';
        const auto channel = connected_channel(*iface);
        report << "- Current Wi-Fi channel: " << (channel ? *channel + " (" + channel_band_name(*channel) + ")" : "unavailable") << '\n';
        const auto info = phy_info(*iface);
        if (!info) report << "- Wi-Fi and hotspot concurrency: couldn't inspect adapter capabilities\n";
        else if (combination_supports_ap_sta(*info, 1))
            report << "- Wi-Fi and hotspot concurrency: adapter advertises support (runtime still needs validation)\n";
        else report << "- Wi-Fi and hotspot concurrency: adapter does not advertise concurrent client/AP support\n";
    }

    std::string ufw_error;
    if (!executable_available("ufw")) {
        report << "- UFW: not installed; apctl manages no firewall rules on this system\n";
    } else {
        const auto ufw = run_process({"ufw", "--version"});
        const auto firewall = ufw.exit_code == 0 ? ufw_state(ufw_error) : UfwState::unavailable;
        if (firewall == UfwState::enabled) report << "- UFW: installed and configured enabled\n";
        else if (firewall == UfwState::disabled) report << "- UFW: installed and configured disabled\n";
        else report << "- UFW: installed; enabled state couldn't be confirmed" << (ufw_error.empty() ? "\n" : ": " + ufw_error + "\n");
    }

    const auto vpn = nordvpn_info();
    if (!vpn.installed) report << "- NordVPN: not found\n";
    else if (!vpn.settings_available) report << "- NordVPN: installed; settings unavailable, firewall state unknown\n";
    else if (!vpn.firewall_enabled) report << "- NordVPN firewall: disabled\n";
    else if (vpn.hotspot_subnet_allowlisted)
        report << "- NordVPN firewall: enabled; hotspot subnet 10.42.0.0/24 is allowlisted\n";
    else
        report << "- NordVPN firewall: enabled; hotspot clients may be blocked\n"
               << "  To allow hotspot traffic outside the VPN, run: nordvpn allowlist add subnet 10.42.0.0/24\n"
               << "  This changes NordVPN's persistent policy. apctl will not run it automatically.\n";

    if (polkit_agent_process_detected())
        report << "- Polkit session: a common authentication-agent process was detected; registration/readiness can't be confirmed\n";
    else
        report << "- Polkit session: no common authentication-agent process detected; it may be integrated into your desktop shell\n"
               << "  Privileged setup may need a working session agent. apctl won't install agents or edit desktop settings.\n";

    report << "- Hotspot credentials: " << saved_config_status() << '\n';
    std::error_code ec;
    report << "- Saved app setup state: " << (fs::exists(state_path(), ec) && !ec ? "present" : "not present") << '\n';

    const auto current = status();
    report << "- Hotspot: " << (current.hotspot_active ? "active" : "stopped") << '\n';
    return result(Severity::info, report.str());
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

OperationResult HotspotService::change_band(BandPreference band) const {
    auto config = load_config();
    if (!config) return result(Severity::error, "No saved hotspot details. Generate them first.", 2);
    if (config->band == band)
        return result(Severity::info, "Hotspot band is already set to " + std::string(band_preference_name(band)) + ".");

    const auto state = load_state();
    const bool owned_profile = !state.profile_uuid.empty() && connection_exists_uuid(state.profile_uuid);
    std::optional<std::string> selected_channel;
    if (const auto iface = active_wifi()) {
        const auto uplink_channel = connected_channel(*iface);
        if (!uplink_channel) return result(Severity::error, "Couldn't read the Wi-Fi channel. Reconnect and retry.", 3);
        std::string selection_error;
        selected_channel = select_hotspot_channel(*iface, *uplink_channel, band, selection_error);
        if (!selected_channel) return result(Severity::error, selection_error, 3);
    } else if (owned_profile) {
        return result(Severity::error, "Connect to Wi-Fi before changing the band of an existing hotspot profile.", 3);
    }

    std::string old_channel;
    if (owned_profile) {
        const auto current = run_process({"nmcli", "-g", "802-11-wireless.channel", "connection", "show",
                                          "uuid", state.profile_uuid});
        if (current.exit_code != 0 || trim(current.out).empty())
            return result(Severity::error, "Couldn't read the current hotspot channel. No changes were made.", 1);
        old_channel = trim(current.out);
        std::string error;
        if (!command({"nmcli", "connection", "modify", "uuid", state.profile_uuid,
                      "802-11-wireless.band", network_manager_band(channel_band(*selected_channel)),
                      "802-11-wireless.channel", *selected_channel}, error))
            return result(Severity::error, error, 1);
    }

    const BandPreference previous = config->band;
    config->band = band;
    if (!save_config(*config)) {
        if (owned_profile) {
            std::string ignored;
            (void)command({"nmcli", "connection", "modify", "uuid", state.profile_uuid,
                           "802-11-wireless.band", network_manager_band(channel_band(old_channel)),
                           "802-11-wireless.channel", old_channel}, ignored, true);
        }
        config->band = previous;
        return result(Severity::error, "Couldn't save the hotspot band preference.", 1);
    }

    if (!owned_profile)
        return result(Severity::success, "Hotspot band preference saved as " +
                      std::string(band_preference_name(band)) + ". It will take effect the next time the hotspot starts.");

    std::string error;
    const auto restarted = restart_profile_if_active(state.profile_uuid, error);
    if (!restarted)
        return result(Severity::error, "Band preference saved, but the hotspot couldn't restart. " + error, 1);
    if (*restarted)
        return result(Severity::success, "Hotspot band changed to " + std::string(band_preference_name(band)) +
                      ". Reconnect your devices.");
    return result(Severity::success, "Hotspot band preference saved as " +
                  std::string(band_preference_name(band)) + ". It will take effect the next time the hotspot starts.");
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
            // The profile can outlive the process that created it. In that case
            // startup used to return here without repairing UFW rules that were
            // missing (or never recorded) for this hotspot.
            if (const auto iface = active_wifi()) state.uplink = *iface;
            if (state.uplink.empty())
                return result(Severity::error,
                    "Hotspot is running, but its Wi-Fi connection couldn't be identified to finish sharing setup.", 1);
            std::string firewall_error;
            const auto plan = firewall_add_plan("ap0", state.uplink, firewall_error);
            if (!plan)
                return result(Severity::error,
                    "Hotspot is running, but firewall setup couldn't be completed. " + firewall_error, 1);
            if (!save_state(state))
                return result(Severity::error,
                    "Hotspot is running, but its setup state couldn't be saved. No firewall changes were made.", 1);
            if (plan->dhcp || plan->dns || plan->route) {
                const auto child = privileged_batch({"firewall-add", plan->dhcp ? "1" : "0", plan->dns ? "1" : "0",
                                                     plan->route ? "1" : "0", state.uplink}, firewall_error);
                if (helper_completed(child, "dhcp")) state.dhcp = true;
                if (helper_completed(child, "dns")) state.dns = true;
                if (helper_completed(child, "route")) state.route = true;
                if (!save_state(state))
                    return result(Severity::error, "Hotspot is running, but its firewall ownership state couldn't be saved.", 1);
                if (child.exit_code != 0)
                    return result(Severity::error,
                        "Hotspot is running, but firewall setup couldn't be completed. " + firewall_error, 1);
            }
            const auto vpn = nordvpn_info();
            const bool nordvpn_warning = nordvpn_needs_warning(vpn);
            auto already = result(nordvpn_warning ? Severity::warning : Severity::info,
                                  nordvpn_warning ? nordvpn_firewall_message(vpn) : "Hotspot is already running.");
            already.credentials = load_credentials(true);
            return already;
        }
    }

    const auto iface = active_wifi();
    if (!iface) return result(Severity::error, "Connect to Wi-Fi first, then start the hotspot.", 3);
    const auto uplink_channel = connected_channel(*iface);
    if (!uplink_channel) return result(Severity::error, "Couldn't read the Wi-Fi channel. Reconnect and retry.", 3);
    if (!concurrent_capability(*iface)) return result(Severity::error,
        "This computer doesn't report support for Wi-Fi and hotspot at the same time.", 3);

    std::string selection_error;
    const auto channel = select_hotspot_channel(*iface, *uplink_channel, config->band, selection_error);
    if (!channel) return result(Severity::error, selection_error, 3);
    const std::string band = network_manager_band(channel_band(*channel));

    constexpr std::string_view ap = "ap0";
    constexpr std::string_view profile = "apctl-hotspot";
    const auto existing = run_process({"iw", "dev", std::string(ap), "info"});
    const bool create_interface = existing.exit_code != 0;
    std::string phy_index = "0";
    if (create_interface) {
        const auto phy = run_process({"iw", "dev", *iface, "info"});
        const auto wiphy = field(phy.out, "\twiphy ");
        if (!wiphy) return result(Severity::error, "Couldn't identify the Wi-Fi adapter. Refresh status and retry.", 3);
        phy_index = *wiphy;
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
    const auto firewall_plan = firewall_add_plan(ap, *iface, error);
    if (!firewall_plan)
        return result(Severity::error, error.empty() ? "Couldn't finish network sharing setup." : error, 1);
    const auto privileged = privileged_batch({"start", create_interface ? "1" : "0", phy_index,
                                              firewall_plan->dhcp ? "1" : "0", firewall_plan->dns ? "1" : "0",
                                              firewall_plan->route ? "1" : "0", *iface}, error);
    if (helper_completed(privileged, "interface-create")) state.interface_owned = true;
    if (helper_completed(privileged, "dhcp")) state.dhcp = true;
    if (helper_completed(privileged, "dns")) state.dns = true;
    if (helper_completed(privileged, "route")) state.route = true;
    if (!save_state(state)) return result(Severity::error, "Hotspot setup state couldn't be saved after system setup.", 1);
    if (privileged.exit_code != 0) return result(Severity::error, error, 1);
    if (!wait_until_available(ap))
        return result(Severity::error, "Wi-Fi isn't ready to start the hotspot. Refresh status and retry.", 1);

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
        if (!command({"nmcli", "device", "wifi", "hotspot", "ifname", std::string(ap), "con-name", std::string(profile),
                      "ssid", config->ssid, "band", band, "channel", *channel, "password", config->password}, error))
            return result(Severity::error, "NetworkManager couldn't start the hotspot: " + error, 1);
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
            {"nmcli", "connection", "modify", "uuid", current_uuid, "802-11-wireless.band", band},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "802-11-wireless.channel", *channel},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "802-11-wireless.ssid", config->ssid},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "wifi-sec.psk", config->password},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "ipv4.method", "shared"},
            {"nmcli", "connection", "modify", "uuid", current_uuid, "connection.autoconnect", "no"}
        };
        for (const auto& setting : settings) {
            if (!command(setting, error))
                return result(Severity::error,
                    "Couldn't update hotspot setting `" + setting[5] + "`. " + error, 1);
        }
        if (!command({"nmcli", "connection", "up", "uuid", current_uuid, "ifname", std::string(ap)}, error))
            return result(Severity::error, "NetworkManager couldn't activate the hotspot profile. " + error, 1);
    }
    const auto vpn = nordvpn_info();
    const bool nordvpn_warning = nordvpn_needs_warning(vpn);
    auto started = result(nordvpn_warning ? Severity::warning : Severity::success,
                          nordvpn_warning ? nordvpn_firewall_message(vpn) : "Hotspot is ready. Wi-Fi stays connected.");
    started.credentials = load_credentials(true);
    return started;
}

OperationResult HotspotService::stop() const {
    constexpr std::string_view ap = "ap0";
    auto state = load_state();
    const bool owned_profile_exists = !state.profile_uuid.empty() && connection_exists_uuid(state.profile_uuid);
    if (!owned_profile_exists && connection_name_exists("apctl-hotspot"))
        return result(Severity::error, "An existing hotspot wasn't created by this app, so it was left unchanged.", 3);
    std::string error;
    if (owned_profile_exists) {
        (void)command({"nmcli", "connection", "down", "uuid", state.profile_uuid}, error, true);
        if (!command({"nmcli", "connection", "delete", "uuid", state.profile_uuid}, error)) return result(Severity::error, error, 1);
    }
    bool cleanup_ok = true;
    bool delete_dhcp = false;
    bool delete_dns = false;
    bool delete_route = false;
    const auto ufw = run_process({"ufw", "--version"});
    if (ufw.exit_code != 0 && (state.dhcp || state.dns || state.route)) cleanup_ok = false;
    if (ufw.exit_code == 0) {
        const auto plan_remove = [&](std::vector<std::string> rule, bool& owned, bool& remove) {
            if (!owned) return;
            const auto presence = ufw_rule_presence(rule);
            if (presence == UfwRulePresence::unreadable) {
                error = "Couldn't inspect UFW's IPv4 rules. The recorded firewall rule was left unchanged.";
                cleanup_ok = false;
            } else if (presence == UfwRulePresence::missing) {
                owned = false;
            } else {
                remove = true;
            }
        };
        plan_remove({"ufw", "delete", "allow", "in", "on", std::string(ap), "to", "any", "port", "67", "proto", "udp"}, state.dhcp, delete_dhcp);
        plan_remove({"ufw", "delete", "allow", "in", "on", std::string(ap), "to", "any", "port", "53", "proto", "udp"}, state.dns, delete_dns);
        if (!state.uplink.empty())
            plan_remove({"ufw", "route", "delete", "allow", "in", "on", std::string(ap), "out", "on", state.uplink,
                         "from", "10.42.0.0/24", "to", "any"}, state.route, delete_route);
        else if (state.route) {
            error = "Couldn't identify the recorded uplink; its UFW route rule was left unchanged.";
            cleanup_ok = false;
        }
    }
    bool delete_interface = false;
    if (state.interface_owned) {
        delete_interface = run_process({"iw", "dev", std::string(ap), "info"}).exit_code == 0;
        if (!delete_interface) state.interface_owned = false;
    }
    if (!save_state(state)) cleanup_ok = false;

    if (delete_interface) (void)command({"nmcli", "device", "set", std::string(ap), "managed", "no"}, error, true);
    if (delete_dhcp || delete_dns || delete_route || delete_interface) {
        const auto child = privileged_batch({"stop", delete_dhcp ? "1" : "0", delete_dns ? "1" : "0",
                                             delete_route ? "1" : "0", state.uplink.empty() ? "ap0" : state.uplink,
                                             delete_interface ? "1" : "0"}, error);
        if (helper_completed(child, "dhcp-delete")) state.dhcp = false;
        if (helper_completed(child, "dns-delete")) state.dns = false;
        if (helper_completed(child, "route-delete")) state.route = false;
        if (helper_completed(child, "interface-delete")) state.interface_owned = false;
        if (!save_state(state)) cleanup_ok = false;
        if (child.exit_code != 0) cleanup_ok = false;
    }
    if (!cleanup_ok) return result(Severity::error, "Hotspot stopped, but some cleanup didn't finish. Run `apctl down` again. " + error, 1);
    std::error_code ignored;
    fs::remove(state_path(), ignored);
    return result(Severity::success, "Hotspot stopped.");
}

}  // namespace apctl::core
