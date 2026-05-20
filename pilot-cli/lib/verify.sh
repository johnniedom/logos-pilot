#!/usr/bin/env bash
# pilot-cli/lib/verify.sh — Evidence collection for evaluators
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

# ──────────────────────────────────────────────────────────────
# Usage
# ──────────────────────────────────────────────────────────────
verify_usage() {
    cat <<'EOF'
Usage: pilot verify [options]

Collect and display evidence for evaluators. Outputs a human-readable
verification report followed by machine-parseable JSON.

Options:
  --json-only     Output only JSON (no human-readable section)
  --data-dir DIR  Data directory (default: /tmp/pilot-data)
  --help          Show this help
EOF
}

# ──────────────────────────────────────────────────────────────
# Collect all evidence
# ──────────────────────────────────────────────────────────────
collect_evidence() {
    local npk account_id balance history skills card status peers uptime

    npk=$(pilot_call "getAgentNpk()" 2>&1)             || npk="unavailable"
    account_id=$(pilot_call "getAccountId()" 2>&1)      || account_id="unavailable"
    balance=$(pilot_call "walletBalance()" 2>&1)         || balance="unavailable"
    history=$(pilot_call "walletHistory()" 2>&1)         || history="[]"
    skills=$(pilot_call "metaSkills()" 2>&1)             || skills="[]"
    card=$(pilot_call "agentCard()" 2>&1)                || card="unavailable"
    status=$(pilot_call "metaStatus()" 2>&1)             || status="unavailable"
    peers=$(pilot_call "agentDiscover(pilot)" 2>&1)      || peers="[]"
    uptime=$(get_uptime)

    # Owner channel
    local owner_channel
    owner_channel=$(pilot_call "getOwnerChannelId()" 2>&1) || owner_channel=""

    # Export for use in print functions
    _ev_npk="$npk"
    _ev_account_id="$account_id"
    _ev_balance="$balance"
    _ev_history="$history"
    _ev_skills="$skills"
    _ev_card="$card"
    _ev_status="$status"
    _ev_peers="$peers"
    _ev_uptime="$uptime"
    _ev_owner_channel="$owner_channel"
}

# ──────────────────────────────────────────────────────────────
# Parse skills into display format
# ──────────────────────────────────────────────────────────────
render_skills() {
    local skills_raw="$1"
    local registered=0 total=21

    # Expected skill names
    local -a skill_names=(
        "wallet.balance" "wallet.send" "wallet.history"
        "spend.create" "spend.approve" "spend.reject" "spend.limits"
        "storage.upload" "storage.download" "storage.list" "storage.share"
        "messaging.send" "messaging.join" "messaging.group"
        "meta.skills" "meta.status" "meta.configure"
        "agent.card" "agent.discover" "agent.task" "agent.cancel"
    )

    local line=""
    local col=0
    for skill in "${skill_names[@]}"; do
        if [[ "$skills_raw" == *"$skill"* ]]; then
            line+="  ${GREEN}✓${RESET} ${skill}"
            (( registered++ ))
        else
            line+="  ${RED}✗${RESET} ${DIM}${skill}${RESET}"
        fi
        (( col++ ))
        if (( col % 4 == 0 )); then
            printf '%s\n' "$line"
            line=""
        fi
    done
    [[ -n "$line" ]] && printf '%s\n' "$line"

    printf '\n  %s/%s registered\n' "$registered" "$total"
}

# ──────────────────────────────────────────────────────────────
# Human-readable report
# ──────────────────────────────────────────────────────────────
print_human_report() {
    printf "\n"
    hrule "═" 50
    printf "  ${BOLD}Pilot Agent Verification Report${RESET}\n"
    hrule "═" 50
    printf "\n"

    printf "${BOLD}Identity${RESET}\n"
    print_kv "NPK" "$_ev_npk"
    print_kv "Account" "$_ev_account_id"
    print_kv "Balance" "$_ev_balance"
    printf "\n"

    printf "${BOLD}Skills${RESET}\n"
    render_skills "$_ev_skills"
    printf "\n"

    # Agent Card
    if [[ -n "$_ev_card" ]] && [[ "$_ev_card" != "unavailable" ]] && [[ "$_ev_card" != "{}" ]]; then
        printf "  Agent Card:     ${GREEN}Published ✓${RESET}\n"
    else
        printf "  Agent Card:     ${RED}Not published ✗${RESET}\n"
    fi

    # Owner Channel
    if [[ -n "$_ev_owner_channel" ]] && [[ "$_ev_owner_channel" != "unavailable" ]] && [[ "$_ev_owner_channel" != "" ]]; then
        printf "  Owner Channel:  ${GREEN}Connected ✓${RESET}\n"
    else
        printf "  Owner Channel:  ${RED}Not connected ✗${RESET}\n"
    fi

    printf "\n"

    # Peers
    local peer_count=0
    if [[ "$_ev_peers" == "["*"]" ]]; then
        # Count array elements (rough JSON parse)
        peer_count=$(echo "$_ev_peers" | tr ',' '\n' | grep -c '"' 2>/dev/null || echo 0)
        peer_count=$(( peer_count / 2 ))  # rough estimate
    fi
    printf "  Peers Discovered: %s\n" "$peer_count"
    printf "\n"

    printf "  Status: %s\n" "$_ev_status"
    printf "  Uptime: %s\n" "$_ev_uptime"
    printf "\n"
}

# ──────────────────────────────────────────────────────────────
# JSON evidence
# ──────────────────────────────────────────────────────────────
print_json_evidence() {
    # Escape strings for JSON
    json_escape() {
        local s="$1"
        s="${s//\\/\\\\}"
        s="${s//\"/\\\"}"
        s="${s//$'\n'/\\n}"
        s="${s//$'\r'/}"
        s="${s//$'\t'/\\t}"
        printf '%s' "$s"
    }

    local card_published="false"
    if [[ -n "$_ev_card" ]] && [[ "$_ev_card" != "unavailable" ]] && [[ "$_ev_card" != "{}" ]]; then
        card_published="true"
    fi

    local owner_connected="false"
    if [[ -n "$_ev_owner_channel" ]] && [[ "$_ev_owner_channel" != "unavailable" ]] && [[ "$_ev_owner_channel" != "" ]]; then
        owner_connected="true"
    fi

    cat <<ENDJSON
{
  "npk": "$(json_escape "$_ev_npk")",
  "account_id": "$(json_escape "$_ev_account_id")",
  "balance": "$(json_escape "$_ev_balance")",
  "skills": $(json_escape "$_ev_skills"),
  "agent_card_published": ${card_published},
  "owner_channel_connected": ${owner_connected},
  "owner_channel_id": "$(json_escape "$_ev_owner_channel")",
  "status": "$(json_escape "$_ev_status")",
  "uptime": "$(json_escape "$_ev_uptime")",
  "peers": $(json_escape "$_ev_peers"),
  "history": $(json_escape "$_ev_history"),
  "collected_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
ENDJSON
}

# ──────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────
verify_main() {
    local json_only=false

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --json-only)   json_only=true; shift ;;
            --data-dir)    PILOT_DATA_DIR="$2"; shift 2 ;;
            --help)        verify_usage; exit 0 ;;
            *)             log_error "Unknown option: $1"; verify_usage; exit 1 ;;
        esac
    done

    require_cmd logoscore "Install logoscore from the Logos SDK"

    collect_evidence

    if [[ "$json_only" == "true" ]]; then
        print_json_evidence
    else
        print_human_report
        printf "${DIM}--- JSON Evidence ---${RESET}\n"
        print_json_evidence
    fi
}

# Allow sourcing without executing
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    verify_main "$@"
fi
