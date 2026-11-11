# apctl-hotspot Contributor Guide

## Project

Build `apctl`, a Linux terminal tool that starts a Wi-Fi hotspot while keeping the computer connected to Wi-Fi. The project/repository name is `apctl-hotspot`. The implementation is C++17 and currently uses NetworkManager through `nmcli`.

## Current behavior

- CLI commands: `help`, `check`, `status`, `gen [SSID]`, `up` (`open` alias), `show-password`, `change-password`, `change-ssid`, `change-band <auto|2.4|5>`, and `down`.
- `apctl tui` opens the full-screen FTXUI interface. Keep it as a frontend that calls the core service; do not add NetworkManager or firewall logic to UI code.
- The TUI has a Change band dialog with Auto, 2.4 GHz, and 5 GHz choices; it calls the core service.
- Keep code separated by responsibility: CLI entry point under `src/cli`, hotspot and process operations under `src/core`, and interactive interface under `src/tui`.
- Hotspot sharing uses a separate AP virtual interface and a locally administered MAC. By default it follows the connected Wi-Fi channel; a forced alternate band requires advertised multi-channel station/AP support and an allowed AP channel.
- NetworkManager provides the AP profile and shared IPv4. The current firewall integration handles UFW only.
- The tested setup is CachyOS with a Realtek RTL8852CE (`rtw89_8852ce`) and UFW. Other systems and adapters remain unverified.
- An FTXUI-based keyboard and mouse TUI is available through `apctl tui`; backend operations run asynchronously so they do not block rendering.

## Implementation rules

- Keep subprocess arguments as argument vectors. Never use `system()` or build shell commands from user input.
- Capture subprocess stdout, stderr, and exit status separately. Give users actionable messages without exposing unnecessary implementation details.
- Never alter or delete a saved connection or firewall rule unless the app recorded ownership of it.
- Keep credentials in user-only config files. Never log passwords. The current NetworkManager CLI path exposes the password briefly in process arguments; retain this as a documented limitation until a safer API is implemented.
- Privileged interface and firewall operations use `pkexec`. Do not run privileged commands during development without explicit user authorization.
- Concurrent mode is adapter/driver dependent. A capability check is advisory; actual activation and client connectivity are runtime proof.
- `auto` band mode follows the connected Wi-Fi channel. A forced alternate band requires advertised multi-channel station/AP support and an allowed AP channel; do not claim this works on unverified adapters.
- Preserve access to existing `~/.config/caelestia-hotspot` settings during the rename to `~/.config/apctl-hotspot`.

## Build and verification

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Use C++17 and compile with `-Wall -Wextra -Wpedantic`. FTXUI is the approved TUI dependency and its version is pinned in CMake; do not add other dependencies without approval. Do not run tests unless requested. Never test against real networking, firewall, or privileged system state without the user's authorization.

## Documentation

Keep portability claims tied to recorded evidence. The README must distinguish tested, expected, and untested platforms. Fedora firewalld support is not implemented. Update `docs/findings.md` when new hardware or distro behavior is verified.
