# apctl 0.1.0 for Linux

This directory contains the `apctl` executable. It is a Linux command-line tool with an interactive terminal interface for managing a Wi-Fi hotspot through NetworkManager.

## Runtime requirements

The target system must provide:

- NetworkManager and `nmcli`
- `iw`
- polkit and `pkexec`
- A Wi-Fi adapter and driver that support client and hotspot mode at the same time

UFW is supported for firewall setup. Other firewall managers are not currently handled. The hotspot may require authorization through `pkexec` when it starts.

This build has only been verified on CachyOS with a Realtek RTL8852CE adapter (`rtw89_8852ce`) and UFW. Other distributions, adapters, and drivers are unverified.

## Install for your user

From this directory, run:

```sh
install -Dm755 apctl "$HOME/.local/bin/apctl"
```

If `apctl` is not found after installation, add this to your shell startup file, such as `~/.bashrc` or `~/.zshrc`:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Open a new terminal, then run `apctl help` or `apctl tui`.

## Install system-wide

From this directory, run:

```sh
sudo install -Dm755 apctl /usr/local/bin/apctl
```

Then run `apctl help` or `apctl tui` from a terminal.

## Notes

- This executable does not bundle NetworkManager, `iw`, polkit, or a Wi-Fi driver.
- Use `apctl down` to stop an app-managed hotspot. Quitting the TUI leaves an active hotspot running.
- While changing hotspot settings, the password is briefly passed to NetworkManager as a process argument and may be visible to other local users.
