# apctl-hotspot Contributor Guide

## Project

Build `apctl`, a Linux terminal tool that starts a Wi-Fi hotspot while keeping the computer connected to Wi-Fi. The project/repository name is `apctl-hotspot`. The implementation is C++17 and currently uses NetworkManager through `nmcli`.

## Current behavior

- CLI commands: `help`, `check`, `status`, `gen [SSID]`, `up` (`open` alias), `show-password`, `change-password`, `change-ssid`, and `down`.
- Hotspot sharing uses a separate AP virtual interface, a locally administered MAC, and the same channel as the connected Wi-Fi uplink.
- NetworkManager provides the AP profile and shared IPv4. The current firewall integration handles UFW only.
- The tested setup is CachyOS with a Realtek RTL8852CE (`rtw89_8852ce`) and UFW. Other systems and adapters remain unverified.
- A dependency-free keyboard TUI is available through `apctl tui`; fixture-based tests are still planned.

## Implementation rules

- Keep subprocess arguments as argument vectors. Never use `system()` or build shell commands from user input.
- Capture subprocess stdout, stderr, and exit status separately. Give users actionable messages without exposing unnecessary implementation details.
- Never alter or delete a saved connection or firewall rule unless the app recorded ownership of it.
- Keep credentials in user-only config files. Never log passwords. The current NetworkManager CLI path exposes the password briefly in process arguments; retain this as a documented limitation until a safer API is implemented.
- Privileged interface and firewall operations use `pkexec`. Do not run privileged commands during development without explicit user authorization.
- Concurrent mode is adapter/driver dependent. A capability check is advisory; actual activation and client connectivity are runtime proof.
- Preserve access to existing `~/.config/caelestia-hotspot` settings during the rename to `~/.config/apctl-hotspot`.

## Build and verification

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Use C++17 and compile with `-Wall -Wextra -Wpedantic`. Do not add dependencies without approval. Do not run tests unless requested. Never test against real networking, firewall, or privileged system state without the user's authorization.

## Documentation

Keep portability claims tied to recorded evidence. The README must distinguish tested, expected, and untested platforms. Fedora firewalld support is not implemented. Update `docs/findings.md` when new hardware or distro behavior is verified.
