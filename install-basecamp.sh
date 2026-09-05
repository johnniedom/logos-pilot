#!/usr/bin/env bash
set -e

BASECAMP_DIR="$HOME/.local/share/Logos/LogosBasecamp"
MODULES_DIR="$BASECAMP_DIR/modules"
PLUGINS_DIR="$BASECAMP_DIR/plugins"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LGPM=$(find /nix/store -maxdepth 3 -name "lgpm" -path "*/bin/*" 2>/dev/null | head -1)

echo "=== Installing Pilot Agent + Pilot Remote into Logos Basecamp ==="
echo "lgpm: $LGPM"
echo ""

mkdir -p "$MODULES_DIR" "$PLUGINS_DIR/pilot_ui" "$PLUGINS_DIR/pilot_remote"

# Install all modules from nix store LGX files
install_lgx() {
  local name="$1"
  local lgx=$(find /nix/store -maxdepth 2 -name "logos-${name}-module-lib*.lgx" 2>/dev/null | head -1)
  if [ -n "$lgx" ] && [ -n "$LGPM" ]; then
    echo "  $name -> $(basename $lgx)"
    $LGPM install --file "$lgx" --modules-dir "$MODULES_DIR" --allow-unsigned 2>/dev/null || true
  else
    echo "  WARNING: $name LGX not found"
  fi
}

echo "[1/4] Installing modules..."
install_lgx "pilot"
install_lgx "capability_module"
install_lgx "lez_core"
install_lgx "delivery_module"
install_lgx "storage_module"
install_lgx "chat_module"
# The owner-side module Pilot Remote talks to (pilot-owner/module): signs, seals and publishes the
# owner's commands to an agent on ANOTHER machine over Logos Messaging. No agent modules needed.
install_lgx "pilot_owner"
echo ""

# Also install pilot from local build if available
if [ -f "$SCRIPT_DIR/pilot-module/result-lgx/logos-pilot-module-lib.lgx" ] && [ -n "$LGPM" ]; then
    echo "[1b] Installing pilot from local build..."
    $LGPM install --file "$SCRIPT_DIR/pilot-module/result-lgx/logos-pilot-module-lib.lgx" \
        --modules-dir "$MODULES_DIR" --allow-unsigned 2>/dev/null || true
fi
# ... and the owner module from a local build (nix build ./pilot-owner/module#lgx -o pilot-owner/module/result-lgx)
OWNER_LOCAL=$(ls "$SCRIPT_DIR"/pilot-owner/module/result-lgx/*.lgx 2>/dev/null | head -1)
if [ -n "$OWNER_LOCAL" ] && [ -n "$LGPM" ]; then
    echo "[1c] Installing pilot_owner from local build: $(basename "$OWNER_LOCAL")"
    $LGPM install --file "$OWNER_LOCAL" --modules-dir "$MODULES_DIR" --allow-unsigned 2>/dev/null || true
fi

# Fix variant: nix builds use "linux-amd64-dev" but Basecamp expects "linux-amd64"
echo "[2/4] Fixing module variants..."
for mod in "$MODULES_DIR"/*/; do
    if [ -f "$mod/variant" ]; then
        chmod -R u+w "$mod" 2>/dev/null || true
        echo -n "linux-amd64" > "$mod/variant"
        sed -i "s/linux-amd64-dev/linux-amd64/g" "$mod/manifest.json" 2>/dev/null || true
    fi
done
echo "   All variants set to linux-amd64"

# Add sqlite if missing
echo "[3/4] Checking sqlite..."
if [ -d "$MODULES_DIR/pilot" ] && [ ! -f "$MODULES_DIR/pilot/libsqlite3.so" ]; then
    chmod -R u+w "$MODULES_DIR/pilot/" 2>/dev/null || true
    cp /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 "$MODULES_DIR/pilot/libsqlite3.so" 2>/dev/null || true
    echo "   Added libsqlite3.so"
else
    echo "   OK"
fi

# Install the two UI plugins:
#   pilot_ui     — "Pilot Agent": the agent runs INSIDE this Basecamp (declares the pilot module)
#   pilot_remote — "Pilot Remote": the agent runs somewhere else; this plugin only declares
#                  pilot_owner and talks to the agent over Logos Messaging through a relay
echo "[4/4] Installing UI plugins..."
cp "$SCRIPT_DIR/pilot-ui/basecamp-plugin/"*.qml "$PLUGINS_DIR/pilot_ui/"
cp "$SCRIPT_DIR/pilot-ui/basecamp-plugin/manifest.json" "$PLUGINS_DIR/pilot_ui/"
cp "$SCRIPT_DIR/pilot-ui/basecamp-plugin/metadata.json" "$PLUGINS_DIR/pilot_ui/"
cp "$SCRIPT_DIR/pilot-ui/basecamp-plugin/variant" "$PLUGINS_DIR/pilot_ui/"
cp "$SCRIPT_DIR/pilot-ui/remote-plugin/"*.qml "$PLUGINS_DIR/pilot_remote/"
cp "$SCRIPT_DIR/pilot-ui/remote-plugin/manifest.json" "$PLUGINS_DIR/pilot_remote/"
cp "$SCRIPT_DIR/pilot-ui/remote-plugin/metadata.json" "$PLUGINS_DIR/pilot_remote/"
cp "$SCRIPT_DIR/pilot-ui/remote-plugin/variant" "$PLUGINS_DIR/pilot_remote/"

echo ""
echo "=== Installed ==="
echo "Modules:"
ls "$MODULES_DIR/" 2>/dev/null || echo "  (none)"
echo ""
echo "Plugins:"
for p in pilot_ui pilot_remote; do
  echo "  $p: $(ls "$PLUGINS_DIR/$p/"*.qml 2>/dev/null | xargs -I{} basename {} | tr '\n' ' ')"
done
echo ""
echo "Done! Restart Basecamp. 'Pilot Agent' runs an agent in this Basecamp; 'Pilot Remote' talks to"
echo "an agent on another machine (see docs/owner-channel.md and agents/local-owner-demo.sh)."
