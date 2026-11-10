# apctl-hotspot Plan

## Goal

Build `apctl`, a terminal tool to start a Wi-Fi hotspot on Linux while staying connected to Wi-Fi. NetworkManager is the current backend. Be explicit about the limits of tested hardware and platforms.

## First CLI build

- Commands for checking support, generating and changing credentials, starting/stopping the hotspot, and showing status: implemented.
- Concurrent Wi-Fi + AP interface setup using NetworkManager, including same-channel selection: implemented.
- UFW DHCP, DNS, and forwarding rules with ownership-aware cleanup: implemented for the tested setup.
- Credential QR output and user-friendly CLI messages: implemented.
- Full-screen FTXUI TUI with mouse support, modal results, and asynchronous backend actions: implemented; runtime polish pending.
- Build: C++17 with CMake/Ninja.

## Verified acceptance scenario

On the tested CachyOS laptop with RTL8852CE, the laptop stays connected to Wi-Fi while a phone joins the hotspot, receives DHCP, and reaches the internet through the same uplink. See [docs/findings.md](docs/findings.md).

## Next work

1. Test `apctl` on more adapters and Linux distributions.
2. Add firewalld support and make firewall integration clearer for other systems.
3. Add fixture-based tests for command parsing and state handling.
4. Refine TUI presentation and accessibility after runtime feedback.
5. Package and publish `apctl-hotspot` after portability testing.

## Known limits

- A reported driver capability does not guarantee a working AP; validate with client association and internet access.
- One Wi-Fi radio generally requires the AP to follow the uplink channel.
- Firewall behavior outside UFW has not been verified; Fedora firewalld support is not implemented.
- The NetworkManager CLI receives the password as a process argument during setup.
