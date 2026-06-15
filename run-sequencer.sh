#!/bin/bash
docker run --rm -p 8080:8080 \
  -e SEQUENCER_LISTEN_ADDR=0.0.0.0:8080 \
  -e SEQUENCER_DB_PATH=/data/sequencer.db \
  -e SEQUENCER_SIGNING_KEY_PATH=/data/sequencer.key \
  -e SEQUENCER_INITIAL_BALANCE=1000 \
  -e SEQUENCER_CHANNEL_ID=6d656d636f696e00000000000000000000000000000000000000000000000001 \
  -v sequencer-data:/data \
  --entrypoint /usr/bin/logos-blockchain-demo-sequencer \
  ghcr.io/logos-blockchain/logos-blockchain:devnet
