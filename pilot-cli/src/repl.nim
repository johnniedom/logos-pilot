import strutils, json, rdstdin, os, osproc, times, streams, terminal
import rpc, daemon, format, selector

var gCfg: Config

proc daemonCallWithSpinner(cfg: Config, meth: string, args: seq[string] = @[], timeoutSec = 10, label = "Thinking"): string =
  var shellArgs = "timeout " & $timeoutSec & " " & quoteShell(cfg.logoscore) &
    " --config-dir " & quoteShell(cfg.configDir) & " call pilot " & meth
  for a in args:
    shellArgs &= " " & quoteShell(a)
  let p = startProcess("bash", args = @["-c", shellArgs],
                       options = {poUsePath, poStdErrToStdOut})
  var frame = 0
  while p.running:
    spinTick(label, frame)
    inc frame
    sleep(120)
  clearLine()
  let raw = p.outputStream.readAll()
  let exitCode = p.waitForExit()
  p.close()
  return extractDaemonResult(raw)

proc formatJson(raw: string): string =
  try:
    let j = parseJson(raw)
    if j.kind != JObject: return raw

    # Errors
    if j.hasKey("error"):
      return RED & "  Error: " & RESET & j["error"].getStr()

    # Skills list
    if j.hasKey("skills") and j.hasKey("count"):
      var lines: seq[string]
      lines.add(BOLD & "  Skills (" & $j["count"].getInt() & ")" & RESET)
      lines.add("")
      var lastCat = ""
      for s in j["skills"]:
        let cat = s["category"].getStr()
        if cat != lastCat:
          lines.add("  " & CYAN & BOLD & cat.toUpperAscii() & RESET)
          lastCat = cat
        let price = s["price_lez"].getInt()
        let priceStr = if price > 0: DIM & " (" & $price & " LEZ)" & RESET else: ""
        lines.add("    " & s["name"].getStr() & priceStr)
        lines.add("    " & DIM & s["description"].getStr() & RESET)
      return lines.join("\n")

    # Balance
    if j.hasKey("balance") and j.hasKey("account"):
      let bal = j["balance"].getStr("")
      let balStr = if bal == "": "0" else: bal
      return BOLD & "  Agent Wallet" & RESET &
             "\n  " & DIM & "Balance      " & RESET & BOLD & balStr & " LEZ" & RESET &
             "\n  " & DIM & "Per-tx limit " & RESET & "100 LEZ" &
             "\n  " & DIM & "Period limit " & RESET & "500 LEZ / 24h" &
             "\n" &
             "\n  " & DIM & "Fund this agent → " & RESET & j["account"].getStr()

    # Status
    if j.hasKey("initialized") and j.hasKey("npk"):
      var lines: seq[string]
      lines.add(BOLD & "  Agent Status" & RESET)
      lines.add("  " & DIM & "Initialized    " & RESET & (if j["initialized"].getBool(): GREEN & "yes" & RESET else: RED & "no" & RESET))
      lines.add("  " & DIM & "Agent Account  " & RESET & truncStr(j["account"].getStr(), 24))
      lines.add("  " & DIM & "Owner          " & RESET & j.getOrDefault("owner_name").getStr("—"))
      if j["llm"].kind == JObject:
        lines.add("  " & DIM & "LLM            " & RESET & j["llm"]["provider"].getStr() & " / " & j["llm"]["model"].getStr())
      else:
        lines.add("  " & DIM & "LLM            " & RESET & "none")
      return lines.join("\n")

    # Pending spends — show with interactive marker
    if j.hasKey("pending") and j["pending"].kind == JArray:
      if j["pending"].len == 0:
        return "  No pending spend requests"
      var lines: seq[string]
      lines.add(BOLD & "  Pending Spends (" & $j["pending"].len & ")" & RESET)
      for s in j["pending"]:
        lines.add("")
        lines.add("  " & YELLOW & BOLD & $s["amount"].getInt() & " LEZ" & RESET & " → " & truncStr(s["recipient"].getStr(""), 20))
        if s["reason"].getStr("") != "":
          lines.add("  " & DIM & s["reason"].getStr("") & RESET)
        lines.add("  " & DIM & "ID: " & s["id"].getStr("?") & RESET)
      lines.add("")
      lines.add("  " & GREEN & "/approve <id>" & RESET & "  or  " & RED & "/reject <id>" & RESET)
      return lines.join("\n")

    # Transaction history
    if j.hasKey("history") and j["history"].kind == JArray:
      if j["history"].len == 0:
        return "  No transactions yet"
      var lines: seq[string]
      lines.add(BOLD & "  Transaction History" & RESET)
      for tx in j["history"]:
        lines.add("  " & tx["type"].getStr("?") & "  " & $tx["amount"].getInt() & " LEZ  " & DIM & tx["timestamp"].getStr("") & RESET)
      return lines.join("\n")

    # File list
    if j.hasKey("files") and j["files"].kind == JArray:
      if j["files"].len == 0:
        return "  No stored files"
      var lines: seq[string]
      lines.add(BOLD & "  Stored Files" & RESET)
      for f in j["files"]:
        lines.add("  " & BOLD & f["label"].getStr("?") & RESET)
        lines.add("    " & DIM & "CID  " & RESET & f["cid"].getStr(""))
      return lines.join("\n")

    # Upload result
    if j.hasKey("cid") and j.hasKey("label") and j.hasKey("encrypted"):
      return GREEN & "  Uploaded" & RESET & "  " & j["label"].getStr() &
             "\n  " & DIM & "CID  " & RESET & j["cid"].getStr() &
             "\n  " & DIM & "Encrypted  " & RESET & "yes"

    # Download result
    if j.hasKey("path") and j.hasKey("decrypted"):
      return GREEN & "  Downloaded" & RESET & "  " & j["path"].getStr() &
             "\n  " & DIM & "Decrypted  " & RESET & "yes"

    # Download in progress
    if j.hasKey("status") and j["status"].getStr() == "downloading":
      return YELLOW & "  Downloading..." & RESET & "  " & j["cid"].getStr("") &
             "\n  " & DIM & "File will be available when download completes" & RESET

    # Share result
    if j.hasKey("shared") and j.hasKey("recipient"):
      return GREEN & "  Shared" & RESET & "  " & j["cid"].getStr() &
             "\n  " & DIM & "Recipient  " & RESET & truncStr(j["recipient"].getStr(), 24)

    # Send result
    if j.hasKey("sent") and j.hasKey("recipient"):
      return GREEN & "  Message sent" & RESET &
             "\n  " & DIM & "To  " & RESET & truncStr(j["recipient"].getStr(), 24) &
             "\n  " & DIM & "Encrypted  " & RESET & "yes"

    # Wallet send result
    if j.hasKey("tx_hash"):
      return GREEN & "  Transfer sent" & RESET &
             "\n  " & DIM & "TX  " & RESET & truncStr(j["tx_hash"].getStr(), 24) &
             "\n  " & DIM & "Amount  " & RESET & $j["amount"].getInt(0) & " LEZ"

    # Spend request held — needs approval (interactive prompt handled in REPL loop)
    if j.hasKey("request_id") and j.hasKey("status") and j["status"].getStr() == "held":
      return "\x01APPROVE:" & j["request_id"].getStr() & ":" & j.getOrDefault("message").getStr("")

    # Spend request created (generic)
    if j.hasKey("request_id") and j.hasKey("state"):
      return YELLOW & "  Spend request created" & RESET &
             "\n  " & DIM & "ID  " & RESET & j["request_id"].getStr() &
             "\n  " & DIM & "State  " & RESET & j["state"].getStr()

  except: discard
  return raw

