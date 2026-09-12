import rpc, daemon, format

const VALID_KEYS = @[
  "llm.provider", "llm.model",
  "owner.npk",
  "spending.per_transaction_limit", "spending.per_period_limit", "spending.period_seconds"
]

const KEY_ALIASES = @[
  ("owner.address", "owner.npk"),
  ("spend.per_tx", "spending.per_transaction_limit"),
  ("spend.per_period", "spending.per_period_limit"),
  ("spend.period", "spending.period_seconds")
]

proc resolveKey(key: string): string =
  for (alias, canonical) in KEY_ALIASES:
    if key == alias: return canonical
  return key

proc runConfigure*(cfg: Config, key, value: string) =
  let resolved = resolveKey(key)

  if resolved notin VALID_KEYS:
    fail("Unknown key: " & key)
    blankLine()
    echo DIM & "  Valid keys:" & RESET
    for k in VALID_KEYS:
      echo "    " & k
    blankLine()
    echo DIM & "  Aliases:" & RESET
    for (alias, canonical) in KEY_ALIASES:
      echo "    " & alias & " → " & canonical
    return

  let startedDaemon = not isDaemonRunning(cfg)
  if startedDaemon:
    if not startDaemon(cfg):
      fail("Failed to start daemon")
      return

  let reply = daemonCall(cfg, "metaConfigure", @[resolved, configureValueArg(value)])
  if configureAccepted(reply):
    ok(resolved & " → " & value)
  elif reply.strip() == "false":
    fail(resolved & " refused by the agent (not a whole number?)")
  else:
    fail(resolved & " not set: no answer from the agent" &
         (if reply.len > 0: " (" & reply.strip() & ")" else: ""))

  if startedDaemon: stopDaemon(cfg)
