#!/bin/bash
export LOGOSCORE=/nix/store/510yj8rfcjhdijzfj8yhrk8sy7r6k8f5-logos-logoscore-cli-0.1.0/bin/logoscore
export LOGOS_HOST_PATH=/nix/store/90xva89s7bhsi0w6l1p1l44xvpwxz53h-logos-liblogos/bin/logos_host
mkdir -p /tmp/pilot-data
$LOGOSCORE -m /tmp/pilot-logoscore/modules -l lez_wallet_module,delivery_module,storage_module,chat_module,pilot -c "pilot.initialize(/tmp/pilot-data)" --quit-on-finish
