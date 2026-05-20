#!/usr/bin/env bash
# pilot-cli/lib/arrow-select.sh — Arrow-key interactive selector widget
# Inquirer/dialoguer-style: navigate with arrow keys, confirm with Enter.
set -euo pipefail

# arrow_select "prompt text" option1 option2 option3 ...
# Prints the selected option to stdout. UI chrome goes to stderr so callers
# can capture the result cleanly:  choice=$(arrow_select "Pick one" a b c)
arrow_select() {
    local prompt="$1"
    shift
    local -a options=("$@")
    local count=${#options[@]}

    if (( count == 0 )); then
        echo ""
        return 1
    fi

    local selected=0

    # ── save terminal state ──────────────────────────────────
    local saved_stty
    saved_stty=$(stty -g)
    # Hide cursor
    printf '\033[?25l' >&2

    # ── cleanup on exit ──────────────────────────────────────
    _arrow_cleanup() {
        stty "$saved_stty" 2>/dev/null || true
        printf '\033[?25h' >&2     # show cursor
    }
    trap _arrow_cleanup EXIT INT TERM

    # ── render function ──────────────────────────────────────
    _render() {
        # Move up to overwrite previous render (skip on first draw)
        if [[ "${__arrow_drawn:-}" == "1" ]]; then
            # Move up count+1 lines (prompt + options)
            printf '\033[%dA' $(( count + 1 )) >&2
        fi
        __arrow_drawn=1

        # Prompt line
        printf '\033[2K\033[0;36m?\033[0m \033[1m%s\033[0m\n' "$prompt" >&2

        local i
        for (( i = 0; i < count; i++ )); do
            printf '\033[2K' >&2   # clear line
            if (( i == selected )); then
                printf '  \033[0;36m›\033[0m \033[1m%s\033[0m\n' "${options[$i]}" >&2
            else
                printf '    \033[2m%s\033[0m\n' "${options[$i]}" >&2
            fi
        done
    }

    # ── input loop ───────────────────────────────────────────
    _render

    while true; do
        # Read one character (raw mode, no echo)
        IFS= read -rsn1 key

        # Check for escape sequence (arrow keys)
        if [[ "$key" == $'\x1b' ]]; then
            IFS= read -rsn1 -t 0.1 k2 || true
            if [[ "$k2" == "[" ]]; then
                IFS= read -rsn1 -t 0.1 k3 || true
                case "$k3" in
                    A)  # Up arrow
                        (( selected > 0 )) && (( selected-- ))
                        ;;
                    B)  # Down arrow
                        (( selected < count - 1 )) && (( selected++ ))
                        ;;
                esac
            fi
            _render
            continue
        fi

        # Enter key (empty read or carriage return)
        if [[ "$key" == "" || "$key" == $'\n' || "$key" == $'\r' ]]; then
            break
        fi

        # j/k vim-style navigation as bonus
        if [[ "$key" == "j" ]]; then
            (( selected < count - 1 )) && (( selected++ ))
            _render
        elif [[ "$key" == "k" ]]; then
            (( selected > 0 )) && (( selected-- ))
            _render
        fi
    done

    # Print final selection summary on stderr
    printf '\033[2K\033[0;36m?\033[0m \033[1m%s\033[0m \033[0;36m%s\033[0m\n' \
        "$prompt" "${options[$selected]}" >&2

    # Clear the option lines
    local i
    for (( i = 0; i < count; i++ )); do
        printf '\033[2K\n' >&2
    done
    # Move cursor back up
    printf '\033[%dA' "$count" >&2

    # Return the selected option to stdout
    printf '%s' "${options[$selected]}"
}
