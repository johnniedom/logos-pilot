#!/bin/bash
# Bring up Agent B as a PERSISTENT container (unlike test-two-agents-docker.sh, which
# runs it with --rm and tears it down at the end) and save its NPK to ~/npk-b.json.
# That NPK is the recipient for the spending-hold demo (use case B) and the A2A payee (C).
set -o pipefail

LOGOSCORE=$(find /nix/store -maxdepth 3 -name logoscore -path "*logoscore-cli-bin*" -type f 2>/dev/null | head -1)
export LOGOS_HOST_PATH=$(find /nix/store -maxdepth 3 -name logos_host -path "*liblogos-bin*" -type f 2>/dev/null | head -1)
MODULES="${PILOT_MODULE_PATH:-$HOME/.pilot/modules}"
MODULES_B="/tmp/pilot-logoscore/modules-b"
AGENT_B="/tmp/agent-b"
CONTAINER="pilot-agent-b"

# Clean any stale container of the same name, then prep B's module set.
docker rm -f $CONTAINER >/dev/null 2>&1
rm -rf $MODULES_B $AGENT_B
mkdir -p $MODULES_B $AGENT_B
for m in capability_module logos_execution_zone delivery_module chat_module pilot; do
  cp -r $MODULES/$m $MODULES_B/$m 2>/dev/null
done

echo "Starting Agent B (persistent)..."
docker run -d \
  --name $CONTAINER \
  -v /nix/store:/nix/store:ro \
  -v $MODULES_B:/modules:ro \
  -v $AGENT_B:/data \
  -v /home/johnnie/.risc0:/root/.risc0:ro \
  -e LOGOS_HOST_PATH=$LOGOS_HOST_PATH \
  -e PILOT_SEQUENCER_ADDR=http://host.docker.internal:3040 \
  -e PILOT_WAKU_ADDR=/ip4/host.docker.internal/tcp/30303 \
  -e RISC0_DEV_MODE=1 \
  -e LOGOS_BLOCKCHAIN_CIRCUITS=/nix/store/y8i3f2qiyhbl9kccvl7z12rnbj6h42g9-logos-blockchain-circuits-0.4.1 \
  -e PATH=/root/.risc0/extensions/v3.0.5-cargo-risczero-x86_64-unknown-linux-gnu:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  --add-host=host.docker.internal:host-gateway \
  ubuntu:22.04 \
  $LOGOSCORE --config-dir /data/.logoscore -D -m /modules \
  > /dev/null 2>&1

sleep 5
if ! docker ps --filter name=$CONTAINER --format "{{.Status}}" | grep -q "Up"; then
  echo "FAIL: Agent B container did not start"; docker logs $CONTAINER 2>&1 | tail -5; exit 1
fi

for m in capability_module logos_execution_zone delivery_module pilot; do
  docker exec $CONTAINER timeout 15 $LOGOSCORE --config-dir /data/.logoscore load-module $m > /dev/null 2>&1
done
sleep 3

docker exec $CONTAINER timeout 30 $LOGOSCORE --config-dir /data/.logoscore call pilot initialize /data > /dev/null 2>&1
sleep 2

RAW=$(docker exec $CONTAINER timeout 30 $LOGOSCORE --config-dir /data/.logoscore call pilot getAgentNpk 2>&1)
NPK=$(echo "$RAW" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" 2>/dev/null)

if [ -z "$NPK" ] || echo "$NPK" | grep -q '"error"'; then
  echo "FAIL: could not get NPK"; echo "  → $RAW"; exit 1
fi

echo "$NPK" > ~/npk-b.json
echo ""
echo "OK — Agent B's NPK saved to ~/npk-b.json:"
cat ~/npk-b.json
echo ""
echo "Container '$CONTAINER' is left RUNNING. To free memory for recording: docker stop $CONTAINER"
