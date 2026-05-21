#!/usr/bin/env bash
# pilot-cli/lib/chat.sh — Headless terminal chat with the Pilot agent
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

chat_usage() {
    cat <<'EOF'
Usage: pilot chat [options]

Interactive terminal chat with your Pilot agent.
Type natural language or /commands. The LLM interprets and dispatches skills.

Options:
  --data-dir DIR  Data directory (default: /tmp/pilot-data)
  --help          Show this help

Built-in commands:
  /help           Show available commands
  /balance        Check wallet balance
  /history        Transaction history
  /skills         List available skills
  /status         Agent status
  /send <to> <amount> <reason>
  /upload <path> <label>
  /download <cid> <path>
  /files          List stored files
  /discover       Discover peer agents
  /approve <id>   Approve pending spend
  /reject <id>    Reject pending spend
  /quit           Exit chat
EOF
}

dispatch_command() {
    local cmd="$1"
    local args="${cmd#* }"
    local verb="${cmd%% *}"
    verb="${verb#/}"

    case "$verb" in
        help)
            chat_usage
            ;;
        balance)
            pilot_daemon_call walletBalance
            ;;
        history)
            pilot_daemon_call walletHistory
            ;;
        skills)
            pilot_daemon_call metaSkills
            ;;
        status)
            pilot_daemon_call metaStatus
            ;;
        files)
            pilot_daemon_call storageList
            ;;
        discover)
            pilot_daemon_call agentDiscover
            ;;
        send)
            local recipient amount reason
            read -r recipient amount reason <<< "$args"
            if [[ -z "${recipient:-}" || -z "${amount:-}" ]]; then
                echo "Usage: /send <recipient> <amount> <reason>"
                return
            fi
            pilot_daemon_call walletSend "$recipient" "$amount" "${reason:-transfer}"
            ;;
        upload)
            local path label
            read -r path label <<< "$args"
            if [[ -z "${path:-}" ]]; then
                echo "Usage: /upload <path> <label>"
                return
            fi
            pilot_daemon_call storageUpload "$path" "${label:-file}"
            ;;
        download)
            local cid path
            read -r cid path <<< "$args"
            if [[ -z "${cid:-}" || -z "${path:-}" ]]; then
                echo "Usage: /download <cid> <output_path>"
                return
            fi
            pilot_daemon_call storageDownload "$cid" "$path"
            ;;
        approve)
            if [[ -z "${args:-}" || "$args" == "$cmd" ]]; then
                echo "Usage: /approve <request_id>"
                return
            fi
            pilot_daemon_call approveSpend "$args"
            ;;
        reject)
            if [[ -z "${args:-}" || "$args" == "$cmd" ]]; then
                echo "Usage: /reject <request_id>"
                return
            fi
            pilot_daemon_call rejectSpend "$args"
            ;;
        quit|exit|q)
            echo "Goodbye."
            exit 0
            ;;
        *)
            echo "Unknown command: /${verb}. Type /help for available commands."
            ;;
    esac
}

dispatch_llm_action() {
    local response="$1"

    local action=""
    action=$(echo "$response" | sed -n 's/.*"action"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')

    case "$action" in
        command)
            local raw
            raw=$(echo "$response" | sed -n 's/.*"raw"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            dispatch_command "$raw"
            ;;
        reply)
            local text
            text=$(echo "$response" | sed -n 's/.*"text"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            echo "$text"
            ;;
        balance|wallet.balance)
            pilot_daemon_call walletBalance
            ;;
        history|wallet.history)
            pilot_daemon_call walletHistory
            ;;
        send|wallet.send)
            local recipient amount reason
            recipient=$(echo "$response" | sed -n 's/.*"recipient"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            amount=$(echo "$response" | sed -n 's/.*"amount"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p')
            reason=$(echo "$response" | sed -n 's/.*"reason"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            if [[ -n "$recipient" && -n "$amount" ]]; then
                pilot_daemon_call walletSend "$recipient" "$amount" "${reason:-transfer}"
            else
                echo "Could not parse send parameters from LLM response."
            fi
            ;;
        approve)
            local id
            id=$(echo "$response" | sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            if [[ -n "$id" ]]; then
                pilot_daemon_call approveSpend "$id"
            else
                echo "Could not parse request ID from LLM response."
            fi
            ;;
        reject)
            local id
            id=$(echo "$response" | sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            if [[ -n "$id" ]]; then
                pilot_daemon_call rejectSpend "$id"
            else
                echo "Could not parse request ID from LLM response."
            fi
            ;;
        upload|storage.upload)
            local path label
            path=$(echo "$response" | sed -n 's/.*"path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            label=$(echo "$response" | sed -n 's/.*"label"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            if [[ -n "$path" ]]; then
                pilot_daemon_call storageUpload "$path" "${label:-file}"
            else
                echo "Could not parse upload parameters."
            fi
            ;;
        skills|meta.skills)
            pilot_daemon_call metaSkills
            ;;
        status|meta.status)
            pilot_daemon_call metaStatus
            ;;
        discover|agent.discover)
            pilot_daemon_call agentDiscover
            ;;
        none)
            ;;
        *)
            echo "$response"
            ;;
    esac
}

