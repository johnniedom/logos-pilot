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

# ── missingModules: deploy installs what setup-modules.sh used to be needed for ──
# A fresh machine has no modules dir; a wiped /tmp has an empty one. Either way deploy
# must know exactly which required modules are absent, in a stable order.
import ../src/modules
import std/[os, strutils, algorithm]

let mdir = getTempDir() / "pilot-test-modules"
removeDir(mdir)
createDir(mdir / "pilot")
createDir(mdir / "lez_core")
doAssert missingModules(mdir, REQUIRED_MODULES) == @["delivery_module", "storage_module"]
createDir(mdir / "delivery_module")
createDir(mdir / "storage_module")
doAssert missingModules(mdir, REQUIRED_MODULES).len == 0
doAssert missingModules(getTempDir() / "pilot-test-no-such-dir", REQUIRED_MODULES).len == 4
removeDir(mdir)

# ── resolveRecipient: the one seam between "@b" and the wallet ──

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

# ── saveContact / listContacts: naming an address once, instead of pasting it ──
# Saving is where validation belongs: a contact written wrong is a wrong recipient
# on every later send, discovered ~40 minutes into a real-proof transfer.

# Raw keys JSON, saved under the given name.
let (savedPath, saveErr) = saveContact(cfg, "alice", FULL_KEYS)
doAssert saveErr == ""
doAssert savedPath == tmp / "contacts" / "alice.json"
doAssert readFile(savedPath) == FULL_KEYS

# Whitespace anywhere in the pasted blob is stripped, as resolveRecipient expects.
doAssert saveContact(cfg, "spaced",
  "{\"nullifier_public_key\": \"aa\",\n \"viewing_public_key\": \"bb\"}\n")[1] == ""
doAssert resolveRecipient(cfg, "@spaced") == (FULL_KEYS, "")

# A file of keys can be handed over instead of the keys themselves.
writeFile(tmp / "npk-b.json", FULL_KEYS & "\n")
doAssert saveContact(cfg, "fromfile", tmp / "npk-b.json")[1] == ""
doAssert resolveRecipient(cfg, "@fromfile") == (FULL_KEYS, "")

# Incomplete keys are refused at save time, and nothing is written.
let (badPath, badErr) = saveContact(cfg, "broken", "{\"nullifier_public_key\":\"aa\"}")
doAssert badErr != ""
doAssert badPath == ""
doAssert not fileExists(tmp / "contacts" / "broken.json")

# Names that would escape the contacts directory or confuse @lookup are refused.
doAssert saveContact(cfg, "../escape", FULL_KEYS)[1] != ""
doAssert saveContact(cfg, "", FULL_KEYS)[1] != ""
doAssert saveContact(cfg, "b b", FULL_KEYS)[1] != ""

# A leading @ IS forgiven — "/contact @b <keys>" is the spelling an owner reaches
# for, having just typed "/send @b". It saves under the bare name.
doAssert saveContact(cfg, "@zed", FULL_KEYS)[0] == tmp / "contacts" / "zed.json"
doAssert resolveRecipient(cfg, "@zed") == (FULL_KEYS, "")
doAssert saveContact(cfg, "b", FULL_KEYS)[1] == ""
doAssert "b" in listContacts(cfg)
doAssert "alice" in listContacts(cfg)
doAssert listContacts(cfg) == listContacts(cfg).sorted

# ── readPeerCard: an Agent Card arrives as a file or as a paste ──
# A card may be handed over as a file or pasted whole. Validate the shape HERE,
# where the owner can still fix it, not three RPC hops later.
let cardTmp = getTempDir() / "pilot-test-card.json"
const GOOD_CARD = """{"name":"Pilot Agent","_logos":{"npk":"abc123","signing_key":"02aa"}}"""
writeFile(cardTmp, GOOD_CARD & "\n")