const HELP_TEXT = """
  """ & BOLD & "Commands" & RESET & """
  /balance                     Check wallet balance
  /history                     Transaction history
  /send <to> <amt> <reason>    Send LEZ tokens
  /approve <id>                Approve pending spend
  /reject <id>                 Reject pending spend
  /upload <path> <label>       Upload a file
  /download <cid> <path>       Download a file
  /files                       List stored files
  /skills                      List available skills
  /status                      Agent status
  /discover [topic]            Discover peer agents
  /help                        Show this help
  /quit                        Exit
"""

proc dispatchSlash(cfg: Config, input: string): string =
  let parts = input.strip().split(maxsplit = 3)
  if parts.len == 0: return ""
  let cmd = parts[0].toLowerAscii()

  case cmd
  of "/help":
    return HELP_TEXT
  of "/balance":
    return formatJson(daemonCall(cfg, "walletBalance"))
  of "/history":
    return formatJson(daemonCall(cfg, "walletHistory"))
  of "/skills":
    return formatJson(daemonCall(cfg, "metaSkills"))
  of "/status":
    return formatJson(daemonCall(cfg, "metaStatus"))
  of "/files":
    return formatJson(daemonCall(cfg, "storageList"))
  of "/pending":
    return formatJson(daemonCall(cfg, "getPendingSpends"))
  of "/discover":
    let topic = if parts.len > 1: parts[1] else: "pilot"
    return formatJson(daemonCallWithSpinner(cfg, "agentDiscover", @[topic], timeoutSec = 20, label = "Discovering agents"))
  of "/send":
    if parts.len < 4:
      return RED & "  usage: /send <recipient> <amount> <reason>" & RESET
    return formatJson(daemonCall(cfg, "walletSend", @[parts[1], parts[2], parts[3]]))
  of "/upload":
    if parts.len < 3:
      return RED & "  usage: /upload <path> <label>" & RESET
    return formatJson(daemonCallWithSpinner(cfg, "storageUpload",
      @[parts[1], parts[2]], timeoutSec = 45, label = "Uploading"))
  of "/download":
    if parts.len < 3:
      return RED & "  usage: /download <cid-or-label> <path>" & RESET
    var cid = parts[1]
    if not cid.startsWith("z"):
      let filesRaw = daemonCall(cfg, "storageList")
      try:
        let fj = parseJson(filesRaw)
        for f in fj["files"]:
          if f["label"].getStr() == cid:
            cid = f["cid"].getStr()
            break
      except: discard
    return formatJson(daemonCallWithSpinner(cfg, "storageDownload", @[cid, parts[2]], timeoutSec = 45, label = "Downloading"))
  of "/approve":
    if parts.len < 2:
      return RED & "  usage: /approve <id>" & RESET
    return daemonCall(cfg, "approveSpend", @[parts[1]])
  of "/reject":
    if parts.len < 2:
      return RED & "  usage: /reject <id>" & RESET
    return daemonCall(cfg, "rejectSpend", @[parts[1]])
  of "/quit", "/exit", "/q":
    return "\x00QUIT"
  else:
    return YELLOW & "  unknown command: " & cmd & DIM & " — type /help" & RESET

