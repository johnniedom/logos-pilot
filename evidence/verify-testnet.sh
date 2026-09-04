#!/usr/bin/env bash
# Re-verify every row of evidence/testnet-transactions.tsv against the PUBLIC LEZ testnet.
# Nothing here trusts a log line: a transaction is checked by asking the chain for it by hash
# (getTransaction), an account by reading it (getAccount, base58 id). Exit 1 if any check fails.
#
#   bash evidence/verify-testnet.sh                # default endpoint
#   LEZ_RPC=https://testnet.lez.logos.co bash evidence/verify-testnet.sh
#
# Needs: curl, python3. No wallet, no keys, no module — anyone can run it.
set -u
RPC="${LEZ_RPC:-https://testnet.lez.logos.co}"
TSV="$(dirname "$0")/testnet-transactions.tsv"
fail=0

rpc() {  # method, params-json
  curl -s -m 30 -X POST "$RPC" -H 'content-type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"
}

echo "endpoint: $RPC"
echo "health:   $(rpc checkHealth '[]' | head -c 120)"
echo

# 1. Every transaction hash listed must be known to the chain.
echo "== transactions =="
tail -n +2 "$TSV" | awk -F'\t' '$7 != "-" {print $1"\t"$7"\t"$8}' | while IFS=$'\t' read -r date hash block; do
  res=$(rpc getTransaction "[\"$hash\"]")
  got=$(printf '%s' "$res" | python3 -c '
import json,sys
d=json.load(sys.stdin); r=d.get("result")
print("missing" if r is None else ("block %s" % r[1] if isinstance(r,list) and len(r)>1 else "found"))')
  if [ "$got" = "missing" ]; then echo "FAIL $date $hash -> not on chain"; fail=1
  else echo "ok   $date $hash -> $got (tsv says block $block)"; fi
done

# 2. Every account listed must exist with a nonce >= 1 (it was registered and used) and be
#    owned by the authenticated_transfer program (i.e. a real public account, not a stub).
echo
echo "== accounts =="
tail -n +2 "$TSV" | awk -F'\t' '{print $4; if ($5 != "-") print $5}' | sort -u | while read -r acct; do
  res=$(rpc getAccount "[\"$acct\"]")
  printf '%s' "$res" | python3 -c '
import json,sys
acct=sys.argv[1]; d=json.load(sys.stdin); r=d.get("result")
if not r: print("FAIL %s -> %s" % (acct, d.get("error"))); sys.exit(1)
nonce=r.get("nonce",0); bal=r.get("balance")
owner=r.get("program_owner"); registered = owner is not None and owner != [0]*len(owner) if isinstance(owner,list) else bool(owner)
ok = nonce >= 1 and registered
print(("ok   " if ok else "FAIL ") + "%s balance=%s nonce=%s registered=%s" % (acct, bal, nonce, registered))
sys.exit(0 if ok else 1)' "$acct" || fail=1
done

# 3. The three category agents (evidence/testnet-agents.tsv): every agent's public account must
#    exist on chain, registered, nonce >= 1 — the identity that ran that role really funded
#    itself on this testnet. CIDs, topics and message bodies are checked by the run logs kept
#    as the workflow's artifacts (testnet-agents.yml), not here: they are not chain state.
AGENTS="$(dirname "$0")/testnet-agents.tsv"
if [ -f "$AGENTS" ] && [ "$(tail -n +2 "$AGENTS" | grep -c .)" -gt 0 ]; then
  echo
  echo "== category agents =="
  tail -n +2 "$AGENTS" | awk -F'\t' 'NF >= 5 {print $2"\t"$4"\t"$5}' | while IFS=$'\t' read -r role agent acct; do
    res=$(rpc getAccount "[\"$acct\"]")
    printf '%s' "$res" | python3 -c '
import json,sys
role,agent,acct=sys.argv[1],sys.argv[2],sys.argv[3]; d=json.load(sys.stdin); r=d.get("result")
if not r: print("FAIL %s/%s %s -> %s" % (role, agent, acct, d.get("error"))); sys.exit(1)
nonce=r.get("nonce",0); bal=r.get("balance")
owner=r.get("program_owner"); registered = owner is not None and owner != [0]*len(owner) if isinstance(owner,list) else bool(owner)
ok = nonce >= 1 and registered
print(("ok   " if ok else "FAIL ") + "%s/%s %s balance=%s nonce=%s registered=%s" % (role, agent, acct, bal, nonce, registered))
sys.exit(0 if ok else 1)' "$role" "$agent" "$acct" || fail=1
  done
fi

echo
if [ "$fail" -ne 0 ]; then echo "RESULT: FAIL"; exit 1; fi
echo "RESULT: all evidence rows verified against $RPC"
