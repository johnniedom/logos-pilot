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
      # Two shapes reach this branch: walletBalance returns a flat {"balance":"100"},
      # but metaStatus nests it ({"balance":{"account":...,"balance":"100"}}) — getStr
      # on an object yields "" and rendered a phantom "0" while the wallet held funds.
      let bal =
        if j["balance"].kind == JObject: j["balance"].getOrDefault("balance").getStr("")
        else: j["balance"].getStr("")
      let balStr = if bal == "": "0" else: bal
      # Limit lines render ONLY when the response carries real limits (metaStatus does,
      # walletBalance doesn't) — never print fabricated defaults as if they were data.
      var limitLines = ""
      if j.hasKey("limits") and j["limits"].kind == JObject:
        let perTx = j["limits"].getOrDefault("per_tx").getInt(100)
        let perPeriod = j["limits"].getOrDefault("per_period").getInt(500)
        let secs = j["limits"].getOrDefault("period_seconds").getInt(86400)
        let periodHrs = max(1, secs div 3600)
        limitLines = "\n  " & DIM & "Per-tx limit " & RESET & $perTx & " LEZ" &
                     "\n  " & DIM & "Period limit " & RESET & $perPeriod & " LEZ / " & $periodHrs & "h"
      return BOLD & "  Agent Wallet" & RESET &
             "\n  " & DIM & "Balance      " & RESET & BOLD & balStr & " LEZ" & RESET &
             limitLines &
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
  /send <to> <amt> <reason>    Send LEZ tokens (<to> = @contact, @keys-file, or raw keys JSON)
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
  # splitWhitespace collapses runs of spaces: "/approve  <id>" (double space — easy to
  # type, seen in the wild) must not turn parts[1] into "" and silently no-op.
  let parts = input.strip().splitWhitespace(maxsplit = 3)
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
      return RED & "  usage: /send <recipient|@keys-file> <amount> <reason>" & RESET
    # Recipient must be the FULL keys JSON (~200 chars) — but pasting that into the line
    # editor gets mangled (head eaten -> LLM chat; corrupted keys -> KeyNotFoundError at
    # approve). @contact/@file sidestep the paste entirely:  /send @b 20 thanks
    let (recipient, rerr) = resolveRecipient(cfg, parts[1])
    if rerr != "":
      return RED & "  " & rerr & RESET
    # A wallet send is a chain op: seconds in dev, minutes in real-proof mode. The default
    # 10s daemonCall would silently time out (like the old storage bug) and gives no feedback.
    # Spinner + wide window mirror the /upload+/download fix (real-proof also needs the module's
    # transfer RPC timeout raised — see pilot_spending.cpp doPrivateTransfer).
    let sendResp = daemonCallWithSpinner(cfg, "walletSend",
      @[recipient, parts[2], parts[3]], timeoutSec = 3600, label = "Sending")
    if sendResp.strip() == "":
      return RED & "  no response from agent — daemon busy or down (try /status)" & RESET
    return formatJson(sendResp)
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
    # approveSpend executes the held transfer synchronously — same wide window + spinner as /send.
    # Never answer with silence: an empty reply reads as "nothing happened" when the module
    # may be busy, down, or the request already terminal.
    let approveResp = daemonCallWithSpinner(cfg, "approveSpend", @[parts[1]],
      timeoutSec = 3600, label = "Approving")
    if approveResp.strip() == "":
      return RED & "  no response from agent — daemon busy or down (try /status)" & RESET
    # approveSpend returns a bare bool over the wire: true = approved AND the
    # transfer executed; false = not found / already decided / expired / transfer
    # failed (the module can't say which — point at /pending and /balance).
    case approveResp.strip()
    of "true":
      return GREEN & BOLD & "  Transaction Approved" & RESET &
             "\n  " & DIM & "ID      " & RESET & parts[1] &
             "\n  " & DIM & "Status  " & RESET & GREEN & "executed" & RESET
    of "false":
      return RED & BOLD & "  Approve Failed" & RESET &
             "\n  " & DIM & "ID      " & RESET & parts[1] &
             "\n  " & DIM & "Status  " & RESET & RED &
             "not approved — request unknown, already decided, expired, or the transfer failed" &
             RESET & "\n  " & DIM & "Check   " & RESET & "/pending and /balance"
    else:
      return formatJson(approveResp)
  of "/reject":
    if parts.len < 2:
      return RED & "  usage: /reject <id>" & RESET
    # rejectSpend is a bare bool too — before the bool fix this printed nothing at
    # all on success ("" reply), which read as the command being ignored.
    let rejectResp = daemonCallWithSpinner(cfg, "rejectSpend", @[parts[1]],
      timeoutSec = 20, label = "Rejecting")
    case rejectResp.strip()
    of "true":
      return RED & BOLD & "  Transaction Rejected" & RESET &
             "\n  " & DIM & "ID      " & RESET & parts[1] &
             "\n  " & DIM & "Status  " & RESET & "cancelled — no tokens moved"
    of "false":
      return RED & "  reject failed — request unknown or already decided (see /pending)" & RESET
    else:
      return formatJson(rejectResp)
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
      # Resolve @contacts here too: the owner says "send 16 to @b" and the LLM
      # echoes "@b" — previously that reached the wallet literally and failed.
      # This is the ONLY safe recipient form on the LLM path: raw keys JSON
      # retyped by the model arrives corrupted (TX_FAILED a live transfer
      # 2026-07-11, request 8ba72d58508f8cc).
      let (recipient, rerr) = resolveRecipient(cfg, p.getOrDefault("recipient").getStr(""))
      if rerr != "":
        return "send failed — " & rerr
      # Same wide window + spinner as the /send slash path (chain op, not a 10s call).
      return daemonCallWithSpinner(cfg, "walletSend",
        @[recipient,
          $p.getOrDefault("amount").getInt(0),
          p.getOrDefault("reason").getStr("")],
        timeoutSec = 3600, label = "Sending")
  of "approve":
    let id = action{"params", "id"}.getStr(action.getOrDefault("id").getStr(""))
    # Bool wire result — translate for the owner (and the LLM feedback loop, which
    # would otherwise be told the bare word "true"/"false" with no context).
    let r = daemonCallWithSpinner(cfg, "approveSpend", @[id],
      timeoutSec = 3600, label = "Approving")
    case r.strip()
    of "true": return "approved — transaction " & id & " executed"
    of "false": return "approve failed — request " & id &
                       " unknown, already decided, expired, or the transfer failed"
    else: return r
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
    if id != "": return daemonCallWithSpinner(cfg, "rejectSpend", @[id],
      timeoutSec = 20, label = "Rejecting")
  of "pending":
    return formatJson(daemonCall(cfg, "getPendingSpends"))
  of "skills":
    return formatJson(daemonCall(cfg, "metaSkills"))
  of "status":
    return formatJson(daemonCall(cfg, "metaStatus"))
  of "discover":
    # Network discovery (subscribes + waits for cards) — match the /discover slash path's
    # spinner + wider window; bare 10s can time out mid-discovery.
    return formatJson(daemonCallWithSpinner(cfg, "agentDiscover", @["pilot"],
      timeoutSec = 20, label = "Discovering agents"))
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

    # A ~200-char keys paste does not always arrive as one line: the editor
    # flushes it in chunks, so the command lands split mid-JSON and used to be
    # sent half-formed. If the braces are still open, keep reading and glue the
    # chunks back together (no separator — a chunk boundary can fall anywhere).
    if line.startsWith("/") and not jsonBracesBalanced(line):
      var joined = line
      for _ in 0 ..< 20:
        echo DIM & "  …paste looks incomplete, reading the rest (blank line cancels)" & RESET
        var more: string
        if not readLineFromStdin("… ", more): break
        if more.strip() == "":
          joined = ""
          break
        joined &= more.strip()
        if jsonBracesBalanced(joined): break
      if joined == "":
        echo DIM & "  cancelled" & RESET
        continue
      line = joined

    var response = ""

    if line.startsWith("/"):
      response = dispatchSlash(gCfg, line)
    elif salvageSendLine(line) != "":
      # The "/send " head was eaten by the line editor, leaving the keys blob
      # first on the line — which reads as chat. The keys parsed and carry both
      # fields, so this is provably intact: rebuild the command instead of
      # making the owner retype the paste.
      let rebuilt = salvageSendLine(line)
      echo DIM & "  (recovered a /send whose start was lost in the paste)" & RESET
      response = dispatchSlash(gCfg, rebuilt)
    elif line.contains("nullifier_public_key") or line.contains("viewing_public_key"):
      # Raw key material must never travel through the LLM: the model retypes
      # long hex and corrupts it, and a keys blob landing here usually means a
      # long paste lost its "/send" head to the line editor's input flush
      # (both burned a live transfer 2026-07-11 — TX_FAILED on garbage keys).
      # Reached only when the blob is NOT salvageable (truncated / missing a
      # field) — intact pastes are rebuilt above. Name the saved contacts so the
      # owner can retry without touching key material at all.
      var known: seq[string] = @[]
      for kind, path in walkDir(gCfg.dataDir / "contacts"):
        if kind == pcFile and path.endsWith(".json"):
          known.add("@" & path.splitFile().name)
      response = RED & "  those keys arrived incomplete — not sending, and not passing them to the LLM." & RESET &
                 "\n  " & DIM & "Use   " & RESET & "/send @<contact> <amount> <reason>" &
                 (if known.len > 0: "\n  " & DIM & "Saved " & RESET & known.join(", ") else:
                    "\n  " & DIM & "Setup " & RESET & "save the keys once: " & gCfg.dataDir & "/contacts/<name>.json")
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
        # approveSpend runs the transfer synchronously (minutes in real-proof) — wide window
        # + spinner, and report the REAL outcome: it can come back TX_FAILED (e.g. stale notes),
        # so never hardcode "executed".
        let approveResult = daemonCallWithSpinner(gCfg, "approveSpend", @[reqId],
          timeoutSec = 3600, label = "Approving")
        # approveSpend returns a bare bool ("true"/"false") over the wire — true
        # means approved AND executed. (This used to look for {"status":"completed"},
        # a shape the daemon never sends, so every successful approval rendered as
        # "Transaction Failed" while the tokens had in fact moved.)
        var okStatus = approveResult.strip() == "true"
        if not okStatus:
          try: okStatus = parseJson(approveResult){"status"}.getStr("") == "completed"
          except: discard
        let statusLine =
          if okStatus: GREEN & "executed" & RESET
          else: RED & "failed — no tokens moved (check /balance)" & RESET
        response = (if okStatus: GREEN else: RED) & BOLD &
                   (if okStatus: "  Transaction Approved" else: "  Transaction Failed") & RESET &
                   "\n  " & DIM & "ID      " & RESET & reqId &
                   "\n  " & DIM & "Status  " & RESET & statusLine
      elif choice == 1:
        discard daemonCall(gCfg, "rejectSpend", @[reqId])
        response = RED & BOLD & "  Transaction Rejected" & RESET &
                   "\n  " & DIM & "ID      " & RESET & reqId &
                   "\n  " & DIM & "Status  " & RESET & "cancelled — no tokens moved"
      else:
        response = DIM & "  Skipped — type /pending to review later" & RESET

    if response != "":
      echo CYAN & "  pilot " & RESET & DIM & "│ " & RESET & response.replace("\n", "\n        " & DIM & "│ " & RESET)
