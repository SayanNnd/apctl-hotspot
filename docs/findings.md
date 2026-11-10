# apctl-hotspot Findings

Updated: 2026-11-11

## Tested environment

- CachyOS with NetworkManager and UFW active.
- Realtek RTL8852CE Wi-Fi controller, `rtw89_8852ce` driver version `7.2.8-1-cachyos`.
- `wlan0` connected to the upstream Wi-Fi network; the laptop's uplink stayed active while `ap0` hosted the hotspot.

## Verified behavior

- The adapter advertises concurrent managed Wi-Fi and AP interfaces with one shared channel.
- A separate AP interface with a locally administered MAC address was required. The tested address is `02:88:cd:08:b8:c8`.
- NetworkManager's `nmcli device wifi hotspot` helper on `ap0`, using the uplink's channel, successfully starts the hotspot while `wlan0` remains connected.
- The hotspot uses NetworkManager shared IPv4 mode. The phone associated, received an address, and reached the internet through the laptop's existing Wi-Fi uplink.
- With UFW active, the working setup allowed hotspot DHCP (`67/udp`), DNS (`53/udp`), and routed traffic from the hotspot subnet to the uplink. The app adds/removes its recorded rules.
- The most recent client station output showed authorized/authenticated/associated state, strong signal (-27 dBm), bidirectional traffic, and no transmit retries or failures.

## Current app constraints

- The verified results are from one CachyOS laptop and adapter. Other distributions and hardware have not been tested.
- Concurrent operation must be supported by the adapter and driver. The AP follows the Wi-Fi uplink's channel.
- Firewall integration currently handles UFW; Fedora firewalld is not handled.
- The app uses the tested locally administered MAC address. Other drivers may behave differently.
- The password is passed to NetworkManager as a process argument during profile creation.
- No hotspot password is stored in this document.

## Earlier troubleshooting notes

The first generic NetworkManager AP profile created by the CLI did not accept client connections. The app was changed to use the tested NetworkManager hotspot helper and stable locally administered MAC. Subsequent station output confirmed the phone connected and had internet access.

An unrelated `hostapd` package/library mismatch was encountered during investigation. `hostapd` is not used by the current app path.
