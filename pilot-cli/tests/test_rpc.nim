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

# A COMPLETE raw keys JSON typed directly passes through untouched.
const FULL_KEYS = "{\"nullifier_public_key\":\"aa\",\"viewing_public_key\":\"bb\"}"
doAssert resolveRecipient(cfg, FULL_KEYS) == (FULL_KEYS, "")

# A partial keys blob still passes through resolveRecipient untouched — the
# wallet, not the CLI, decides what it can spend to. recipientKeysError exists
# only to tell salvageSendLine whether a paste is intact enough to rebuild.
doAssert resolveRecipient(cfg, "{\"nullifier_public_key\":\"aa\"}") ==
  ("{\"nullifier_public_key\":\"aa\"}", "")
doAssert recipientKeysError(FULL_KEYS) == ""
doAssert recipientKeysError("{\"nullifier_public_key\":\"aa\"}") != ""
doAssert recipientKeysError("b3dd66d\",\"viewing_public_key\":\"cc\"}") != ""

# ── brace balance: how the REPL knows a pasted /send is still incomplete ──
doAssert jsonBracesBalanced("/send " & FULL_KEYS & " 20 coffee")
doAssert not jsonBracesBalanced("/send {\"nullifier_public_key\":\"aa\",")
doAssert jsonBracesBalanced("/balance")                 # no braces at all = complete
doAssert not jsonBracesBalanced("{\"a\":{\"b\":1}")     # nested, still open

# ── salvaging a /send whose "/send " head was lost ──
# Only ever salvaged when the keys parse AND carry both fields, so a corrupted
# blob is still refused rather than guessed at.
doAssert salvageSendLine(FULL_KEYS & " 20 coffee") == "/send " & FULL_KEYS & " 20 coffee"
doAssert salvageSendLine(FULL_KEYS & " 20") == "/send " & FULL_KEYS & " 20 (no reason given)"
doAssert salvageSendLine("{\"nullifier_public_key\":\"aa\"} 20 coffee") == ""   # incomplete keys
doAssert salvageSendLine("hello how are you") == ""                             # ordinary chat
doAssert salvageSendLine(FULL_KEYS) == ""                                       # no amount

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

# ── explainRpcFailure: turn RPC_FAILED into something actionable ──
# When the sequencer goes unreachable mid-transfer the upstream wallet unwraps a
# connect error and the whole module process dies. Every later wallet call then
# returns RPC_FAILED, which reads like the send was rejected. It was not: the
# module is simply gone and the daemon needs restarting (seen 2026-07-25).
const RPC_FAIL = """{"code":"RPC_FAILED","message":"callModuleMethod('pilot', 'walletSend') RPC call failed.","status":"error"}"""

let walletDead = explainRpcFailure(RPC_FAIL,
  "[critical] Module process crashed: logos_execution_zone")
doAssert "wallet module" in walletDead
doAssert "nothing was sent" in walletDead.toLowerAscii
doAssert "3040" in walletDead            # points at the usual cause

# A crash of some other module gets the generic restart advice, not a wallet story.
let otherDead = explainRpcFailure(RPC_FAIL, "[critical] Module process crashed: storage_module")
doAssert "storage_module" in otherDead
doAssert "wallet module" notin otherDead

# RPC_FAILED with a clean log = daemon down / busy.
let noCrash = explainRpcFailure(RPC_FAIL, "[info] Module loaded: pilot")
doAssert "not answering" in noCrash

# Anything that is not an RPC failure is left alone.
doAssert explainRpcFailure("""{"status":"completed"}""", "") == ""
doAssert explainRpcFailure("", "") == ""

removeDir(tmp)
echo "test_rpc: all assertions passed"
