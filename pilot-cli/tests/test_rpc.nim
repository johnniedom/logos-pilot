# Tests for extractDaemonResult — the seam between the logoscore daemon's reply
# envelope {"result": ..., "status":"ok"} and everything the REPL shows the owner.
#
# Run:  nix develop -c nim c -r tests/test_rpc.nim   (from pilot-cli/)

import ../src/rpc

# Bool-returning module methods (approveSpend, rejectSpend, setSpendingLimits)
# arrive as JSON booleans. These must surface as "true"/"false" — NOT collapse
# to "" (which the REPL misreads as "daemon down" and, on the interactive
# approval prompt, as "Transaction Failed" even when the transfer completed).
doAssert extractDaemonResult(
  """{"method":"approveSpend","module":"pilot","result":true,"status":"ok"}""") == "true"
doAssert extractDaemonResult(
  """{"method":"approveSpend","module":"pilot","result":false,"status":"ok"}""") == "false"
doAssert extractDaemonResult(
  """{"method":"rejectSpend","module":"pilot","result":false,"status":"ok"}""") == "false"

# String results pass through unchanged (echo, metaStatus, ...).
doAssert extractDaemonResult(
  """{"method":"echo","module":"pilot","result":"echo: hello","status":"ok"}""") == "echo: hello"

# String results that themselves contain JSON (walletSend's held response)
# keep working — the REPL parses these for status/request_id.
doAssert extractDaemonResult(
  """{"result":"{\"status\":\"held\",\"request_id\":\"abc123\"}","status":"ok"}""") ==
  """{"status":"held","request_id":"abc123"}"""

# Non-envelope text falls through stripped.
doAssert extractDaemonResult("  plain text  ") == "plain text"

# ── resolveRecipient: the one seam between "@b" and the wallet ──
import std/[os, strutils]

let tmp = getTempDir() / "pilot-test-contacts"
removeDir(tmp)
createDir(tmp / "contacts")
var cfg = Config(dataDir: tmp)

# Non-@ strings pass through untouched (raw keys JSON typed directly).
doAssert resolveRecipient(cfg, "{\"nullifier_public_key\":\"aa\"}") ==
  ("{\"nullifier_public_key\":\"aa\"}", "")

# @name resolves <dataDir>/contacts/<name>.json with ALL whitespace stripped —
# a trailing newline inside the file once reached the wallet verbatim.
writeFile(tmp / "contacts" / "b.json",
  "{\"nullifier_public_key\": \"aa\",\n \"viewing_public_key\": \"bb\"}\n")
doAssert resolveRecipient(cfg, "@b") ==
  ("{\"nullifier_public_key\":\"aa\",\"viewing_public_key\":\"bb\"}", "")

# Unknown contact -> error naming what was looked for, no keys.
let (noKeys, noErr) = resolveRecipient(cfg, "@nobody")
doAssert noKeys == ""
doAssert "nobody" in noErr

# Empty contact file -> error, no keys.
writeFile(tmp / "contacts" / "empty.json", "  \n ")
let (emptyKeys, emptyErr) = resolveRecipient(cfg, "@empty")
doAssert emptyKeys == ""
doAssert "empty" in emptyErr

removeDir(tmp)
echo "test_rpc: all assertions passed"
