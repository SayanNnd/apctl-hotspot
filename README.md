# apctl

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

```sh
apctl check        # Check Wi-Fi/hotspot concurrency
apctl gen [SSID]   # Save a name and generate a password
apctl up           # Start the hotspot
apctl down         # Stop it
```

`apctl up` prints the hotspot name, password, and QR code. Other commands include `status`, `show-password`, `change-password`, `change-ssid`, and `help`.

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