proc dispatchAction(cfg: Config, action: JsonNode): string =
  let act = action.getOrDefault("action").getStr("none")
  case act
  of "command":
    let raw = action{"params", "raw"}.getStr("")
    if raw != "":
      return dispatchSlash(cfg, raw)
  of "reply":
    return action{"params", "text"}.getStr("")
  of "balance":
    return formatJson(daemonCall(cfg, "walletBalance"))
  of "send":
    let p = action.getOrDefault("params")
    if not p.isNil and p.kind == JObject:
      return daemonCall(cfg, "walletSend",
        @[p.getOrDefault("recipient").getStr(""),
          $p.getOrDefault("amount").getInt(0),
          p.getOrDefault("reason").getStr("")])
  of "approve":
    let id = action{"params", "id"}.getStr(action.getOrDefault("id").getStr(""))
    return daemonCall(cfg, "approveSpend", @[id])
  of "upload":
    let p = action.getOrDefault("params")
    if not p.isNil:
      # Same 45s window + spinner as the /upload slash path: a large file's
      # chunk transfer + finalize can exceed the default 10s daemonCall and
      # time out silently (no "Uploading..." feedback, no reply).
      return daemonCallWithSpinner(cfg, "storageUpload",
        @[p.getOrDefault("path").getStr(""),
          p.getOrDefault("label").getStr("")],
        timeoutSec = 45, label = "Uploading")
  of "files":
    return formatJson(daemonCall(cfg, "storageList"))
  of "history":
    return formatJson(daemonCall(cfg, "walletHistory"))
  of "download":
    let p = action.getOrDefault("params")
    if not p.isNil and p.kind == JObject:
      var cid = p.getOrDefault("cid").getStr(p.getOrDefault("label").getStr(""))
      if cid != "" and not cid.startsWith("z"):
        let filesRaw = daemonCall(cfg, "storageList")
        try:
          let fj = parseJson(filesRaw)
          for f in fj["files"]:
            if f["label"].getStr() == cid:
              cid = f["cid"].getStr()
              break
        except: discard
      # Match the /download slash path's 45s window: a network fetch can poll up to
      # ~30s inside storageDownload, so the default 10s daemonCall would time out and
      # the agent would return nothing ("Thinking..." then silence). Use the spinner
      # call with the same 45s timeout so conversational downloads behave like manual ones.
      return formatJson(daemonCallWithSpinner(cfg, "storageDownload",
        @[cid, p.getOrDefault("path").getStr("/tmp/download")],
        timeoutSec = 45, label = "Downloading"))
  of "reject":
    let id = action{"params", "id"}.getStr("")
    if id != "": return daemonCall(cfg, "rejectSpend", @[id])
  of "pending":
    return formatJson(daemonCall(cfg, "getPendingSpends"))
  of "skills":
    return formatJson(daemonCall(cfg, "metaSkills"))
  of "status":
    return formatJson(daemonCall(cfg, "metaStatus"))
  of "discover":
    return formatJson(daemonCall(cfg, "agentDiscover", @["pilot"]))
  of "none":
    return ""
  else:
    let text = action{"params", "text"}.getStr("")
    if text != "": return text
    return $action

