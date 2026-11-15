# apctl-hotspot

A terminal tool to start a Wi-Fi hotspot on Linux while staying connected to Wi-Fi.

`apctl` uses NetworkManager to manage the hotspot, share the uplink, and show credentials as text and a QR code. The project/repository name is `apctl-hotspot`.

## Preview

Run `apctl tui` for a full-screen interface with separate core, CLI, and TUI code. It uses FTXUI, keeps the terminal responsive while NetworkManager actions run, and restores the terminal when you quit. The home panel grows with the terminal width; credential forms stay compact. Action output appears in a separate result dialog, and the centered Working dialog keeps a fixed size while its indicator animates.

## Install

There is no AUR package yet. Install a C++17 compiler, CMake, Ninja, NetworkManager, `iw`, and `pkexec` using your distribution's package manager, then build from source. CMake fetches the pinned FTXUI dependency on the first configure, so that step needs network access.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The resulting executable is `build/apctl`. To install it under `/usr/local`:

```sh
sudo cmake --install build --prefix /usr/local
```

## Usage

### All commands

| Command | Usage |
| --- | --- |
| `help` | `apctl help` — list commands. `apctl -h` and `apctl --help` are aliases. |
| `setup` | `apctl setup` — run read-only checks for dependencies, Wi-Fi capability, firewall/VPN state, credentials, and authorization-agent hints. |
| `check` | `apctl check` — check concurrent Wi-Fi/hotspot support and the current channel. |
| `status` | `apctl status` — show the Wi-Fi connection, hotspot state, and connected device count. |
| `devices` | `apctl devices` — list connected clients as name, total MB transferred, and connection time. Add `-v` for full details. |
| `gen` | `apctl gen [SSID]` — generate and save credentials. The SSID is optional; the default is `apctl-hotspot`. |
| `up` | `apctl up` — start the hotspot and display its credentials and QR code. |
| `open` | `apctl open` — alias for `up`. |
| `show-password` | `apctl show-password` — display the hotspot name, password, and QR code. |
| `change-password` | `apctl change-password` — securely prompt for and confirm a new password. |
| `change-ssid` | `apctl change-ssid` — prompt for a new hotspot name. |
| `change-band` | `apctl change-band <auto|2.4|5>` — select automatic, 2.4 GHz, or 5 GHz hotspot band. |
| `down` | `apctl down` — stop the app-managed hotspot and clean up its recorded rules. |
| `cleanup` | `apctl cleanup` — stop and clean up an app-owned hotspot left by an earlier run. |
| `tui` | `apctl tui` — open the full-screen interactive terminal interface. |

### Typical flow

```sh
apctl setup
apctl check
apctl gen "My Hotspot"
apctl change-band auto
apctl up
# Later:
apctl down
# Or open the interactive interface:
apctl tui
```

In the TUI, use Up/Down and Enter to navigate or run an action, click a button to run it, and use the scroll wheel to move through the buttons. Esc or `q` quits. Status refreshes silently every six seconds; selecting “Refresh status” shows a brief one-second loading indicator. Connected devices appear below a divider as name, total MB transferred, and connection time. Select “Connected Devices Details” for full device information. Names use the hostname provided in the DHCP lease; if a client does not provide one, apctl uses a short MAC-based label. The CLI command `apctl devices` prints the summary; `apctl devices -v` prints full details. The password and SSID forms accept mouse clicks; password fields hide typed characters. When no valid saved hotspot credentials exist, “Generate hotspot credentials” appears at the top of the action list and disappears after generation succeeds. “Change band” opens a dialog with Auto, 2.4 GHz, and 5 GHz choices; Auto follows the Wi-Fi uplink. “Show password and QR code”, “Setup diagnostics”, and “Check Wi-Fi support” display their output in a separate, centered result dialog; press Enter or left-click to dismiss it. Long-running system actions run in the background while the UI shows a fixed-size animated Working dialog. Start and Stop are disabled whenever their action does not apply. Quitting leaves a running hotspot active; stop it with the button or `apctl down`.

If no SSID is provided to `gen`, it uses `apctl-hotspot`. Passwords are generated and saved with user-only permissions. Changing the password or name while the hotspot is active restarts it, so connected devices need to reconnect.

