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
PILOT_MODULE_PATH="${PILOT_MODULE_PATH:-./result/lib}"
PILOT_CONFIG_DIR="${PILOT_CONFIG_DIR:-${PILOT_DATA_DIR}/.logoscore}"
PILOT_START_TIME_FILE="${PILOT_DATA_DIR}/.pilot_start_time"

# logoscore CLI — from logos-co/logos-logoscore-cli (NOT logos-liblogos)
# Ref: https://github.com/logos-co/logos-logoscore-cli
LOGOSCORE="${LOGOSCORE:-logoscore}"

# ──────────────────────────────────────────────────────────────
# Module interaction — inline mode (one-shot calls)
# ──────────────────────────────────────────────────────────────
# pilot_call "method(args)" — invoke a pilot module method
# Uses inline mode for one-shot calls (deploy, verify, discover)
pilot_call() {
    local method_call="$1"
    local full_cmd="pilot.${method_call}"
    "$LOGOSCORE" -m "${PILOT_MODULE_PATH}" -l pilot \
        --persistence-path "${PILOT_DATA_DIR}" \
        -c "${full_cmd}" --quit-on-finish 2>&1
}

pilot_call_quiet() {
    local method_call="$1"
    local full_cmd="pilot.${method_call}"
    "$LOGOSCORE" -m "${PILOT_MODULE_PATH}" -l pilot \
        --persistence-path "${PILOT_DATA_DIR}" \
        -c "${full_cmd}" --quit-on-finish 2>/dev/null || true
}

# ──────────────────────────────────────────────────────────────
# Daemon management — for persistent agent operation (chat)
# ──────────────────────────────────────────────────────────────
pilot_daemon_start() {
    "$LOGOSCORE" --config-dir "${PILOT_CONFIG_DIR}" \
        -D -m "${PILOT_MODULE_PATH}" \
        --persistence-path "${PILOT_DATA_DIR}" > "${PILOT_DATA_DIR}/daemon.log" 2>&1 &
    local pid=$!
    echo "$pid" > "${PILOT_DATA_DIR}/daemon.pid"
    sleep 2
    if kill -0 "$pid" 2>/dev/null; then
        "$LOGOSCORE" --config-dir "${PILOT_CONFIG_DIR}" load-module pilot --json >/dev/null 2>&1 || true
        return 0
    else
        return 1
    fi
}

pilot_daemon_call() {
    local method="$1"
    shift
    "$LOGOSCORE" --config-dir "${PILOT_CONFIG_DIR}" call pilot "$method" "$@" 2>&1
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

# Truncate a string to a max length, appending "..." if truncated
truncate_str() {
    local str="$1"
    local max="${2:-16}"
    if (( ${#str} > max )); then
        printf '%s...' "${str:0:$((max - 3))}"
    else
        printf '%s' "$str"
    fi
}

# Print a key-value pair with alignment
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
