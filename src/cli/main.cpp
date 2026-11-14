#include "../core/hotspot.hpp"
#include "../core/process.hpp"
#include "../tui/tui.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <termios.h>
#include <unistd.h>

namespace {

void print_result(const apctl::core::OperationResult& result) {
    std::ostream& output = result.severity == apctl::core::Severity::error ? std::cerr : std::cout;
    if (!result.message.empty()) output << result.message << '\n';
    if (!result.credentials) return;
    std::cout << "SSID: " << result.credentials->ssid << "\nPassword: " << result.credentials->password << '\n';
    if (!result.credentials->qr_text.empty()) std::cout << result.credentials->qr_text;
}

enum class DiagnosticTone { normal, good, warning, error };

DiagnosticTone diagnostic_tone(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value.find("not found") != std::string::npos ||
        value.find("not responding") != std::string::npos ||
        value.find("does not advertise") != std::string::npos ||
        value.find("not allowlisted") != std::string::npos ||
        value.find("blocked") != std::string::npos ||
        value.find("incomplete") != std::string::npos ||
        value.find("unreadable") != std::string::npos ||
        value.find("no saved credentials") != std::string::npos)
        return DiagnosticTone::error;
    if (value.find("unknown") != std::string::npos ||
        value.find("couldn't") != std::string::npos ||
        value.find("could not") != std::string::npos ||
        value.find("not checked") != std::string::npos ||
        value.find("no connected") != std::string::npos ||
        value.find("not installed") != std::string::npos ||
        value.find("unavailable") != std::string::npos ||
        value.find("may block") != std::string::npos ||
        value.find("still needs validation") != std::string::npos ||
        value.find("not present") != std::string::npos ||
        value.find("not detected") != std::string::npos ||
        value.find("no common authentication-agent") != std::string::npos)
        return DiagnosticTone::warning;
    if (value.find("available") != std::string::npos ||
        value.find("connected") != std::string::npos ||
        value.find("advertises support") != std::string::npos ||
        value.find("enabled") != std::string::npos ||
        value.find("disabled") != std::string::npos ||
        value.find("allowlisted") != std::string::npos ||
        value.find("not restricted") != std::string::npos ||
        value.find("credentials are present") != std::string::npos ||
        value.find("configured") != std::string::npos ||
        value.find("active") != std::string::npos ||
        value.find("present") != std::string::npos)
        return DiagnosticTone::good;
    return DiagnosticTone::good;
}

const char* diagnostic_ansi(DiagnosticTone tone) {
    switch (tone) {
        case DiagnosticTone::good: return "\033[32m";
        case DiagnosticTone::warning: return "\033[33m";
        case DiagnosticTone::error: return "\033[31m";
        case DiagnosticTone::normal: return "\033[0m";
    }
    return "\033[0m";
}

void print_setup_diagnostics(const apctl::core::OperationResult& result) {
    const bool use_color = isatty(STDOUT_FILENO) && std::getenv("NO_COLOR") == nullptr;
    std::istringstream lines(result.message);
    std::string line;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (!use_color || colon == std::string::npos) {
            std::cout << line << '\n';
            continue;
        }
        const std::string label = line.substr(0, colon + 1);
        const std::string value = line.substr(colon + 1);
        std::cout << label << diagnostic_ansi(diagnostic_tone(value)) << value << "\033[0m\n";
    }
}

std::optional<std::string> prompt_secret(const char* prompt) {
    const int tty = open("/dev/tty", O_RDWR);
    if (tty < 0) return std::nullopt;
    termios original{};
    if (tcgetattr(tty, &original) != 0) { close(tty); return std::nullopt; }
    termios hidden = original;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    if (tcsetattr(tty, TCSAFLUSH, &hidden) != 0) { close(tty); return std::nullopt; }
    (void)write(tty, prompt, std::char_traits<char>::length(prompt));
    std::string value;
    char ch = 0;
    while (read(tty, &ch, 1) == 1 && ch != '\n' && ch != '\r') value.push_back(ch);
    const bool restored = tcsetattr(tty, TCSAFLUSH, &original) == 0;
    (void)write(tty, "\n", 1);
    close(tty);
    if (!restored) return std::nullopt;
    return value;
}

void print_help() {
    std::cout << "Usage: apctl <command>\n"
              << "  help                 Show this help (-h and --help also work)\n"
              << "  setup                Run read-only setup diagnostics\n"
              << "  check                Check if this computer can share Wi-Fi\n"
              << "  status               Show Wi-Fi and hotspot status\n"
              << "  devices [-v]         List connected devices; -v shows full details\n"
              << "  gen [SSID]           Save an SSID and generate a password\n"
              << "  up                   Start the hotspot\n"
              << "  open                 Alias for up\n"
              << "  show-password        Show SSID, password, and QR code\n"
              << "  change-password      Prompt for a new hotspot password\n"
              << "  change-ssid          Prompt for a new SSID\n"
              << "  change-band <band>   Set hotspot band: auto, 2.4, or 5\n"
              << "  down                 Stop the hotspot\n"
              << "  cleanup              Stop and clean up a recorded hotspot from an earlier run\n"
              << "  tui                  Open the interactive terminal interface\n";
}

