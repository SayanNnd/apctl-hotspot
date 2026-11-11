#include "../core/hotspot.hpp"
#include "../tui/tui.hpp"

#include <fcntl.h>
#include <iostream>
#include <optional>
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
              << "  check                Check if this computer can share Wi-Fi\n"
              << "  status               Show Wi-Fi and hotspot status\n"
              << "  gen [SSID]           Save an SSID and generate a password\n"
              << "  up                   Start the hotspot\n"
              << "  open                 Alias for up\n"
              << "  show-password        Show SSID, password, and QR code\n"
              << "  change-password      Prompt for a new hotspot password\n"
              << "  change-ssid          Prompt for a new SSID\n"
              << "  change-band <band>   Set hotspot band: auto, 2.4, or 5\n"
              << "  down                 Stop the hotspot\n"
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
    if (action == "tui") return apctl::tui::run();

    HotspotService service;
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
    if (action == "gen") return report(service.generate(argc > 2 ? argv[2] : "apctl-hotspot"));
    if (action == "up" || action == "open") return report(service.start());
    if (action == "show-password") return report(service.show_credentials());
    if (action == "down") return report(service.stop());
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
