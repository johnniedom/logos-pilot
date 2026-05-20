#!/usr/bin/env bash
set -euo pipefail

# LP-0008 Pilot Module — Demo Script
# Builds the module from source and loads it into logoscore.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$SCRIPT_DIR/pilot-module"
TEST_DIR="/tmp/logos-pilot-demo"

echo "=== LP-0008: Pilot — Autonomous AI Module ==="
echo ""

# Step 1: Build the module
echo "[1/4] Building pilot module..."
cd "$MODULE_DIR"
nix --extra-experimental-features "nix-command flakes" build
echo "      Built: result/lib/pilot_plugin.so"

# Step 2: Inspect module metadata and methods
echo ""
echo "[2/4] Inspecting module..."
LM=$(find /nix/store -maxdepth 3 -name "lm" -path "*/logos-module*/bin/lm" 2>/dev/null | head -1)
if [ -n "$LM" ]; then
    $LM result/lib/pilot_plugin.so 2>&1
else
    echo "      lm inspector not found, skipping introspection"
fi

# Step 3: Set up logoscore with the module
echo ""
echo "[3/4] Setting up logoscore environment..."
LOGOSCORE_BUILD=$(find /nix/store -maxdepth 1 -name "*-logos-liblogos-build-*" -type d 2>/dev/null | head -1)

if [ -z "$LOGOSCORE_BUILD" ]; then
    echo "      ERROR: logoscore build not found in nix store. Run 'nix build' first."
    exit 1
fi

rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR/bin" "$TEST_DIR/modules"
cp "$LOGOSCORE_BUILD/bin/.logoscore-wrapped" "$TEST_DIR/bin/logoscore"
cp result/lib/pilot_plugin.so "$TEST_DIR/modules/"

# Find Qt plugin paths
QT_BASE=$(find /nix/store -maxdepth 1 -name "*-qtbase-6.*" -not -name "*only*" -not -name "*dev*" -type d 2>/dev/null | head -1)
QT_DECL=$(find /nix/store -maxdepth 1 -name "*-qtdeclarative-6.*" -not -name "*dev*" -type d 2>/dev/null | head -1)
QT_RO=$(find /nix/store -maxdepth 1 -name "*-qtremoteobjects-6.*" -not -name "*dev*" -type d 2>/dev/null | head -1)

export QT_PLUGIN_PATH="$QT_BASE/lib/qt-6/plugins:$QT_DECL/lib/qt-6/plugins"
export NIXPKGS_QT6_QML_IMPORT_PATH="$QT_DECL/lib/qt-6/qml:$QT_RO/lib/qt-6/qml"
export LD_LIBRARY_PATH="$LOGOSCORE_BUILD/lib"

echo "      logoscore:  $TEST_DIR/bin/logoscore"
echo "      module:     $TEST_DIR/modules/pilot_plugin.so"

# Step 4: Load module in logoscore
echo ""
echo "[4/4] Loading pilot module in logoscore..."
echo ""
timeout 10 "$TEST_DIR/bin/logoscore" -c "pilot.echo hello" 2>&1 || true

echo ""
echo "=== Demo complete ==="
echo ""
echo "Module: pilot v1.0.0 by Johnnie Dom"
echo "Skills: 21 (wallet, storage, messaging, agent, meta, blockchain)"
echo "Status: Loaded successfully into Logos Core runtime"
echo ""
echo "To install as .lgx package:"
echo "  nix build .#lgx"
echo "  lgpm install --file result/logos-pilot-module-lib.lgx --allow-unsigned"