std::optional<apctl::core::BandPreference> parse_band(std::string_view value) {
    if (value == "auto") return apctl::core::BandPreference::automatic;
    if (value == "2.4") return apctl::core::BandPreference::ghz_2_4;
    if (value == "5") return apctl::core::BandPreference::ghz_5;
    return std::nullopt;
}

int report(apctl::core::OperationResult result) {
    print_result(result);
    return result.exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    using apctl::core::HotspotService;
    if (argc < 2) { print_help(); return 2; }
    const std::string_view action(argv[1]);
    if (action == "help" || action == "--help" || action == "-h") { print_help(); return 0; }
    if (action == "--privileged-helper") return apctl::core::run_privileged_helper(argc, argv);
    if (action == "tui") return apctl::tui::run();

    HotspotService service;
    if (action == "setup") {
        const auto diagnostics = service.setup();
        print_setup_diagnostics(diagnostics);
        return diagnostics.exit_code;
    }
    if (action == "check") return report(service.check());
    if (action == "status") {
        const auto state = service.status();
        if (!state.network_manager_available) { std::cerr << "Network settings aren't available right now.\n"; return 1; }
        if (!state.wifi_device.empty()) {
            std::cout << "Wi-Fi: connected on " << state.wifi_device;
            if (!state.wifi_connection.empty()) std::cout << " (" << state.wifi_connection << ')';
            std::cout << '\n';
        } else std::cout << "Wi-Fi: not connected\n";
        if (!state.hotspot_active) std::cout << "Hotspot: stopped\n";
        else {
            std::cout << "Hotspot: active (" << state.hotspot_ssid << ')';
            if (!state.hotspot_channel.empty()) {
                if (!state.hotspot_band.empty()) std::cout << " on " << state.hotspot_band;
                std::cout << " channel " << state.hotspot_channel;
            }
            std::cout << "\nConnected devices: " << state.connected_devices << '\n';
        }
        return 0;
    }
    if (action == "devices") {
        if (argc > 3 || (argc == 3 && std::string_view(argv[2]) != "-v")) {
            std::cerr << "Usage: apctl devices [-v]\n";
            return 2;
        }
        if (argc == 3) return report(service.connected_device_details());
        const auto state = service.status();
        if (!state.network_manager_available) {
            std::cerr << "Network settings aren't available right now.\n";
            return 1;
        }
        if (!state.hotspot_active) {
            std::cout << "Hotspot is stopped. No connected devices.\n";
            return 0;
        }
        std::cout << "Connected devices: " << state.devices.size() << '\n';
        if (state.devices.empty()) {
            std::cout << "No devices are currently connected.\n";
            return 0;
        }
        for (size_t index = 0; index < state.devices.size(); ++index) {
            const auto& device = state.devices[index];
            std::cout << (index + 1) << ". " << device.name << " - "
                      << (device.traffic_mb.empty() ? "unknown" : device.traffic_mb) << " MB - "
                      << (device.connected_duration.empty() ? "unknown" : device.connected_duration) << '\n';
        }
        return 0;
    }
    if (action == "gen") return report(service.generate(argc > 2 ? argv[2] : "apctl-hotspot"));
    if (action == "up" || action == "open") return report(service.start());
    if (action == "show-password") return report(service.show_credentials());
    if (action == "down" || action == "cleanup") return report(service.stop());
    if (action == "change-password") {
        const auto first = prompt_secret("New hotspot password (8–63 characters): ");
        if (!first) { std::cerr << "Couldn't read the password.\n"; return 1; }
        const auto confirm = prompt_secret("Repeat new hotspot password: ");
        if (!confirm || *first != *confirm) { std::cerr << "Passwords didn't match. No changes made.\n"; return 2; }
        return report(service.change_password(*first));
    }
    if (action == "change-ssid") {
        std::cout << "New SSID: " << std::flush;
        std::string ssid;
        if (!std::getline(std::cin, ssid)) { std::cerr << "Couldn't read the new SSID.\n"; return 1; }
        return report(service.change_ssid(std::move(ssid)));
    }
    if (action == "change-band") {
        if (argc != 3) {
            std::cerr << "Usage: apctl change-band <auto|2.4|5>\n";
            return 2;
        }
        const auto band = parse_band(argv[2]);
        if (!band) {
            std::cerr << "Choose `auto`, `2.4`, or `5` for the hotspot band.\n";
            return 2;
        }
        return report(service.change_band(*band));
    }
    std::cerr << "Unknown command: " << action << ". Run `apctl help` to see available commands.\n";
    return 2;
}
