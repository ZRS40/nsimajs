#!/bin/sh
# nsimaj agent bootstrap — download and run on each student PC:
#   curl -fsSL http://SERVER:7421/install.sh | sudo bash
#
# Placeholders below are replaced by the server when the script is downloaded.
# Prompts always go through /dev/tty so they work under `curl | sudo bash`.
set -eu

DEFAULT_SERVER="__NSIMAJ_SERVER__"
DEFAULT_PORT="__NSIMAJ_PORT__"
DEFAULT_HTTP_PORT="__NSIMAJ_HTTP_PORT__"
DEFAULT_SECRET="__NSIMAJ_SECRET__"

# Clear unreplaced placeholders
case "${DEFAULT_SERVER}" in __*) DEFAULT_SERVER="" ;; esac
case "${DEFAULT_PORT}" in __*) DEFAULT_PORT="7422" ;; esac
case "${DEFAULT_HTTP_PORT}" in __*) DEFAULT_HTTP_PORT="7421" ;; esac
case "${DEFAULT_SECRET}" in __*) DEFAULT_SECRET="" ;; esac

SERVER="${1:-}"
SECRET="${2:-}"
PORT="${NSIMAJ_PORT:-$DEFAULT_PORT}"
HTTP_PORT="${NSIMAJ_HTTP_PORT:-$DEFAULT_HTTP_PORT}"

ask() {
    _prompt="$1"
    _def="${2:-}"
    if [ -n "$_def" ]; then
        printf "%s [%s]: " "$_prompt" "$_def" > /dev/tty
    else
        printf "%s: " "$_prompt" > /dev/tty
    fi
    IFS= read -r _ans < /dev/tty || _ans=""
    if [ -z "$_ans" ]; then
        _ans="$_def"
    fi
    printf "%s" "$_ans"
}

ask_secret() {
    _prompt="$1"
    _def="${2:-}"
    if [ -n "$_def" ]; then
        printf "%s [stored/default hidden, Enter to keep]: " "$_prompt" > /dev/tty
    else
        printf "%s: " "$_prompt" > /dev/tty
    fi
    stty -echo < /dev/tty 2>/dev/null || true
    IFS= read -r _ans < /dev/tty || _ans=""
    stty echo < /dev/tty 2>/dev/null || true
    printf "\n" > /dev/tty
    if [ -z "$_ans" ]; then
        _ans="$_def"
    fi
    printf "%s" "$_ans"
}

host_id() {
    if [ -r /etc/hostname ]; then
        tr -d ' \t\r\n' < /etc/hostname
        return
    fi
    if command -v hostname >/dev/null 2>&1; then
        hostname -s 2>/dev/null || hostname
        return
    fi
    if command -v hostnamectl >/dev/null 2>&1; then
        hostnamectl --static 2>/dev/null && return
    fi
    printf "unknown"
}

detect_family() {
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        case "${ID:-}:${ID_LIKE:-}" in
            arch:*|*:*arch*) echo arch; return ;;
            debian:*|ubuntu:*|linuxmint:*|*:*debian*|*:*ubuntu*) echo debian; return ;;
        esac
    fi
    echo unknown
}

install_tmux() {
    if command -v tmux >/dev/null 2>&1; then
        return 0
    fi
    echo "[nsimaj] Installing tmux..." > /dev/tty
    family="$(detect_family)"
    case "$family" in
        arch) pacman -Sy --noconfirm tmux ;;
        debian) DEBIAN_FRONTEND=noninteractive apt-get install -y tmux ;;
        *) echo "[nsimaj] warning: could not auto-install tmux (unknown distro)" > /dev/tty ;;
    esac
}

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run as root (sudo)." >&2
    exit 1
fi

echo "[nsimaj] Agent installer" > /dev/tty

