#!/usr/bin/env bash
set -e

BASECAMP_DIR="$HOME/.local/share/Logos/LogosBasecamp"
MODULES_DIR="$BASECAMP_DIR/modules"
PLUGINS_DIR="$BASECAMP_DIR/plugins"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== Installing Pilot Agent into Logos Basecamp ==="

# Install core module
echo "[1/2] Installing core module..."
if command -v lgpm &>/dev/null; then
    lgpm install --file "$SCRIPT_DIR/pilot-module/result/logos-pilot-module-lib.lgx" --modules-dir "$MODULES_DIR" --allow-unsigned 2>/dev/null || true
fi

# Add sqlite if missing
if [ ! -f "$MODULES_DIR/pilot/libsqlite3.so" ]; then
    chmod -R u+w "$MODULES_DIR/pilot/" 2>/dev/null
    cp /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 "$MODULES_DIR/pilot/libsqlite3.so" 2>/dev/null || true
    echo "   Added libsqlite3.so"
fi

# Install UI plugin
echo "[2/2] Installing UI plugin..."
mkdir -p "$PLUGINS_DIR/pilot_ui"
cp "$SCRIPT_DIR/pilot-ui/basecamp-plugin/manifest.json" "$PLUGINS_DIR/pilot_ui/"
cp "$SCRIPT_DIR/pilot-ui/basecamp-plugin/Main.qml" "$PLUGINS_DIR/pilot_ui/"

echo ""
echo "Done! Restart Basecamp to see the Pilot Agent."
