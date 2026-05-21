#!/bin/bash
export LOGOSCORE=/nix/store/510yj8rfcjhdijzfj8yhrk8sy7r6k8f5-logos-logoscore-cli-0.1.0/bin/logoscore
export LOGOS_HOST_PATH=/nix/store/90xva89s7bhsi0w6l1p1l44xvpwxz53h-logos-liblogos/bin/logos_host
MODULES_DIR=/tmp/pilot-logoscore/modules

# Create wallet config pointing to our running sequencer
mkdir -p /tmp/pilot-wallet
cat > /tmp/pilot-wallet/wallet_config.json << 'EOF'
{
    "sequencer_addr": "http://127.0.0.1:8080",
    "seq_poll_timeout": "30s",
    "seq_tx_poll_max_blocks": 15,
    "seq_poll_max_retries": 10,
    "seq_block_poll_max_amount": 100,
    "initial_accounts": []
}
EOF

echo "=== All-in-one: init wallet, create account, list accounts ==="
$LOGOSCORE -m $MODULES_DIR -l lez_wallet_module \
  -c "lez_wallet_module.create_new(/tmp/pilot-wallet/wallet_config.json,/tmp/pilot-wallet/storage,password123)" \
  -c "lez_wallet_module.create_account_private()" \
  -c "lez_wallet_module.list_accounts()" \
  --quit-on-finish
