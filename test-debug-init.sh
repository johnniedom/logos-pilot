#!/bin/bash
export LOGOSCORE=/nix/store/510yj8rfcjhdijzfj8yhrk8sy7r6k8f5-logos-logoscore-cli-0.1.0/bin/logoscore
export LOGOS_HOST_PATH=/nix/store/90xva89s7bhsi0w6l1p1l44xvpwxz53h-logos-liblogos/bin/logos_host
MODULES_DIR=/tmp/pilot-logoscore/modules

# Create wallet config before anything runs
mkdir -p /tmp/pilot-data
cat > /tmp/pilot-data/wallet_config.json << 'EOF'
{
    "sequencer_addr": "http://127.0.0.1:8080",
    "seq_poll_timeout": "30s",
    "seq_tx_poll_max_blocks": 15,
    "seq_poll_max_retries": 10,
    "seq_block_poll_max_amount": 100,
    "initial_accounts": []
}
EOF

echo "=== Step 1: Init wallet module first, then pilot.initialize ==="
$LOGOSCORE -m $MODULES_DIR \
  -l lez_wallet_module,delivery_module,storage_module,chat_module,pilot \
  -c "lez_wallet_module.create_new(/tmp/pilot-data/wallet_config.json,/tmp/pilot-data/wallet_storage,pilot_agent)" \
  -c "pilot.initialize(/tmp/pilot-data)" \
  -c "pilot.metaStatus()" \
  --quit-on-finish
