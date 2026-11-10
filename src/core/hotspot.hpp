#pragma once

#include <optional>
#include <string>

namespace apctl::core {

enum class Severity { info, success, warning, error };

struct Credentials {
    std::string ssid;
    std::string password;
    std::string qr_text;
};

struct OperationResult {
    Severity severity = Severity::info;
    int exit_code = 0;
    std::string message;
    std::optional<Credentials> credentials;
};

struct Status {
    bool network_manager_available = false;
    std::string wifi_device;
    std::string wifi_connection;
    bool hotspot_active = false;
    std::string hotspot_ssid;
    std::string hotspot_channel;
    int connected_devices = 0;
};

class HotspotService {
public:
    Status status() const;
    OperationResult check() const;
    OperationResult generate(std::string ssid = "apctl-hotspot") const;
    OperationResult start() const;
    OperationResult stop() const;
    OperationResult show_credentials() const;
    OperationResult change_password(std::string password) const;
    OperationResult change_ssid(std::string ssid) const;
};

}  // namespace apctl::core
