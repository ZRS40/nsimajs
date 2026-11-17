#!/bin/bash
# Update nsimajd from the latest git tag (v*), rebuild, restart.
# On make failure: roll back to the previous known-good tag and keep running.
# Git/SSH runs as the user recorded at install (.nsimaj-git-user).
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
    local home
    home=$(getent passwd "$git_user" | cut -d: -f6)
    sudo -u "$git_user" -H env "HOME=$home" "$@"
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
  rm -f "$git_err"
  exit 1
fi
rm -f "$git_err"

prev=""
if [[ -f "$GOOD_FILE" ]]; then
  prev=$(cat "$GOOD_FILE")
elif git_as_user git describe --tags --exact-match HEAD >/dev/null 2>&1; then
  prev=$(git_as_user git describe --tags --exact-match HEAD)
else
  prev=$(git_as_user git describe --tags --always HEAD 2>/dev/null || true)
fi

log "fetching tags as $git_user (previous good: ${prev:-none})"
if ! git_as_user git fetch --tags origin; then
  log "ERROR: git fetch failed (check SSH key for user $git_user)"
  exit 1
fi

latest=$(git_as_user git tag -l 'v*' --sort=-v:refname | head -1 || true)
if [[ -z "$latest" ]]; then
  log "no v* tags found; trying origin/HEAD"
  latest=$(git_as_user git rev-parse --abbrev-ref origin/HEAD 2>/dev/null | sed 's#.*/##' || echo main)
  git_as_user git checkout -q "$latest"
  git_as_user git pull --ff-only origin "$latest" || true
else
  log "latest tag: $latest"
  if [[ -n "$prev" && "$prev" == "$latest" ]]; then
    if [[ -x "$ROOT/nsimajd" ]]; then
      log "already on $latest — nothing to do"
      exit 0
    fi
  fi
  git_as_user git checkout -q "$latest"
fi

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
  log "update complete: $latest ($(git_as_user git describe --tags --always --dirty))"
  exit 0
fi

log "ERROR: make failed on $latest — rolling back to ${prev:-unknown}"
if [[ -n "$prev" ]]; then
  git_as_user git checkout -q "$prev" || true
  if make -C "$ROOT"; then
    if id "$git_user" >/dev/null 2>&1 && [[ "$git_user" != "root" ]]; then
      chown -R "$git_user":"$git_user" "$ROOT" 2>/dev/null || true
    fi
    install_bin
    echo "$prev" >"$GOOD_FILE"
    log "rollback build ok — staying on $prev"
    if systemctl cat nsimajd >/dev/null 2>&1; then
      systemctl restart nsimajd || true
    fi
    exit 1
  fi
  log "ERROR: rollback make also failed"
fi
exit 1
