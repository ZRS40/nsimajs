# nsimaj

LAN control plane for mass package updates: a **server** on the teacher PC and a
**persistent agent** on each student machine.

Agents connect out to the server, announce they are online (heartbeats), and wait
for orders. Jobs run inside **tmux** so you can focus a host interactively, detach,
and leave the work running.

## Build

```sh
make
```

Produces `nsimajd` (server + TUI) and `nsimaj-agent`, and copies the agent into
`bootstrap/` for HTTP install.

## Server (teacher PC)

```sh
./nsimajd
```

On first launch it lists network interfaces, lets you pick one (IP is taken from
that iface), asks for the **join secret**, then saves `~/.config/nsimaj/server.conf`.
Later launches load that file and start immediately (no re-prompt). If you ever ran
the server with `sudo` and config is not writable, fix with:
`sudo chown -R "$USER":"$USER" ~/.config/nsimaj`.

Headless / non-interactive:

```sh
./nsimajd --headless --secret 'classroom' --advertise-ip 192.168.1.10 -y
```

Options:

| Flag | Default | Meaning |
|------|---------|---------|
| `--secret` | prompt / config | Shared join secret |
| `--advertise-ip` | auto-detect | IP shown in install curl / injected into `install.sh` |
| `--port` | `7422` | Agent control TCP port |
| `--http-port` | `7421` | Bootstrap HTTP port |
| `--assets` | `./bootstrap` | Directory with `install.sh`, agent binary, unit |
| `--bind` | `0.0.0.0` | Listen address |
| `--headless` | off | No TUI; status on stderr, commands on stdin |
| `-y` / `--yes` | off | Skip interactive setup prompts |

### TUI keys

| Key | Action |
|-----|--------|
| `j` / `k` or arrows | Move cursor |
| `space` | Select / deselect host |
| `a` | Select all / clear |
| `u` | Update selected (or all if none selected) — confirms first |
| `e` | Start `exec …` command line |
| `c` | Cancel jobs on selected/all |
| `f` | Focus current host (attach to its tmux job) |
| `Ctrl-N` | Detach from focus (job keeps running) |
| `:` | Command mode (`update`, `exec …`, `cancel`, `focus N`, `quit`) |
| `q` | Quit server |

## Bootstrap each student PC

With the server running (use the command shown in the TUI — IP is filled in):

```sh
curl -fsSL http://192.168.1.10:7421/install.sh | sudo bash
```

The script asks for server IP / secret via the TTY (or offers to reuse an existing
`/etc/nsimaj/agent.conf`), installs `tmux`, saves the agent config (`chmod 0600`),
installs the binary, and enables the systemd service. Host id comes from
`/etc/hostname` (works on Arch without the `hostname` binary).

### Manual agent (dev)

```sh
./nsimaj-agent --server 192.168.1.10 --secret classroom
```

## What `update` does

On each targeted agent:

1. Detect distro family (`debian` for Linux Mint/Ubuntu/Debian, `arch` for Arch)
2. Ensure `tmux` is installed (also installed at bootstrap)
3. Run package upgrade in a detached tmux session
4. Download the latest `nsimaj-agent` (+ unit) from the server HTTP port and restart the service
5. Stream stages: `queued` → `preparing` → `updating_indexes` → `upgrading` → `done`/`failed`

Re-running the install curl on a PC also refreshes the agent binary from the server.

Job history: `~/.config/nsimaj/jobs.log` (or `/tmp/nsimaj-jobs.log` if that directory is not writable).

Note: a full `pacman -Su` / `apt-get upgrade` may update grub/shim and write under
`/boot/efi` (e.g. EFI/Boot/HP on HP machines) — that is the package manager, not
nsimaj copying those folders.

## Firewall (ufw)

- `nsimajd` tries to open **7421/tcp** and **7422/tcp** when ufw is active (needs root or passwordless sudo)
- The install script opens **outbound** rules toward the server on those ports

Manual fallback:

```sh
sudo ufw allow 7421/tcp comment nsimaj-bootstrap
sudo ufw allow 7422/tcp comment nsimaj-control
```

## Security notes

- Designed for a **trusted school LAN**
- Shared secret stops casual registration; there is **no TLS** in v1
- Agents run as **root** (needed for apt/pacman)
- Do not expose ports 7421/7422 beyond the LAN

## Layout

```text
common/     framing, JSON helpers, utils
server/     nsimajd + fleet + HTTP bootstrap + TUI
agent/      nsimaj-agent (daemon)
bootstrap/  install.sh, systemd unit, packaged agent binary
protocol/   message documentation
legacy/     previous ARP+SSH single-binary code
```