doAssert readPeerCard(cardTmp) == (GOOD_CARD, "")          # file, trailing newline stripped
doAssert readPeerCard(GOOD_CARD) == (GOOD_CARD, "")        # pasted literally
doAssert readPeerCard("  " & GOOD_CARD & "  ") == (GOOD_CARD, "")   # paste with stray spaces

# Every rejection names the problem AND returns no card — a half-validated card
# must never reach the module.
for bad in ["/no/such/file.json",           # missing file
            "{\"name\":\"x\"}",             # JSON, but no _logos.npk
            "{\"_logos\":{}}",              # _logos present, npk empty
            "not json",                     # neither a path nor JSON
            "{\"a\":1",                     # truncated paste
            "[1,2,3]"]:                     # valid JSON, wrong kind
  let (noCard, cardErr) = readPeerCard(bad)
  doAssert cardErr != "", "expected an error for: " & bad
  doAssert noCard == "", "expected no card for: " & bad

removeFile(cardTmp)

# ── explainRpcFailure: turn RPC_FAILED into something actionable ──
# When the sequencer goes unreachable mid-transfer the upstream wallet unwraps a
# connect error and the whole module process dies. Every later wallet call then
# returns RPC_FAILED, which reads like the send was rejected. It was not: the
# module is simply gone and the daemon needs restarting (seen 2026-07-25).
const RPC_FAIL = """{"code":"RPC_FAILED","message":"callModuleMethod('pilot', 'walletSend') RPC call failed.","status":"error"}"""

let walletDead = explainRpcFailure(RPC_FAIL,
  "[critical] Module process crashed: lez_core")
doAssert "wallet module" in walletDead
doAssert "nothing was sent" in walletDead.toLowerAscii
doAssert "3040" in walletDead            # points at the usual cause
# The module was renamed lez_core in 2026-08; a daemon log from before the rename must
# still read as the wallet story (the CLI matched only the old name until 2026-09-04).
doAssert "wallet module" in explainRpcFailure(RPC_FAIL, "[critical] Module process crashed: logos_execution_zone")

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

# ── syncHint: say when the wallet is replaying the chain ──
# A cold agent replays the chain before it can answer anything — 37s per boot on
# a 3.1 GB chain (2026-07-26). Silence during that is indistinguishable from a
# hang, which is most of why one diagnosis took six hours.
doAssert syncHint(
  "[lez_core] Syncing to block 3269. Blocks to sync: 51").contains("51")
doAssert syncHint("[lez_core] Synced to block 3269 in 1.8s") == ""
doAssert syncHint("") == ""
doAssert syncHint("nothing interesting in this log") == ""

# The LAST word in the log wins: a sync that finished must stop hinting, and a
# fresh one after it must start again.
doAssert syncHint("Blocks to sync: 51\nSynced to block 3269 in 1.8s") == ""
doAssert syncHint("Synced to block 10 in 1s\nBlocks to sync: 7").contains("7")

# A tail read while the file is being appended to can cut a line mid-write. That
# must still hint (the wallet IS syncing) and must never crash on the slice.
doAssert syncHint("Blocks to sync:") != ""

# ── daemonLogTail: the seam that feeds both hints from the real log ──
# Worth testing on disk: if the path or the slice were wrong this would quietly
# return "" forever and every hint would silently never appear.
doAssert daemonLogTail(cfg) == ""                    # no log yet -> no guesses
var manyLines: seq[string]
for i in 1 .. 60: manyLines.add("line " & $i)
manyLines.add("[lez_core] Syncing to block 3269. Blocks to sync: 51")
writeFile(tmp / "daemon.log", manyLines.join("\n"))

doAssert daemonLogTail(cfg, 5).splitLines().len == 5
doAssert "Blocks to sync: 51" in daemonLogTail(cfg, 5)
doAssert "line 1\n" notin daemonLogTail(cfg, 5)      # older lines really are dropped
doAssert syncHint(daemonLogTail(cfg, 20)).contains("51")
doAssert daemonLogTail(cfg, 500).splitLines().len == 61   # asking for more than exists is fine

removeDir(tmp)
echo "test_rpc: all assertions passed"
