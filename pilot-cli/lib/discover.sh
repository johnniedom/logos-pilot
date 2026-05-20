#!/usr/bin/env bash
# pilot-cli/lib/discover.sh — Agent discovery on the LEZ network
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

# ──────────────────────────────────────────────────────────────
# Usage
# ──────────────────────────────────────────────────────────────
discover_usage() {
    cat <<'EOF'
Usage: pilot discover [options] [topic]

Discover peer agents on the LEZ network. Queries the discovery
topic and displays found agents in a table.

Arguments:
  topic           Discovery topic to query (default: "pilot")

Options:
  --timeout SECS  Discovery timeout in seconds (default: 10)
  --json          Output as JSON instead of table
  --data-dir DIR  Data directory (default: /tmp/pilot-data)
  --help          Show this help
EOF
}

# ──────────────────────────────────────────────────────────────
# Parse agent discovery result
# ──────────────────────────────────────────────────────────────
# Expected format from agentDiscover: JSON array of agent cards
# [{"name":"...", "npk":"...", "skills":["...", ...], "address":"..."}, ...]

render_agent_table() {
    local agents_json="$1"

    # Table header
    printf "\n"
    printf "  ${BOLD}Discovered Agents:${RESET}\n"
    printf "  %-20s %-16s %s\n" "Name" "NPK (short)" "Skills"
    printf "  "
    hrule "─" 56
    printf "\n"

    if [[ "$agents_json" == "[]" ]] || [[ -z "$agents_json" ]] || [[ "$agents_json" == "unavailable" ]]; then
        printf "  ${DIM}No agents discovered${RESET}\n"
        printf "\n"
        return
    fi

    # Parse JSON array — line-by-line extraction
    # This is intentionally simple; a real deployment would use jq.
    # We handle both jq-available and jq-absent scenarios.
    if command -v jq &>/dev/null; then
        local count
        count=$(echo "$agents_json" | jq 'length' 2>/dev/null) || count=0

        local i
        for (( i = 0; i < count; i++ )); do
            local name npk skills_count
            name=$(echo "$agents_json" | jq -r ".[$i].name // \"unknown\"" 2>/dev/null)
            npk=$(echo "$agents_json" | jq -r ".[$i].npk // \"\"" 2>/dev/null)
            skills_count=$(echo "$agents_json" | jq ".[$i].skills | length" 2>/dev/null || echo 0)

            local short_npk
            short_npk=$(truncate_str "$npk" 14)

            printf "  %-20s %-16s %s skills\n" \
                "$(truncate_str "$name" 18)" \
                "$short_npk" \
                "$skills_count"
        done
    else
        # Fallback: best-effort parse without jq
        # Extract name fields with grep/sed
        local names
        names=$(echo "$agents_json" | grep -oP '"name"\s*:\s*"[^"]*"' 2>/dev/null || true)

        if [[ -z "$names" ]]; then
            # Try to at least show the raw data
            printf "  ${DIM}(install jq for formatted output)${RESET}\n"
            printf "  %s\n" "$agents_json"
        else
            echo "$agents_json" | grep -oP '"name"\s*:\s*"\K[^"]*' 2>/dev/null | while read -r name; do
                printf "  %-20s %-16s %s\n" "$name" "..." "?"
            done
        fi
    fi

    printf "\n"
}

render_agent_json() {
    local agents_json="$1"

    if command -v jq &>/dev/null; then
        echo "$agents_json" | jq '.' 2>/dev/null || echo "$agents_json"
    else
        echo "$agents_json"
    fi
}

# ──────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────
discover_main() {
    local topic="pilot"
    local timeout=10
    local json_output=false

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --timeout)   timeout="$2"; shift 2 ;;
            --json)      json_output=true; shift ;;
            --data-dir)  PILOT_DATA_DIR="$2"; shift 2 ;;
            --help)      discover_usage; exit 0 ;;
            -*)          log_error "Unknown option: $1"; discover_usage; exit 1 ;;
            *)           topic="$1"; shift ;;
        esac
    done

    require_cmd logoscore "Install logoscore from the Logos SDK"

    if [[ "$json_output" == "false" ]]; then
        log_info "Discovering agents on topic '${topic}'..."
    fi

    local agents_result
    agents_result=$(pilot_call "agentDiscover(${topic})" 2>&1) || agents_result="[]"

    if [[ "$json_output" == "true" ]]; then
        render_agent_json "$agents_result"
    else
        render_agent_table "$agents_result"
    fi
}

# Allow sourcing without executing
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    discover_main "$@"
fi
