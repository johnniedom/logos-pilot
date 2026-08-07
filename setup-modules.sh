#!/bin/bash
set -e

# Persistent per-user location by default so installed modules survive a reboot
# / a /tmp wipe (matches pilot-cli's loadConfig default). Override with PILOT_MODULE_PATH.
MODULES_DIR="${PILOT_MODULE_PATH:-$HOME/.pilot/modules}"
LGPM=$(find /nix/store -maxdepth 3 -name "lgpm" -path "*/bin/*" 2>/dev/null | head -1)

echo "=== Installing Logos modules ==="
echo "lgpm: $LGPM"
echo ""

rm -rf $MODULES_DIR
mkdir -p $MODULES_DIR

# Install from LGX files already in nix store (no network needed)
install_lgx() {
  local name="$1"
  local lgx=$(find /nix/store -maxdepth 2 -name "logos-${name}-module-lib*.lgx" 2>/dev/null | head -1)
  if [ -n "$lgx" ]; then
    echo "  $name -> $lgx"
    $LGPM install --file "$lgx" --modules-dir $MODULES_DIR --allow-unsigned 2>/dev/null
  else
    echo "  WARNING: $name LGX not found in nix store"
  fi
}

echo "--- Installing from nix store cache ---"
install_lgx "pilot"
install_lgx "capability_module"
install_lgx "lez_core"   # renamed from lez_wallet_module; the daemon loads this name
install_lgx "delivery_module"
install_lgx "storage_module"
install_lgx "chat_module"            # no longer a pilot dependency, but rpc.nim still loads it
echo ""

echo "=== Installed modules ==="
ls $MODULES_DIR/
echo ""

# Find logoscore + logos_host
LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f 2>/dev/null | head -1)
if [ -z "$LOGOSCORE" ]; then
  LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli*" -type f 2>/dev/null | head -1)
fi
LOGOS_HOST=$(find /nix/store -maxdepth 1 -name "*-logos-liblogos" -type d 2>/dev/null | head -1)/bin/logos_host

echo "=== Smoke test ==="
export LOGOS_HOST_PATH=$LOGOS_HOST
$LOGOSCORE -m $MODULES_DIR \
  -l capability_module,lez_core,delivery_module,storage_module,pilot \
  -c "pilot.echo(hello_real_modules)" --quit-on-finish
