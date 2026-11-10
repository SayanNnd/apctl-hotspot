# apctl-hotspot

A terminal tool to start a Wi-Fi hotspot on Linux while staying connected to Wi-Fi.

`apctl` uses NetworkManager to manage the hotspot, share the uplink, and show credentials as text and a QR code. The project/repository name is `apctl-hotspot`.

## Preview

The current build is a command-line tool. A terminal interface (TUI) is planned; there is no TUI screenshot to show yet.

## Install

There is no AUR package yet. Install a C++17 compiler, CMake, Ninja, NetworkManager, `iw`, and `pkexec` using your distribution's package manager, then build from source:

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
| `check` | `apctl check` — check concurrent Wi-Fi/hotspot support and the current channel. |
| `status` | `apctl status` — show the Wi-Fi connection, hotspot state, and connected device count. |
| `gen` | `apctl gen [SSID]` — generate and save credentials. The SSID is optional; the default is `apctl-hotspot`. |
| `up` | `apctl up` — start the hotspot and display its credentials and QR code. |
| `open` | `apctl open` — alias for `up`. |
| `show-password` | `apctl show-password` — display the hotspot name, password, and QR code. |
| `change-password` | `apctl change-password` — securely prompt for and confirm a new password. |
| `change-ssid` | `apctl change-ssid` — prompt for a new hotspot name. |
| `down` | `apctl down` — stop the app-managed hotspot and clean up its recorded rules. |

### Typical flow

```sh
apctl check
apctl gen "My Hotspot"
apctl up
# Later:
apctl down
```

If no SSID is provided to `gen`, it uses `apctl-hotspot`. Passwords are generated and saved with user-only permissions. Changing the password or name while the hotspot is active restarts it, so connected devices need to reconnect.

## Authorization prompts and cleanup

Starting a hotspot may show more than one system password prompt. These are normal `pkexec` authorization prompts for privileged setup steps: creating and preparing the extra Wi-Fi interface, and—when UFW is active—adding the firewall rules needed for client DHCP, DNS, and internet forwarding. The prompts themselves are harmless: they do not change your login password or install software. Approving them permits those specific network changes. The app does not read or save your system password. Depending on your desktop's authorization policy, it may reuse a recent approval or prompt again for another step.

The CLI exits after `apctl up`, but the hotspot keeps running. Closing the terminal does not stop it. Run `apctl down` to stop the hotspot, remove the app-created virtual interface, and remove only the UFW rules the app recorded as its own. Your existing Wi-Fi connection and unrelated firewall rules are left in place.

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
- While sharing Wi-Fi from one radio, the hotspot must use the uplink's channel.
- Firewall setup currently handles UFW rules. Other firewall managers may need separate integration.
- Starting the hotspot can prompt for system authorization through `pkexec`.
- The current build passes the hotspot password to NetworkManager as a process argument. Other local users may be able to inspect it while the command runs.
- The project has been tested on one laptop/adapter combination so far.

## Contributing

Help test `apctl-hotspot` on other distributions, Wi-Fi adapters, drivers, and firewall setups. Please include the distro, adapter/driver, firewall, and whether a client received an address and internet access.

See [PLAN.md](PLAN.md) for planned work and [docs/findings.md](docs/findings.md) for the test record.