chat_main() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --data-dir) PILOT_DATA_DIR="$2"; shift 2 ;;
            --help)     chat_usage; exit 0 ;;
            *)          log_error "Unknown option: $1"; chat_usage; exit 1 ;;
        esac
    done

    require_cmd "$LOGOSCORE" "Build from github:logos-co/logos-logoscore-cli"

    # Start daemon if not already running
    if ! pilot_daemon_running 2>/dev/null; then
        log_info "Starting pilot daemon..."
        if ! pilot_daemon_start; then
            log_error "Failed to start daemon. Check ${PILOT_DATA_DIR}/daemon.log"
            exit 1
        fi
        log_success "Daemon started"
    else
        log_info "Daemon already running"
    fi

    # Cleanup daemon on exit
    trap 'printf "\n"; log_info "Stopping daemon..."; pilot_daemon_stop; exit 0' INT TERM

    # Auto-initialize if not already done
    local npk
    npk=$(pilot_daemon_call getAgentNpk 2>&1) || npk=""
    if [[ -z "$npk" || "$npk" == "{}" ]]; then
        log_info "Initializing agent identity..."
        pilot_daemon_call initialize "${PILOT_DATA_DIR}" >/dev/null 2>&1 || true
        npk=$(pilot_daemon_call getAgentNpk 2>&1) || npk="not initialized"
    fi

    # Get agent info for the header
    local llm_status
    llm_status=$(pilot_daemon_call metaStatus 2>&1) || llm_status="{}"

    local provider model account
    provider=$(echo "$llm_status" | sed -n 's/.*"provider"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p') || true
    model=$(echo "$llm_status" | sed -n 's/.*"model"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p') || true
    account=$(echo "$llm_status" | sed -n 's/.*"account"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p') || true

    printf "\n"
    hrule "═" 50
    printf "  ${BOLD}Pilot Agent — Terminal Chat${RESET}\n"
    if [[ -n "$provider" ]]; then
        printf "  ${DIM}LLM: %s (%s)${RESET}\n" "$provider" "$model"
    else
        printf "  ${DIM}LLM: not configured (command-only mode)${RESET}\n"
    fi
    printf "  ${DIM}Account: %s${RESET}\n" "$(truncate_str "${account:-unknown}" 20)"
    printf "  ${DIM}Type /help for commands, /quit to exit${RESET}\n"
    hrule "═" 50
    printf "\n"

    # Disable errexit for the REPL — daemon calls may fail without crashing the loop
    set +e

    # Main REPL loop
    while true; do
        printf "${BOLD}${GREEN}you${RESET} ${DIM}›${RESET} "
        if ! IFS= read -r input; then
            printf "\n"
            break
        fi

        [[ -z "$input" ]] && continue

        # Direct slash commands — skip LLM
        if [[ "$input" == /* ]]; then
            printf "${BOLD}${BLUE}pilot${RESET} ${DIM}›${RESET} "
            dispatch_command "$input" || true
            printf "\n"
            continue
        fi

        # Send to LLM via processOwnerMessage (daemon call — fast, no module reload)
        printf "${DIM}  thinking...${RESET}\r"
        local response
        response=$(pilot_daemon_call processOwnerMessage "$input" 2>&1) || response=""

        # Clear the "thinking..." line
        printf "\033[2K\r"

        printf "${BOLD}${BLUE}pilot${RESET} ${DIM}›${RESET} "
        if [[ -z "$response" ]]; then
            echo "No response from agent."
        else
            dispatch_llm_action "$response"
        fi
        printf "\n"
    done
}

# Allow sourcing without executing
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    chat_main "$@"
fi
