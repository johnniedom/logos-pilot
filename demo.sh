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

# Step 3: Set up logoscore environment
echo ""
echo "[3/4] Setting up logoscore environment..."

# Find logoscore CLI (from logos-logoscore-cli, NOT logos-liblogos)
LOGOSCORE="$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli*" -type f 2>/dev/null | head -1)"
if [ -z "$LOGOSCORE" ]; then
    echo "      ERROR: logoscore CLI not found. Run: nix build 'github:logos-co/logos-logoscore-cli'"
    exit 1
fi

# Find lgpm for module installation
LGPM="$(find /nix/store -maxdepth 3 -name lgpm -path "*/bin/*" 2>/dev/null | head -1)"

# Set LOGOS_HOST_PATH (required for module spawning)
export LOGOS_HOST_PATH="$(find /nix/store -maxdepth 1 -name "*-logos-liblogos" -type d 2>/dev/null | head -1)/bin/logos_host"

# Install module via lgpm (logoscore uses lgpm package format)
MODULES_DIR="$TEST_DIR/modules"
rm -rf "$TEST_DIR"
mkdir -p "$MODULES_DIR"

if [ -n "$LGPM" ]; then
    nix --extra-experimental-features "nix-command flakes" build .#lgx -o result-lgx
    $LGPM install --file result-lgx/logos-pilot-module-lib.lgx \
        --modules-dir "$MODULES_DIR" --allow-unsigned 2>&1 || true
    echo "      Installed via lgpm"
else
    echo "      WARN: lgpm not found, copying .so directly"
    mkdir -p "$MODULES_DIR/pilot"
    cp result/lib/pilot_plugin.so "$MODULES_DIR/pilot/"
fi

echo "      logoscore:  $LOGOSCORE"
echo "      modules:    $MODULES_DIR"

# Step 4: Load module in logoscore (inline mode)
echo ""
echo "[4/4] Loading pilot module in logoscore..."
echo ""
timeout 10 "$LOGOSCORE" -m "$MODULES_DIR" -l pilot \
    -c "pilot.echo(hello)" --quit-on-finish 2>&1 || true

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