`change-band` saves a band preference in the user configuration. `auto` follows the connected Wi-Fi channel and remains the default. A forced band uses that channel when the uplink is already on the requested band. Using a different band while preserving the uplink requires the adapter and driver to advertise concurrent operation on separate channels and to report a currently usable AP channel. The command refuses the request when those checks fail; it does not switch or disconnect the uplink. Changing the band while the app-managed hotspot is active restarts it, so clients need to reconnect. `check` evaluates the saved forced-band preference when one is set. Only 2.4 GHz and 5 GHz are supported; 6 GHz is out of scope.

## Authorization prompts and cleanup

`apctl` groups its privileged interface and UFW setup into one `pkexec` request for each hotspot start, and its cleanup into one request for each stop. Your desktop or NetworkManager may still request authorization separately under local policy. Approval is not made permanent: apctl never stores the system password or changes Polkit policy, so later operations can require authorization again.

The CLI exits after `apctl up`, but the hotspot keeps running. Closing the terminal does not stop it. Run `apctl down` or `apctl cleanup` to stop the hotspot, remove the app-created virtual interface, and remove only the UFW rules the app recorded as its own. This also handles saved state migrated from the old Caelestia config location. Profiles and rules without an app ownership record are left untouched.

`apctl setup` is a read-only diagnostic. It does not install software, request elevated permissions, change desktop configuration, or change firewall/VPN settings. Its polkit-agent check is only a process hint; it cannot confirm that an agent is registered or ready to authorize a request. Use one authentication agent for the active desktop session. Noctalia has a built-in agent controlled by `[shell].polkit_agent`; if using a separate agent, disable the built-in one first to avoid competing registrations.

If `pkexec` displays a terminal password prompt instead of a desktop dialog, it may be using its built-in text agent because no session agent is registered. If authentication fails and the system journal reports `No session for cookie`, stop retrying: this can be a Polkit/helper session bug even when the password is accepted. Use a separate session authentication agent or update Polkit, then verify authorization before retrying apctl. Repeated failed attempts can trigger a temporary PAM account lockout. apctl never installs or configures authentication agents. See the [upstream Polkit issue](https://github.com/polkit-org/polkit/issues/686) for one documented occurrence of this failure.

## Requirements and support

| Status | What |
| --- | --- |
| Tested | CachyOS, RTL8852CE adapter (`rtw89_8852ce`), UFW firewall; a phone connected and reached the internet through the laptop. |
| Expected to work | Other Arch-based systems with NetworkManager and compatible Wi-Fi hardware. Not yet verified. |
| Untested | Debian and Fedora. Fedora's default firewalld integration is not handled yet. |
| Requirements | Linux, NetworkManager (`nmcli`), `iw`, `pkexec`, and a Wi-Fi adapter/driver that supports client and hotspot operation at the same time. |

## Known limitations

- The adapter and driver must support concurrent Wi-Fi client and hotspot modes. `apctl check` is only a preflight check; a real client connection is the proof.
- The locally administered AP MAC was validated on the tested adapter; behavior on other drivers is unverified.
- By default, a one-radio hotspot follows the uplink's channel. A forced band on a different channel requires driver-reported multi-channel station/AP support and an allowed AP frequency; this remains adapter and regulatory-domain dependent.
- Firewall setup currently handles UFW rules. Other firewall managers may need separate integration.
- VPN clients can install firewall or kill-switch rules that block hotspot traffic. `apctl setup` checks common Linux clients (NordVPN, Mullvad, Proton VPN, Private Internet Access, ExpressVPN, Surfshark, and Windscribe) when their command-line client is available, and reports only flags it can read. Other providers and settings that a client does not expose are reported as unknown; NetworkManager-managed VPN connections are also detected generically. These checks do not change VPN settings. For NordVPN, the diagnostic checks whether `10.42.0.0/24` is allowlisted and shows `nordvpn allowlist add subnet 10.42.0.0/24` when needed. NordVPN documents this as a persistent policy change that lets the subnet bypass its VPN firewall; apctl does not run the command.
- Starting the hotspot can prompt for system authorization through `pkexec`.
- The current build passes the hotspot password to NetworkManager as a process argument. Other local users may be able to inspect it while the command runs.
- The project has been tested on one laptop/adapter combination so far.

## Contributing

Help test `apctl-hotspot` on other distributions, Wi-Fi adapters, drivers, and firewall setups. Please include the distro, adapter/driver, firewall, and whether a client received an address and internet access.

See [PLAN.md](PLAN.md) for planned work and [docs/findings.md](docs/findings.md) for the test record.
