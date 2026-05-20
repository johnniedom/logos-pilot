#!/usr/bin/env bash
# pilot-cli/lib/deploy.sh — 5-step deployment wizard for Pilot agent
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"
# shellcheck source=arrow-select.sh
source "${SCRIPT_DIR}/arrow-select.sh"

# ──────────────────────────────────────────────────────────────
# Usage
# ──────────────────────────────────────────────────────────────
deploy_usage() {
    cat <<'EOF'
Usage: pilot deploy [options]

Deploy a Pilot agent to the LEZ network.

Options:
  --testnet       Deploy to LEZ testnet (default)
  --mainnet       Deploy to LEZ mainnet
  --data-dir DIR  Data directory (default: /tmp/pilot-data)
  --help          Show this help

The deploy wizard walks through 5 steps:
  1. Agent identity generation
  2. LLM provider selection
  3. Owner identity binding
  4. Funding verification
  5. Deployment & agent card publication
EOF
}

# ──────────────────────────────────────────────────────────────
# Step 1 — Agent Identity
# ──────────────────────────────────────────────────────────────
step_identity() {
    log_step "Step 1/5: Agent Identity"

    local init_result
    init_result=$(pilot_call "initialize(${PILOT_DATA_DIR})" 2>&1) || true

    if [[ "$init_result" == *"true"* ]] || [[ "$init_result" == *"already initialized"* ]]; then
        log_success "Identity loaded from existing data"
    else
        log_info "Generating new agent identity..."
        init_result=$(pilot_call "initialize(${PILOT_DATA_DIR})" 2>&1) || true
    fi

    local npk account_id
    npk=$(pilot_call "getAgentNpk()" 2>&1) || npk="unavailable"
    account_id=$(pilot_call "getAccountId()" 2>&1) || account_id="unavailable"

    printf "\n"
    print_kv "Agent NPK" "$npk"
    print_kv "Account ID" "$account_id"
    printf "\n"

    log_success "Agent identity ready"
}

# ──────────────────────────────────────────────────────────────
# Step 2 — LLM Provider (arrow-key selector)
# ──────────────────────────────────────────────────────────────
step_llm_provider() {
    log_step "Step 2/5: LLM Provider"

    local provider
    provider=$(arrow_select "Select LLM Provider:" \
        "Claude (Anthropic)" \
        "OpenAI / GPT" \
        "Gemini (Google)" \
        "Local (Ollama / LM Studio)" \
        "OpenRouter")

    printf "\n"

    local provider_key="" model="" api_key_var="" api_key="" base_url=""
    case "$provider" in
        "Claude (Anthropic)")
            provider_key="anthropic"
            model="claude-sonnet-4-20250514"
            api_key_var="ANTHROPIC_API_KEY"
            ;;
        "OpenAI / GPT")
            provider_key="openai"
            model="gpt-4o"
            api_key_var="OPENAI_API_KEY"
            ;;
        "Gemini (Google)")
            provider_key="google"
            model="gemini-2.0-flash"
            api_key_var="GOOGLE_API_KEY"
            ;;
        "Local (Ollama / LM Studio)")
            provider_key="local"
            model="llama3"
            ;;
        "OpenRouter")
            provider_key="openrouter"
            model="anthropic/claude-sonnet-4-20250514"
            api_key_var="OPENROUTER_API_KEY"
            ;;
    esac

    # Prompt for API key or base URL
    if [[ "$provider_key" == "local" ]]; then
        local default_url="http://localhost:11434"
        printf "  Enter base URL [%s]: " "$default_url"
        read -r base_url
        base_url="${base_url:-$default_url}"

        printf "  Enter model name [%s]: " "$model"
        read -r user_model
        model="${user_model:-$model}"

        export PILOT_LLM_BASE_URL="$base_url"
    else
        # Check for existing env var
        local existing_key="${!api_key_var:-}"
        if [[ -n "$existing_key" ]]; then
            log_info "Found existing ${api_key_var} in environment"
            printf "  Use existing key? [Y/n]: "
            read -r use_existing
            if [[ "${use_existing,,}" != "n" ]]; then
                api_key="$existing_key"
            fi
        fi

        if [[ -z "$api_key" ]]; then
            printf "  Enter %s: " "$api_key_var"
            read -rs api_key
            printf "\n"
        fi

        if [[ -z "$api_key" ]]; then
            log_error "API key cannot be empty"
            return 1
        fi

        export "${api_key_var}=${api_key}"
    fi

    # Set pilot-level env vars
    export PILOT_LLM_PROVIDER="$provider_key"
    export PILOT_LLM_MODEL="$model"

    # Configure in the module
    pilot_call "metaConfigure(llm.provider, ${provider_key})" &>/dev/null || true
    pilot_call "metaConfigure(llm.model, ${model})" &>/dev/null || true

    printf "\n"
    print_kv "Provider" "$provider"
    print_kv "Model" "$model"
    printf "\n"

    log_success "LLM provider configured"
}

