import strutils, json, times
import rpc, daemon, format

const SKILL_NAMES = @[
  "wallet.balance", "wallet.send", "wallet.history",
  "storage.upload", "storage.download", "storage.list", "storage.share",
  "messaging.send", "messaging.join", "messaging.create_group",
  "agent.card", "agent.discover", "agent.task", "agent.subscribe", "agent.cancel",
  "program.query", "program.call", "program.deploy",
  "meta.skills", "meta.status", "meta.configure"
]

proc runVerify*(cfg: Config, jsonOnly: bool) =
  if not jsonOnly:
    header("Verification Report")
    blankLine()
    spinner("Collecting data")

  let startedDaemon = not isDaemonRunning(cfg)
  if startedDaemon:
    if not startDaemon(cfg):
      fail("Failed to start daemon")
      return

  let npk = daemonCall(cfg, "getAgentNpk")
  let accountId = daemonCall(cfg, "getAccountId")
  let balance = daemonCall(cfg, "walletBalance")
  let history = daemonCall(cfg, "walletHistory")
  let skills = daemonCall(cfg, "metaSkills")
  let card = daemonCall(cfg, "agentCard")
  let status = daemonCall(cfg, "metaStatus")
  let uptime = getUptime(cfg)

  var evidence = newJObject()
  evidence["timestamp"] = %($now())
  evidence["npk"] = %npk
  evidence["account_id"] = %accountId
  try: evidence["balance"] = parseJson(balance)
  except: evidence["balance"] = %balance
  try: evidence["history"] = parseJson(history)
  except: evidence["history"] = %history
  try: evidence["skills"] = parseJson(skills)
  except: evidence["skills"] = %skills
  try: evidence["agent_card"] = parseJson(card)
  except: evidence["agent_card"] = %card
  try: evidence["status"] = parseJson(status)
  except: evidence["status"] = %status
  evidence["uptime"] = %uptime

  if jsonOnly:
    echo $evidence
    if startedDaemon: stopDaemon(cfg)
    return

  clearLine()

  step("Identity")
  if npk != "":
    kv("NPK", truncStr(npk, 40))
    kv("Account", truncStr(accountId, 40))
    ok("Identity present")
  else:
    fail("No identity")
  blankLine()

  step("Wallet")
  kv("Balance", balance)
  blankLine()

  step("Skills")
  var registeredSkills: seq[string] = @[]
  try:
    let sj = parseJson(skills)
    if sj.hasKey("skills") and sj["skills"].kind == JArray:
      for s in sj["skills"]:
        registeredSkills.add(s.getOrDefault("name").getStr(""))
  except: discard

  let categories = @[
    ("Wallet", @["wallet.balance", "wallet.send", "wallet.history"]),
    ("Storage", @["storage.upload", "storage.download", "storage.list", "storage.share"]),
    ("Messaging", @["messaging.send", "messaging.join", "messaging.create_group"]),
    ("Agent", @["agent.card", "agent.discover", "agent.task", "agent.subscribe", "agent.cancel"]),
    ("Blockchain", @["program.query", "program.call", "program.deploy"]),
    ("Meta", @["meta.skills", "meta.status", "meta.configure"])
  ]

  var totalPass = 0
  for (catName, catSkills) in categories:
    var line = "  " & DIM & alignLeft(catName, 12) & RESET
    for sk in catSkills:
      let present = sk in registeredSkills
      if present: inc totalPass
      let mark = if present: GREEN & "●" & RESET else: DIM & "○" & RESET
      line &= mark & " "
    echo line

  blankLine()
  let color = if totalPass == SKILL_NAMES.len: GREEN else: YELLOW
  echo "  " & color & $totalPass & "/" & $SKILL_NAMES.len & " registered" & RESET
  blankLine()

  step("Agent Card")
  if card.contains("error"):
    warn("Not published")
  else:
    try:
      let cj = parseJson(card)
      kv("Name", cj.getOrDefault("name").getStr("?"))
      kv("Version", cj.getOrDefault("version").getStr("?"))
      kv("Skills", $cj.getOrDefault("skills").len)
      ok("Published")
    except:
      ok("Published")
  blankLine()

  step("Runtime")
  kv("Uptime", uptime)
  try:
    let sj = parseJson(status)
    let init = sj.getOrDefault("initialized").getBool(false)
    kv("Initialized", if init: GREEN & "yes" & RESET else: RED & "no" & RESET)
  except: discard
  blankLine()

  hrule()
  echo GREEN & "✓ " & RESET & "Verification complete"
  echo DIM & "  Run with --json-only for machine-readable output" & RESET
  blankLine()

  if startedDaemon: stopDaemon(cfg)
