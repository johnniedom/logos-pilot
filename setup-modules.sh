#!/bin/bash
set -e

MODULES_DIR=/tmp/pilot-logoscore/modules
LGPM=$(find /nix/store -maxdepth 3 -name "lgpm" -path "*/bin/*" 2>/dev/null | head -1)
LOGOSCORE=/nix/store/510yj8rfcjhdijzfj8yhrk8sy7r6k8f5-logos-logoscore-cli-0.1.0/bin/logoscore
LOGOS_HOST=/nix/store/90xva89s7bhsi0w6l1p1l44xvpwxz53h-logos-liblogos/bin/logos_host

echo "=== Setting up Pilot modules ==="
echo "lgpm: $LGPM"
echo "logoscore: $LOGOSCORE"
echo "logos_host: $LOGOS_HOST"

# Install pilot module via lgpm
mkdir -p $MODULES_DIR
echo "Installing pilot module..."
$LGPM install --file ~/dev/logos/logos-pilot/pilot-module/result-lgx/logos-pilot-module-lib.lgx \
  --modules-dir $MODULES_DIR --allow-unsigned

# Create stub manifests for dependency modules
for mod in lez_wallet_module delivery_module storage_module chat_module capability_module; do
  MOD_DIR="$MODULES_DIR/$mod"
  mkdir -p "$MOD_DIR"

  VARIANT_HASH=$(echo -n "$mod-stub" | sha256sum | cut -c1-64)

  cat > "$MOD_DIR/manifest.json" << EOF
{
  "manifestVersion": "0.2.0",
  "name": "$mod",
  "version": "1.0.0",
  "type": "module",
  "description": "Stub for $mod dependency",
  "hashes": {
    "variant": "$VARIANT_HASH"
  }
}
EOF

  echo "$VARIANT_HASH" > "$MOD_DIR/variant"
  echo "  Created stub: $mod"
done

echo ""
echo "=== Modules installed ==="
ls -la $MODULES_DIR/
echo ""
echo "=== Quick smoke test ==="
export LOGOS_HOST_PATH=$LOGOS_HOST
$LOGOSCORE -m $MODULES_DIR -l lez_wallet_module,delivery_module,storage_module,chat_module,pilot -c "pilot.echo(hello_from_pilot)" --quit-on-finish