proc cleanup() {.noconv.} =
  echo ""
  info("Shutting down...")
  stopDaemon(gCfg)
  quit(0)

proc runRepl*(cfg: Config, dataDir: string) =
  gCfg = cfg
  if dataDir != "":
    gCfg.dataDir = dataDir
    gCfg.configDir = dataDir / ".logoscore"

  header("Pilot Chat")
  spinner("Starting daemon")

  if not startDaemon(gCfg):
    clearLine()
    fail("Failed to start daemon")
    return

  clearLine()
  recordStartTime(gCfg)

  let accountId = daemonCall(gCfg, "getAccountId")

  var ownerName = ""
  let statusRaw = daemonCall(gCfg, "metaStatus")
  try:
    let sj = parseJson(statusRaw)
    ownerName = sj.getOrDefault("owner_name").getStr("")
  except: discard

  if ownerName == "":
    stdout.write(DIM & "  Your name: " & RESET)
    stdout.flushFile()
    var ownerInput = stdin.readLine()
    ownerName = ownerInput.strip()
    if ownerName != "":
      discard daemonCall(gCfg, "metaConfigure", @["owner.name", ownerName])

  ok("Agent online")
  if accountId != "":
    kv("Agent Account", truncStr(accountId, 24))
  if ownerName != "":
    echo DIM & "  Owner: " & RESET & BOLD & ownerName & RESET

  # Show pending approvals on startup
  var pendingCount = 0
  try:
    let pendRaw = daemonCall(gCfg, "getPendingSpends")
    let pj = parseJson(pendRaw)
    if pj.hasKey("pending") and pj["pending"].kind == JArray:
      pendingCount = pj["pending"].len
  except: discard
  if pendingCount > 0:
    echo YELLOW & BOLD & "  " & $pendingCount & " pending approval(s)" & RESET & DIM & " — type /pending to review" & RESET

  echo DIM & "  Type /help for commands, or just chat." & RESET
  blankLine()

  setControlCHook(cleanup)

  while true:
    var line: string
    try:
      let prompt = "> "
      let readOk = readLineFromStdin(prompt, line)
      if not readOk:
        cleanup()
        return
    except IOError:
      cleanup()
      return

    line = line.strip()
    if line == "": continue

    var response = ""

    if line.startsWith("/"):
      response = dispatchSlash(gCfg, line)
    else:
      let raw = daemonCallWithSpinner(gCfg, "processOwnerMessage", @[line], timeoutSec = 30)
      var wasAction = false
      try:
        let j = parseJson(raw)
        let act = j.getOrDefault("action").getStr("reply")
        if act != "reply" and act != "none":
          wasAction = true
        response = dispatchAction(gCfg, j)
      except JsonParsingError:
        response = raw
      except:
        response = RED & "  error: " & getCurrentExceptionMsg() & RESET

      if wasAction and response != "":
        let feedback = daemonCall(gCfg, "processOwnerMessage",
          @["[System: the action you dispatched returned this result] " & response], timeoutSec = 15)
        try:
          let fj = parseJson(feedback)
          let text = fj{"params", "text"}.getStr("")
          if text != "":
            response = text
        except: discard

    if response == "\x00QUIT":
      cleanup()
      return

    # Interactive approval prompt
    if response.startsWith("\x01APPROVE:"):
      let parts = response[9..^1].split(":", maxsplit = 1)
      let reqId = parts[0]
      let msg = if parts.len > 1: parts[1] else: ""
      echo ""
      echo YELLOW & BOLD & "  Approval Required" & RESET
      if msg != "":
        echo "  " & DIM & msg & RESET
      echo ""
      let choice = arrowSelect("", @[
        GREEN & "Approve" & RESET & " — execute this transaction",
        RED & "Reject" & RESET & " — cancel and refund",
        DIM & "Skip" & RESET & " — decide later (/pending)"
      ])
      if choice == 0:
        let approveResult = daemonCall(gCfg, "approveSpend", @[reqId])
        response = GREEN & BOLD & "  Transaction Approved" & RESET &
                   "\n  " & DIM & "ID      " & RESET & reqId &
                   "\n  " & DIM & "Status  " & RESET & GREEN & "executed" & RESET
      elif choice == 1:
        discard daemonCall(gCfg, "rejectSpend", @[reqId])
        response = RED & BOLD & "  Transaction Rejected" & RESET &
                   "\n  " & DIM & "ID      " & RESET & reqId &
                   "\n  " & DIM & "Status  " & RESET & "cancelled — no tokens moved"
      else:
        response = DIM & "  Skipped — type /pending to review later" & RESET

    if response != "":
      echo CYAN & "  pilot " & RESET & DIM & "│ " & RESET & response.replace("\n", "\n        " & DIM & "│ " & RESET)
