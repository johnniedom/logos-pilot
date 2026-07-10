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

echo "test_rpc: all assertions passed"