# ──────────────────────────────────────────────────────────────
# Step 3 — Owner Identity
# ──────────────────────────────────────────────────────────────
step_owner_identity() {
    log_step "Step 3/5: Owner Identity"

    printf "  Enter your Logos address (from Basecamp > Settings): "
    read -r owner_address

    if [[ -z "$owner_address" ]]; then
        log_error "Owner address cannot be empty"
        return 1
    fi

    # Store for later use
    export PILOT_OWNER_ADDRESS="$owner_address"

    pilot_call "metaConfigure(owner.address, ${owner_address})" &>/dev/null || true

    printf "\n"
    print_kv "Owner" "$owner_address"
    printf "\n"

    log_success "Owner identity set"
}

# ──────────────────────────────────────────────────────────────
# Step 4 — Funding
# ──────────────────────────────────────────────────────────────
step_funding() {
    log_step "Step 4/5: Funding"

    local account_id
    account_id=$(pilot_call "getAccountId()" 2>&1) || account_id="unknown"

    printf "\n"
    printf "  ${BOLD}Agent LEZ Address:${RESET} %s\n" "$account_id"
    printf "\n"
    printf "  Send LEZ tokens to the address above to fund your agent.\n"
    printf "  The wizard will poll for incoming funds.\n"
    printf "\n"
    printf "  ${DIM}Press Enter to skip funding check...${RESET}\n"
    printf "\n"

    local balance="" poll_count=0 max_polls=60

    while (( poll_count < max_polls )); do
        # Check if user pressed Enter (non-blocking read with timeout)
        if read -t 5 -r 2>/dev/null; then
            log_info "Skipping funding check"
            break
        fi

        balance=$(pilot_call "walletBalance()" 2>&1) || balance="0"

        # Strip non-numeric for comparison
        local numeric_balance
        numeric_balance=$(echo "$balance" | tr -dc '0-9')

        if [[ -n "$numeric_balance" ]] && (( numeric_balance > 0 )); then
            printf "\n"
            log_success "Funds received! Balance: ${balance}"
            break
        fi

        (( poll_count++ ))
        printf "\r  ${DIM}Checking balance... (%d/%d)${RESET}" "$poll_count" "$max_polls"
    done

    if (( poll_count >= max_polls )); then
        log_warn "Funding check timed out. You can fund the agent later."
    fi

    printf "\n"
}

# ──────────────────────────────────────────────────────────────
# Step 5 — Deploy
# ──────────────────────────────────────────────────────────────
step_deploy() {
    log_step "Step 5/5: Deploy"

    log_info "Publishing Agent Card..."
    local card_result
    card_result=$(pilot_call "agentCard()" 2>&1) || true
    if [[ -n "$card_result" ]]; then
        log_success "Agent Card published"
    else
        log_warn "Agent Card publication returned empty (module may not be fully running)"
    fi

    log_info "Establishing owner channel..."
    local channel_result
    channel_result=$(pilot_call "establishOwnerChannel()" 2>&1) || true
    if [[ "$channel_result" == *"true"* ]]; then
        log_success "Owner channel established"
    else
        log_warn "Owner channel setup returned: ${channel_result:-empty}"
    fi

    # Record start time
    record_start_time

    # ── Success summary ──────────────────────────────────────
    local npk account_id balance
    npk=$(pilot_call "getAgentNpk()" 2>&1) || npk="unavailable"
    account_id=$(pilot_call "getAccountId()" 2>&1) || account_id="unavailable"
    balance=$(pilot_call "walletBalance()" 2>&1) || balance="0"

    printf "\n"
    hrule "═" 50
    printf "  ${BOLD}${GREEN}Pilot Agent Deployed Successfully${RESET}\n"
    hrule "═" 50
    printf "\n"
    print_kv "Agent NPK" "$npk"
    print_kv "Account ID" "$account_id"
    print_kv "Balance" "$balance"
    print_kv "LLM Provider" "${PILOT_LLM_PROVIDER:-not set}"
    print_kv "LLM Model" "${PILOT_LLM_MODEL:-not set}"
    print_kv "Owner" "${PILOT_OWNER_ADDRESS:-not set}"
    print_kv "Data Dir" "$PILOT_DATA_DIR"
    printf "\n"
    printf "  ${DIM}Use 'pilot status' to check agent status${RESET}\n"
    printf "  ${DIM}Use 'pilot verify' to generate verification report${RESET}\n"
    printf "\n"
}

# ──────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────
deploy_main() {
    local network="testnet"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --testnet)  network="testnet"; shift ;;
            --mainnet)  network="mainnet"; shift ;;
            --data-dir) PILOT_DATA_DIR="$2"; shift 2 ;;
            --help)     deploy_usage; exit 0 ;;
            *)          log_error "Unknown option: $1"; deploy_usage; exit 1 ;;
        esac
    done

    require_cmd "$LOGOSCORE" "Build from github:logos-co/logos-logoscore-cli"

    printf "\n"
    hrule "═" 50
    printf "  ${BOLD}Pilot Agent Deployment Wizard${RESET}\n"
    printf "  ${DIM}Network: %s${RESET}\n" "$network"
    hrule "═" 50

    step_identity
    step_llm_provider
    step_owner_identity
    step_funding
    step_deploy
}

# Allow sourcing without executing
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    deploy_main "$@"
fi