# Reuse existing agent.conf with a single confirm when present
if [ -z "$SERVER" ] && [ -z "$SECRET" ] && [ -r /etc/nsimaj/agent.conf ]; then
    OLD_SERVER="$(sed -n 's/^server=//p' /etc/nsimaj/agent.conf | head -n1)"
    OLD_SECRET="$(sed -n 's/^secret=//p' /etc/nsimaj/agent.conf | head -n1)"
    OLD_PORT="$(sed -n 's/^port=//p' /etc/nsimaj/agent.conf | head -n1)"
    OLD_HTTP="$(sed -n 's/^http_port=//p' /etc/nsimaj/agent.conf | head -n1)"
    if [ -n "$OLD_SERVER" ] && [ -n "$OLD_SECRET" ]; then
        printf "Reuse saved config (server=%s)? [Y/n]: " "$OLD_SERVER" > /dev/tty
        IFS= read -r _reuse < /dev/tty || _reuse=""
        case "${_reuse}" in
            n|N|no|NO)
                ;;
            *)
                SERVER="$OLD_SERVER"
                SECRET="$OLD_SECRET"
                if [ -n "$OLD_PORT" ]; then
                    PORT="$OLD_PORT"
                fi
                if [ -n "$OLD_HTTP" ]; then
                    HTTP_PORT="$OLD_HTTP"
                fi
                echo "[nsimaj] Reusing /etc/nsimaj/agent.conf" > /dev/tty
                ;;
        esac
    fi
fi

if [ -z "$SERVER" ]; then
    SERVER="$(ask "Server IP / hostname" "$DEFAULT_SERVER")"
fi
if [ -z "$SERVER" ]; then
    echo "Server address is required." >&2
    exit 1
fi

if [ -z "$SECRET" ]; then
    OLD_SECRET=""
    if [ -r /etc/nsimaj/agent.conf ]; then
        OLD_SECRET="$(sed -n 's/^secret=//p' /etc/nsimaj/agent.conf | head -n1)"
    fi
    if [ -n "$OLD_SECRET" ]; then
        SECRET="$(ask_secret "Join secret" "$OLD_SECRET")"
    elif [ -n "$DEFAULT_SECRET" ]; then
        SECRET="$(ask_secret "Join secret" "$DEFAULT_SECRET")"
    else
        SECRET="$(ask_secret "Join secret" "")"
    fi
fi
if [ -z "$SECRET" ]; then
    echo "Join secret is required (must match the server)." >&2
    exit 1
fi

# Only ask ports when not reusing a full saved config interactively earlier
if [ -z "${1:-}" ] && [ ! -r /etc/nsimaj/agent.conf ]; then
    PORT="$(ask "Control port" "$PORT")"
    HTTP_PORT="$(ask "HTTP bootstrap port" "$HTTP_PORT")"
elif [ -z "${1:-}" ]; then
    : # keep defaults / reused port
fi

echo "[nsimaj] Downloading / updating nsimaj-agent from http://${SERVER}:${HTTP_PORT}/ ..." > /dev/tty

# Open outbound paths in ufw when the firewall is active (agent dials the server)
if command -v ufw >/dev/null 2>&1; then
    if ufw status 2>/dev/null | head -n1 | grep -qi active; then
        echo "[nsimaj] Configuring ufw outbound rules toward ${SERVER}..." > /dev/tty
        ufw allow out proto tcp to "${SERVER}" port "${HTTP_PORT}" comment 'nsimaj-bootstrap' || true
        ufw allow out proto tcp to "${SERVER}" port "${PORT}" comment 'nsimaj-control' || true
    fi
fi

install_tmux || true

tmpdir="$(mktemp -d)"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

curl -fsSL "http://${SERVER}:${HTTP_PORT}/nsimaj-agent" -o "$tmpdir/nsimaj-agent"
curl -fsSL "http://${SERVER}:${HTTP_PORT}/nsimaj-agent.service" -o "$tmpdir/nsimaj-agent.service"

install -d -m 0755 /usr/local/bin
install -d -m 0755 /etc/nsimaj
install -m 0755 "$tmpdir/nsimaj-agent" /usr/local/bin/nsimaj-agent

HOST="$(host_id)"
cat > /etc/nsimaj/agent.conf <<EOF
server=${SERVER}
port=${PORT}
http_port=${HTTP_PORT}
secret=${SECRET}
id=${HOST}
EOF
chmod 0600 /etc/nsimaj/agent.conf

install -m 0644 "$tmpdir/nsimaj-agent.service" /etc/systemd/system/nsimaj-agent.service
systemctl daemon-reload
systemctl enable nsimaj-agent.service
systemctl restart nsimaj-agent.service

echo "[nsimaj] Saved /etc/nsimaj/agent.conf (server=${SERVER}, id=${HOST})" > /dev/tty
echo "[nsimaj] Agent installed/updated and (re)started." > /dev/tty
systemctl --no-pager --full status nsimaj-agent.service || true
