#!/usr/bin/env bash
# pilot-cli/lib/common.sh — Shared utilities for Pilot CLI
set -euo pipefail

# ──────────────────────────────────────────────────────────────
# Colors & formatting
# ──────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    GREEN=$'\033[0;32m'
    RED=$'\033[0;31m'
    YELLOW=$'\033[0;33m'
    BLUE=$'\033[0;34m'
    CYAN=$'\033[0;36m'
    BOLD=$'\033[1m'
    DIM=$'\033[2m'
    RESET=$'\033[0m'
else
    GREEN="" RED="" YELLOW="" BLUE="" CYAN="" BOLD="" DIM="" RESET=""
fi

# ──────────────────────────────────────────────────────────────
# Logging
# ──────────────────────────────────────────────────────────────
log_info()    { printf "%s %s\n"  "${BLUE}[info]${RESET}"    "$*"; }
log_success() { printf "%s %s\n"  "${GREEN}[  ok]${RESET}"   "$*"; }
log_warn()    { printf "%s %s\n"  "${YELLOW}[warn]${RESET}"  "$*"; }
log_error()   { printf "%s %s\n"  "${RED}[fail]${RESET}"     "$*" >&2; }
log_step()    { printf "\n%s %s\n" "${BOLD}${CYAN}==>${RESET}" "${BOLD}$*${RESET}"; }

# ──────────────────────────────────────────────────────────────
# Dependency checks
# ──────────────────────────────────────────────────────────────
require_cmd() {
    local cmd="$1"
    local hint="${2:-}"
    if ! command -v "$cmd" &>/dev/null; then
        log_error "Required command '$cmd' not found."
        [[ -n "$hint" ]] && log_error "  Hint: $hint"
        exit 1
    fi
}

# ──────────────────────────────────────────────────────────────
# Configuration
# ──────────────────────────────────────────────────────────────
PILOT_DATA_DIR="${PILOT_DATA_DIR:-/tmp/pilot-data}"
PILOT_MODULE_PATH="${PILOT_MODULE_PATH:-/tmp/pilot-logoscore/modules}"
PILOT_CONFIG_DIR="${PILOT_CONFIG_DIR:-${PILOT_DATA_DIR}/.logoscore}"
PILOT_START_TIME_FILE="${PILOT_DATA_DIR}/.pilot_start_time"
PILOT_MODULES="capability_module,lez_wallet_module,delivery_module,storage_module,pilot"

# logoscore CLI — from logos-co/logos-logoscore-cli (NOT logos-liblogos)
if [[ -z "${LOGOSCORE:-}" ]]; then
    LOGOSCORE="$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli*" -type f 2>/dev/null | head -1)"
fi
LOGOSCORE="${LOGOSCORE:-logoscore}"

# logos_host — required for module subprocess spawning
if [[ -z "${LOGOS_HOST_PATH:-}" ]]; then
    LOGOS_HOST_PATH="$(find /nix/store -maxdepth 1 -name "*-logos-liblogos" -type d 2>/dev/null | head -1)/bin/logos_host"
    export LOGOS_HOST_PATH
fi

# Waku peer for delivery_module
export PILOT_WAKU_ADDR="${PILOT_WAKU_ADDR:-/ip4/127.0.0.1/tcp/30303}"

# ──────────────────────────────────────────────────────────────
# Module interaction — inline mode (one-shot calls)
# ──────────────────────────────────────────────────────────────
pilot_call() {
    local method_call="$1"
    local full_cmd="pilot.${method_call}"
    "$LOGOSCORE" -m "${PILOT_MODULE_PATH}" -l "${PILOT_MODULES}" \
        -c "${full_cmd}" --quit-on-finish 2>&1
}

pilot_call_quiet() {
    local method_call="$1"
    local full_cmd="pilot.${method_call}"
    "$LOGOSCORE" -m "${PILOT_MODULE_PATH}" -l "${PILOT_MODULES}" \
        -c "${full_cmd}" --quit-on-finish 2>/dev/null || true
}

