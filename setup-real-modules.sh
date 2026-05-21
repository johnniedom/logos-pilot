#!/bin/bash
set -e

MODULES_DIR=/tmp/pilot-logoscore/modules
LGPM=$(find /nix/store -maxdepth 3 -name "lgpm" -path "*/bin/*" 2>/dev/null | head -1)
LOGOSCORE=/nix/store/510yj8rfcjhdijzfj8yhrk8sy7r6k8f5-logos-logoscore-cli-0.1.0/bin/logoscore
LOGOS_HOST=/nix/store/90xva89s7bhsi0w6l1p1l44xvpwxz53h-logos-liblogos/bin/logos_host

echo "=== Installing REAL Logos modules ==="
echo "lgpm: $LGPM"
echo ""

# Clean old stubs
rm -rf $MODULES_DIR
mkdir -p $MODULES_DIR

# 1. Install pilot module
echo "--- Installing pilot module ---"
$LGPM install --file ~/dev/logos/logos-pilot/pilot-module/result-lgx/logos-pilot-module-lib.lgx \
  --modules-dir $MODULES_DIR --allow-unsigned
echo ""

# 2. Build and install lez_wallet_module
echo "--- Building lez_wallet_module (LGX) ---"
nix build 'github:logos-blockchain/logos-execution-zone-module#lgx' \
  --extra-experimental-features 'nix-command flakes' -o /tmp/lez-wallet-lgx
echo "Installing lez_wallet_module..."
$LGPM install --file /tmp/lez-wallet-lgx/*.lgx \
  --modules-dir $MODULES_DIR --allow-unsigned
echo ""

# 3. Build and install delivery_module
echo "--- Building delivery_module (LGX) ---"
nix build 'github:logos-co/logos-delivery-module#lgx' \
  --extra-experimental-features 'nix-command flakes' -o /tmp/delivery-lgx
echo "Installing delivery_module..."
$LGPM install --file /tmp/delivery-lgx/*.lgx \
  --modules-dir $MODULES_DIR --allow-unsigned
echo ""

# 4. Build and install storage_module
echo "--- Building storage_module (LGX) ---"
nix build 'github:logos-co/logos-storage-module#lgx' \
  --extra-experimental-features 'nix-command flakes' -o /tmp/storage-lgx
echo "Installing storage_module..."
$LGPM install --file /tmp/storage-lgx/*.lgx \
  --modules-dir $MODULES_DIR --allow-unsigned
echo ""

# 5. Build and install chat_module
echo "--- Building chat_module (LGX) ---"
nix build 'github:logos-co/logos-chat-module#lgx' \
  --extra-experimental-features 'nix-command flakes' -o /tmp/chat-lgx
echo "Installing chat_module..."
$LGPM install --file /tmp/chat-lgx/*.lgx \
  --modules-dir $MODULES_DIR --allow-unsigned
echo ""

# 6. Install real capability_module (from nix store — built by logos-module-builder)
echo "--- Installing capability_module ---"
CAP_LGX=$(find /nix/store -maxdepth 2 -name "logos-capability_module-module-lib.lgx" 2>/dev/null | head -1)
if [ -n "$CAP_LGX" ]; then
  $LGPM install --file "$CAP_LGX" --modules-dir $MODULES_DIR --allow-unsigned
else
  echo "WARNING: capability_module LGX not found in nix store"
fi
echo ""

echo "=== All modules installed ==="
ls -la $MODULES_DIR/
echo ""

echo "=== Smoke test ==="
export LOGOS_HOST_PATH=$LOGOS_HOST
$LOGOSCORE -m $MODULES_DIR \
  -l capability_module,lez_wallet_module,delivery_module,storage_module,chat_module,pilot \
  -c "pilot.echo(hello_real_modules)" --quit-on-finish
