#pragma once

#include <optional>
#include <string>
#include <vector>

namespace apctl::core {

enum class Severity { info, success, warning, error };
enum class BandPreference { automatic, ghz_2_4, ghz_5 };

struct Credentials {
    std::string ssid;
    std::string password;
    std::string qr_text;
};

struct ConnectedDevice {
    std::string name;
    std::string mac_address;
    std::string ipv4_address;
    std::string signal;
    std::string connected_time;
    std::string connected_duration;
    std::string traffic_mb;
    std::string rx_bytes;
    std::string tx_bytes;
    std::string rx_bitrate;
    std::string tx_bitrate;
};

struct OperationResult {
    Severity severity = Severity::info;
    int exit_code = 0;
    std::string message;
    std::optional<Credentials> credentials;
};

struct Status {
    bool network_manager_available = false;
    bool credentials_configured = false;
    std::string wifi_device;
    std::string wifi_connection;
    bool hotspot_profile_configured = false;
    bool hotspot_active = false;
    std::string hotspot_ssid;
    std::string hotspot_band;
    std::string hotspot_channel;
    int connected_devices = 0;
    std::vector<ConnectedDevice> devices;
};

class HotspotService {
public:
    Status status() const;
    OperationResult connected_device_details() const;
    OperationResult check() const;
    OperationResult setup() const;
    OperationResult generate(std::string ssid = "apctl-hotspot") const;
    OperationResult start() const;
    OperationResult stop() const;
    OperationResult show_credentials() const;
    OperationResult change_password(std::string password) const;
    OperationResult change_ssid(std::string ssid) const;
    OperationResult change_band(BandPreference band) const;
};

}  // namespace apctl::core
