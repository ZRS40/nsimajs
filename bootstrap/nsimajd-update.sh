#!/bin/bash
# Update nsimajd from the existing checkout at /opt/nsimaj (git pull + make).
# Never re-clones. Git/SSH runs as the user recorded at install (.nsimaj-git-user).
# On make failure: roll back to the previous known-good commit and keep running.
set -euo pipefail

ROOT="${NSIMAJ_ROOT:-/opt/nsimaj}"
LOG="${NSIMAJ_UPDATE_LOG:-/var/log/nsimaj-update.log}"
GOOD_FILE="$ROOT/.nsimaj-last-good-tag"
GIT_USER_FILE="$ROOT/.nsimaj-git-user"

mkdir -p "$(dirname "$LOG")"
exec >>"$LOG" 2>&1

log() { printf '%s %s\n' "$(date -Is)" "$*"; }

cd "$ROOT"

git_user="${NSIMAJ_GIT_USER:-}"
if [[ -z "$git_user" && -f "$GIT_USER_FILE" ]]; then
  git_user=$(tr -d '[:space:]' <"$GIT_USER_FILE")
fi
if [[ -z "$git_user" ]]; then
  git_user="root"
fi

git_as_user() {
  if [[ "$(id -u)" -eq 0 ]] && [[ "$git_user" != "root" ]] && id "$git_user" >/dev/null 2>&1; then
    local home sock
    home=$(getent passwd "$git_user" | cut -d: -f6)
    sock="${SSH_AUTH_SOCK:-}"
    if [[ -n "$sock" && -S "$sock" ]]; then
      sudo -u "$git_user" -H env "HOME=$home" "SSH_AUTH_SOCK=$sock" "$@"
    else
      sudo -u "$git_user" -H env "HOME=$home" "$@"
    fi
  else
    "$@"
  fi
}

# Root often owns a different HOME than the checkout; avoid "dubious ownership".
if [[ "$(id -u)" -eq 0 ]]; then
  if ! git config --global --get-all safe.directory 2>/dev/null | grep -qx "$ROOT"; then
    git config --global --add safe.directory "$ROOT" || true
    log "added safe.directory $ROOT for root git"
  fi
fi

git_err=$(mktemp)
if ! git_as_user git rev-parse --git-dir >"$git_err" 2>&1; then
  log "ERROR: $ROOT is not a usable git checkout (as $git_user): $(tr '\n' ' ' <"$git_err")"
  log "HINT: install with bootstrap/install-server.sh (clone once), then update via git pull here"
  rm -f "$git_err"
  exit 1
fi
rm -f "$git_err"

# Prefer SSH remote — curl/https to github.com often fails where git@ works.
origin_url=$(git_as_user git remote get-url origin 2>/dev/null || true)
if [[ "$origin_url" == https://github.com/* ]]; then
  ssh_url="git@github.com:${origin_url#https://github.com/}"
  ssh_url="${ssh_url%.git}.git"
  log "switching origin from HTTPS to SSH: $ssh_url"
  git_as_user git remote set-url origin "$ssh_url" || true
fi

prev=""
if [[ -f "$GOOD_FILE" ]]; then
  prev=$(tr -d '[:space:]' <"$GOOD_FILE")
fi
if [[ -z "$prev" ]]; then
  prev=$(git_as_user git rev-parse --short HEAD 2>/dev/null || true)
fi
prev_full=$(git_as_user git rev-parse HEAD 2>/dev/null || true)

branch=$(git_as_user git rev-parse --abbrev-ref HEAD 2>/dev/null || echo main)
if [[ "$branch" == "HEAD" ]]; then
  branch=$(git_as_user git rev-parse --abbrev-ref origin/HEAD 2>/dev/null | sed 's#.*/##' || echo main)
  log "detached HEAD — checking out $branch"
  git_as_user git checkout -q "$branch" || git_as_user git checkout -q -B "$branch" "origin/$branch"
fi

log "git pull in $ROOT as $git_user (branch=$branch, previous good: ${prev:-none})"
pull_err=$(mktemp)
if ! git_as_user git pull --ff-only origin "$branch" >"$pull_err" 2>&1; then
  log "ff-only pull blocked or failed: $(tr '\n' ' ' <"$pull_err")"
  log "fetch + hard reset to origin/$branch (deploy tree)"
  if ! git_as_user git fetch origin "$branch" >>"$pull_err" 2>&1; then
    log "ERROR: git fetch failed (as $git_user): $(tr '\n' ' ' <"$pull_err")"
    log "HINT: interactive 'git -C $ROOT pull' as $git_user should work with the same SSH key"
    rm -f "$pull_err"
    exit 1
  fi
  git_as_user git reset --hard "origin/$branch" >>"$pull_err" 2>&1 || {
    log "ERROR: reset failed: $(tr '\n' ' ' <"$pull_err")"
    rm -f "$pull_err"
    exit 1
  }
fi
rm -f "$pull_err"

latest=$(git_as_user git describe --tags --always --dirty 2>/dev/null || git_as_user git rev-parse --short HEAD)
log "tree now at $latest"

install_bin() {
  if [[ -x "$ROOT/nsimajd" ]]; then
    install -m 0755 "$ROOT/nsimajd" /usr/local/bin/nsimajd
    log "installed /usr/local/bin/nsimajd"
  fi
}

log "building $latest"
if make -C "$ROOT"; then
  if id "$git_user" >/dev/null 2>&1 && [[ "$git_user" != "root" ]]; then
    chown -R "$git_user":"$git_user" "$ROOT" 2>/dev/null || true
  fi
  install_bin
  echo "$latest" >"$GOOD_FILE"
  log "build ok — restarting nsimajd"
  if systemctl is-enabled nsimajd >/dev/null 2>&1 || \
     systemctl cat nsimajd >/dev/null 2>&1; then
    systemctl restart nsimajd
  else
    log "nsimajd.service not installed; binary updated only"
  fi
  log "update complete: $latest"
  exit 0
fi

log "ERROR: make failed on $latest — rolling back to ${prev_full:-$prev}"
if [[ -n "$prev_full" ]]; then
  git_as_user git reset --hard "$prev_full" || true
  if make -C "$ROOT"; then
    if id "$git_user" >/dev/null 2>&1 && [[ "$git_user" != "root" ]]; then
      chown -R "$git_user":"$git_user" "$ROOT" 2>/dev/null || true
    fi
    install_bin
    echo "${prev:-$prev_full}" >"$GOOD_FILE"
    log "rollback build ok — staying on ${prev:-$prev_full}"
    if systemctl cat nsimajd >/dev/null 2>&1; then
      systemctl restart nsimajd || true
    fi
    exit 1
  fi
  log "ERROR: rollback make also failed"
fi
exit 1