# ──────────────────────────────────────────────────────────────
# Daemon management — for persistent agent operation (chat)
# ──────────────────────────────────────────────────────────────
pilot_daemon_start() {
    mkdir -p "${PILOT_DATA_DIR}"
    "$LOGOSCORE" --config-dir "${PILOT_CONFIG_DIR}" \
        -D -m "${PILOT_MODULE_PATH}" > "${PILOT_DATA_DIR}/daemon.log" 2>&1 &
    local pid=$!
    echo "$pid" > "${PILOT_DATA_DIR}/daemon.pid"

    # Poll until daemon is responding (max 15s)
    local attempts=0
    while (( attempts < 15 )); do
        if pilot_daemon_running 2>/dev/null; then
            break
        fi
        sleep 1
        (( attempts++ ))
    done

    if ! kill -0 "$pid" 2>/dev/null; then
        return 1
    fi

    # Load modules
    local IFS=','
    for mod in ${PILOT_MODULES}; do
        "$LOGOSCORE" --config-dir "${PILOT_CONFIG_DIR}" load-module "$mod" >/dev/null 2>&1 || true
    done
    unset IFS

    # Wait until pilot module responds
    local ready=0
    for i in 1 2 3 4 5; do
        if pilot_daemon_call echo ready 2>/dev/null | grep -q "ready"; then
            ready=1
            break
        fi
        sleep 2
    done

    if (( ready )); then
        pilot_daemon_call initialize "${PILOT_DATA_DIR}" >/dev/null 2>&1 || true
    fi

    record_start_time
    return 0
}

pilot_daemon_call() {
    local method="$1"
    shift
    local raw
    raw=$("$LOGOSCORE" --config-dir "${PILOT_CONFIG_DIR}" call pilot "$method" "$@" 2>&1) || true
    if [[ "$raw" == *'"status":"ok"'* ]]; then
        local result
        result=$(echo "$raw" | sed 's/.*"result":"//; s/","status":"ok".*//' | sed 's/\\"/"/g')
        echo "$result"
    elif [[ "$raw" == *"Result:"* ]]; then
        echo "$raw" | sed 's/.*Result: //'
    else
        echo "$raw"
    fi
}

pilot_daemon_stop() {
    "$LOGOSCORE" --config-dir "${PILOT_CONFIG_DIR}" stop 2>/dev/null || true
    rm -f "${PILOT_DATA_DIR}/daemon.pid"
}

pilot_daemon_running() {
    "$LOGOSCORE" --config-dir "${PILOT_CONFIG_DIR}" status --json 2>/dev/null | grep -q '"running"'
}

# ──────────────────────────────────────────────────────────────
# Formatting helpers
# ──────────────────────────────────────────────────────────────
hrule() {
    local char="${1:-═}"
    local width="${2:-50}"
    printf '%*s\n' "$width" '' | tr ' ' "$char"
}

truncate_str() {
    local str="$1"
    local max="${2:-16}"
    if (( ${#str} > max )); then
        printf '%s...' "${str:0:$((max - 3))}"
    else
        printf '%s' "$str"
    fi
}

print_kv() {
    local key="$1"
    local val="$2"
    local width="${3:-14}"
    printf "  %-${width}s %s\n" "${key}:" "$val"
}

# ──────────────────────────────────────────────────────────────
# Uptime
# ──────────────────────────────────────────────────────────────
record_start_time() {
    mkdir -p "$(dirname "$PILOT_START_TIME_FILE")"
    date +%s > "$PILOT_START_TIME_FILE"
}

get_uptime() {
    if [[ -f "$PILOT_START_TIME_FILE" ]]; then
        local start now elapsed h m s
        start=$(< "$PILOT_START_TIME_FILE")
        now=$(date +%s)
        elapsed=$(( now - start ))
        h=$(( elapsed / 3600 ))
        m=$(( (elapsed % 3600) / 60 ))
        s=$(( elapsed % 60 ))
        printf '%dh %dm %ds' "$h" "$m" "$s"
    else
        printf 'unknown'
    fi
}
