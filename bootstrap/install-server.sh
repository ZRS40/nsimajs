#!/bin/bash
# Install nsimajd as a systemd service from this source tree.
# Content always comes from the local working tree (not a fresh GitHub clone),
# so unpushed local changes are included. Git remote is kept for later updates.
set -euo pipefail

DEST="${NSIMAJ_ROOT:-/opt/nsimaj}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
UNIT_DIR=/etc/systemd/system
GIT_USER_FILE=".nsimaj-git-user"

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

# Run git as that user, keeping SSH agent if sudo forwarded it.
git_as_user() {
  local sock="${SSH_AUTH_SOCK:-}"
  if [[ -n "$sock" && -S "$sock" ]]; then
    sudo -u "$git_user" -H env "HOME=$git_home" "SSH_AUTH_SOCK=$sock" "$@"
  else
    sudo -u "$git_user" -H env "HOME=$git_home" "$@"
  fi
}

echo "Installing nsimajd from $SRC -> $DEST (git/SSH as user: $git_user)"

mkdir -p "$DEST"

if [[ "$SRC" != "$DEST" ]]; then
  echo "Syncing local tree (includes unpushed files) ..."
  rsync -a --delete \
    --exclude '.nsimaj-last-good-tag' \
    --exclude '.nsimaj-git-user' \
    "$SRC"/ "$DEST"/
fi

# Tree owned by git user so fetch/checkout work during updates.
chown -R "$git_user":"$git_user" "$DEST"
echo "$git_user" >"$DEST/$GIT_USER_FILE"
chown "$git_user":"$git_user" "$DEST/$GIT_USER_FILE"

if [[ -d "$DEST/.git" ]]; then
  echo "Checking git fetch as $git_user ..."
  if git_as_user git -C "$DEST" fetch --tags origin; then
    echo "git remote OK"
  else
    echo "WARN: git fetch failed — install continues; fix SSH before auto-update" >&2
  fi
else
  echo "WARN: no .git in $DEST — auto-update via tags will not work" >&2
fi

make -C "$DEST"
chown -R "$git_user":"$git_user" "$DEST"

chmod +x "$DEST/bootstrap/install.sh" \
  "$DEST/bootstrap/nsimajd-update.sh" \
  "$DEST/bootstrap/install-server.sh"

# Units use /opt/nsimaj; rewrite if DEST differs.
for unit in nsimajd.service nsimajd-update.service nsimajd-update.timer; do
  src="$DEST/bootstrap/$unit"
  if [[ ! -f "$src" ]]; then
    echo "missing $src" >&2
    exit 1
  fi
  sed "s|/opt/nsimaj|$DEST|g" "$src" >"$UNIT_DIR/$unit"
done

systemctl daemon-reload
systemctl enable --now nsimajd.service
systemctl enable --now nsimajd-update.timer

tag=$(git_as_user git -C "$DEST" describe --tags --always 2>/dev/null || echo unknown)
echo "$tag" >"$DEST/.nsimaj-last-good-tag"
chown "$git_user":"$git_user" "$DEST/.nsimaj-last-good-tag"

cat <<EOF

nsimajd installed and running.
Git/SSH operations use user: $git_user (saved in $DEST/$GIT_USER_FILE)

Control the service with:
  systemctl status nsimajd
  systemctl restart nsimajd
  systemctl stop nsimajd

Self-update (git latest v* tag + make + restart):
  systemctl start nsimajd-update
  journalctl -u nsimajd-update -n 50
  systemctl status nsimajd-update.timer

In the TUI, press S (or :svc) to show this cheat-sheet.

Release a new version on the teacher machine:
  git tag v0.2.0 && git push origin v0.2.0

Note: auto-update needs a usable SSH key for $git_user (agent or
passphrase-less key in $git_home/.ssh).

EOF
