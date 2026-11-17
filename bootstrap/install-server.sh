#!/bin/bash
# Install nsimajd as a systemd service by cloning from GitHub into /opt/nsimaj.
set -euo pipefail

DEST="${NSIMAJ_ROOT:-/opt/nsimaj}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
UNIT_DIR=/etc/systemd/system
GIT_USER_FILE=".nsimaj-git-user"
DEFAULT_URL="git@github.com:ZRS40/nsimajs.git"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

# --- Who owns the SSH key for git@github.com? ---
default_user="${NSIMAJ_GIT_USER:-${SUDO_USER:-}}"
if [[ -z "$default_user" || "$default_user" == "root" ]]; then
  default_user="$(logname 2>/dev/null || true)"
fi
if [[ -z "$default_user" || "$default_user" == "root" ]]; then
  default_user="paul"
fi

if [[ -n "${NSIMAJ_GIT_USER:-}" ]]; then
  git_user="$NSIMAJ_GIT_USER"
else
  printf "Unix user whose SSH key can access the git remote [%s]: " "$default_user"
  read -r git_user </dev/tty || true
  git_user="${git_user:-$default_user}"
fi

if ! id "$git_user" >/dev/null 2>&1; then
  echo "ERROR: user '$git_user' does not exist" >&2
  exit 1
fi

git_home=$(getent passwd "$git_user" | cut -d: -f6)
if [[ ! -d "$git_home" ]]; then
  echo "ERROR: home directory for '$git_user' not found ($git_home)" >&2
  exit 1
fi

git_as_user() {
  local sock="${SSH_AUTH_SOCK:-}"
  if [[ -n "$sock" && -S "$sock" ]]; then
    sudo -u "$git_user" -H env "HOME=$git_home" "SSH_AUTH_SOCK=$sock" "$@"
  else
    sudo -u "$git_user" -H env "HOME=$git_home" "$@"
  fi
}

url=$(git -C "$SRC" remote get-url origin 2>/dev/null || true)
url="${url:-$DEFAULT_URL}"

echo "Installing nsimajd from $url -> $DEST (git/SSH as user: $git_user)"

if [[ -d "$DEST/.git" ]]; then
  echo "Existing install at $DEST — fetching and resetting to origin"
  chown -R "$git_user":"$git_user" "$DEST"
  git_as_user git -C "$DEST" remote set-url origin "$url" || true
  git_as_user git -C "$DEST" fetch --tags origin
  # Prefer latest v* tag; else default branch
  latest=$(git_as_user git -C "$DEST" tag -l 'v*' --sort=-v:refname | head -1 || true)
  if [[ -n "$latest" ]]; then
    echo "Checking out tag $latest"
    git_as_user git -C "$DEST" checkout -q "$latest"
  else
    branch=$(git_as_user git -C "$DEST" rev-parse --abbrev-ref origin/HEAD 2>/dev/null | sed 's#.*/##' || echo main)
    echo "No v* tags — checking out origin/$branch"
    git_as_user git -C "$DEST" checkout -q -B "$branch" "origin/$branch"
  fi
else
  if [[ -e "$DEST" ]] && [[ -n "$(ls -A "$DEST" 2>/dev/null || true)" ]]; then
    echo "ERROR: $DEST exists and is not a git repo. Remove it first:" >&2
    echo "  sudo rm -rf $DEST" >&2
    exit 1
  fi
  # Root creates /opt/nsimaj; git_user cannot mkdir under /opt.
  mkdir -p "$DEST"
  chown "$git_user":"$git_user" "$DEST"
  echo "Cloning $url as $git_user ..."
  git_as_user git clone "$url" "$DEST"
  latest=$(git_as_user git -C "$DEST" tag -l 'v*' --sort=-v:refname | head -1 || true)
  if [[ -n "$latest" ]]; then
    echo "Checking out tag $latest"
    git_as_user git -C "$DEST" checkout -q "$latest"
  fi
fi

chown -R "$git_user":"$git_user" "$DEST"
echo "$git_user" >"$DEST/$GIT_USER_FILE"
chown "$git_user":"$git_user" "$DEST/$GIT_USER_FILE"

make -C "$DEST"
chown -R "$git_user":"$git_user" "$DEST"

install -m 0755 "$DEST/nsimajd" /usr/local/bin/nsimajd
echo "Installed /usr/local/bin/nsimajd"

# Daemon (root) must use the same secret/IP as agents — not ~/.config of root.
mkdir -p /etc/nsimaj
user_conf="/home/$git_user/.config/nsimaj/server.conf"
if [[ -f "$user_conf" ]]; then
  install -m 0600 "$user_conf" /etc/nsimaj/server.conf
  echo "Installed /etc/nsimaj/server.conf from $user_conf"
elif [[ ! -f /etc/nsimaj/server.conf ]]; then
  adv=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src"){print $(i+1); exit}}')
  cat >/etc/nsimaj/server.conf <<CFGEOF
secret=nsimaj
port=7422
http_port=7421
bind=0.0.0.0
advertise_ip=${adv:-}
iface=
CFGEOF
  chmod 0600 /etc/nsimaj/server.conf
  echo "Created /etc/nsimaj/server.conf (edit secret/advertise_ip if needed)"
fi

chmod +x "$DEST/bootstrap/install.sh" \
  "$DEST/bootstrap/nsimajd-update.sh" \
  "$DEST/bootstrap/install-server.sh"

for unit in nsimajd.service nsimajd-update.service nsimajd-update.timer; do
  src="$DEST/bootstrap/$unit"
  if [[ ! -f "$src" ]]; then
    echo "missing $src — is this tag/commit too old?" >&2
    exit 1
  fi
  sed "s|/opt/nsimaj|$DEST|g" "$src" >"$UNIT_DIR/$unit"
done

systemctl daemon-reload
# Drop any leftover binary (old --headless) that still holds the ports.
systemctl stop nsimajd.service 2>/dev/null || true
pkill -x nsimajd 2>/dev/null || true
sleep 1
pkill -9 -x nsimajd 2>/dev/null || true
systemctl enable --now nsimajd.service
systemctl enable --now nsimajd-update.timer
# Confirm control socket appeared.
for i in 1 2 3 4 5 6 7 8 9 10; do
  [[ -S /run/nsimajd.sock ]] && break
  sleep 0.3
done
if [[ ! -S /run/nsimajd.sock ]]; then
  echo "WARN: /run/nsimajd.sock missing after start — check: journalctl -u nsimajd -n 50" >&2
  systemctl status nsimajd --no-pager -l || true
fi

tag=$(git_as_user git -C "$DEST" describe --tags --always 2>/dev/null || echo unknown)
echo "$tag" >"$DEST/.nsimaj-last-good-tag"
chown "$git_user":"$git_user" "$DEST/.nsimaj-last-good-tag"

cat <<EOF

nsimajd installed and running from GitHub.
Binary on PATH: /usr/local/bin/nsimajd
Git/SSH user: $git_user (saved in $DEST/$GIT_USER_FILE)

Daily use (service stays running; TUI attaches):
  nsimajd

Self-update:
  sudo nsimajd update
  # or in the TUI: press S

Release a new version:
  git tag v0.2.0 && git push origin v0.2.0
  sudo nsimajd update

Note: auto-update needs a usable SSH key for $git_user (agent or
passphrase-less key in $git_home/.ssh).

EOF
