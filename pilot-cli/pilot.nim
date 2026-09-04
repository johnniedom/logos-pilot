import os, strutils, json, parseopt
import src/[rpc, daemon, format, repl, deploy, discover, verify, configure]

const VERSION = "1.0.0"

const USAGE = BOLD & "pilot" & RESET & " — Logos autonomous agent" & """

""" & DIM & "USAGE" & RESET & """
  pilot <command> [flags]

""" & DIM & "COMMANDS" & RESET & """
  deploy                       Deploy a new agent
  chat                         Interactive chat with your agent
  status                       Agent status overview
  verify                       Full verification report
  discover [topic]             Discover peer agents
  poll                         Pull new peer cards/tasks/replies from the relay store
  contact <name> <keys|file>   Save an address as @name (list them with: contact)
  peer add <card.json>         Learn a peer from its Agent Card (list them with: peer)
  open                         Open your agent for hire — strangers can send it paid work
  close                        Close it again — takes it back off the market
  configure <key> <value>      Set configuration
  help                         Show this help

""" & DIM & "FLAGS" & RESET & """
  --testnet                    Deploy against the public LEZ testnet (sets PILOT_SEQUENCER_ADDR)
  --data-dir <dir>             Data directory for chat
  --json, --json-only          Machine-readable output
  --version, -v                Show version
"""

proc runStatus(cfg: Config, jsonOutput: bool) =
  let startedDaemon = not isDaemonRunning(cfg)
  if startedDaemon:
    spinner("Starting daemon")
    if not startDaemon(cfg):
      clearLine()
      fail("Failed to start daemon")
      return
    clearLine()

  let npk = daemonCall(cfg, "getAgentNpk")
  let accountId = daemonCall(cfg, "getAccountId")
  let balance = daemonCall(cfg, "walletBalance")
  let status = daemonCall(cfg, "metaStatus")
  let uptime = getUptime(cfg)

  if jsonOutput:
    var j = newJObject()
    j["npk"] = %npk
    j["account_id"] = %accountId
    try: j["balance"] = parseJson(balance)
    except: j["balance"] = %balance
    try: j["status"] = parseJson(status)
    except: j["status"] = %status
    j["uptime"] = %uptime
    echo $j
  else:
    let initialized = npk != ""
    let statusBadge = if initialized: badge("ONLINE", GREEN)
                      else: badge("OFFLINE", RED)

    header("Pilot Agent " & statusBadge)
    kv("NPK", if npk != "": truncStr(npk, 40) else: DIM & "not initialized" & RESET)
    kv("Account", if accountId != "": truncStr(accountId, 40) else: DIM & "not initialized" & RESET)
    kv("Uptime", uptime)
    blankLine()

    try:
      let bj = parseJson(balance)
      if bj.hasKey("balance"):
        kv("Balance", bj["balance"].getStr("?") & " LEZ")
      elif bj.hasKey("error"):
        kv("Balance", DIM & bj["error"].getStr() & RESET)
    except:
      kv("Balance", balance)

    try:
      let sj = parseJson(status)
      let llm = sj.getOrDefault("llm")
      if not llm.isNil and llm.kind == JObject:
        kv("LLM", llm["provider"].getStr("?") & "/" & llm["model"].getStr("?"))
      else:
        kv("LLM", DIM & "none" & RESET)
    except: discard

    blankLine()
    if not initialized:
      echo DIM & "  Run " & RESET & BOLD & "pilot deploy" & RESET & DIM & " to get started" & RESET
      blankLine()

  if startedDaemon: stopDaemon(cfg)

# Importing a card is what makes a peer usable at all: the module resolves a peer's
# messaging key, payout account and declared price ONLY from a stored card. It runs
# the same signature and TOFU checks on an imported card as on a broadcast one, so
# this is a shortcut in DELIVERY, not in trust.
proc runPeerAdd(cfg: Config, card: string) =
  let startedDaemon = not isDaemonRunning(cfg)
  if startedDaemon:
    spinner("Starting daemon")
    if not startDaemon(cfg):
      clearLine()
      fail("Failed to start daemon")
      quit(1)
    clearLine()

  let res = daemonCall(cfg, "agentImportCard", @[card], timeoutSec = 30)
  if startedDaemon: stopDaemon(cfg)

  var j: JsonNode
  try:
    j = parseJson(res)
  except:
    fail("the agent gave no usable answer — is it running? (pilot status)")
    echo DIM & "  " & res & RESET
    quit(1)
  if j.hasKey("error"):
    fail(j["error"].getStr())
    quit(1)
  if not j.getOrDefault("imported").getBool(false):
    fail("the card was not stored")
    echo DIM & "  " & res & RESET
    quit(1)

  let sig = j.getOrDefault("signature_status").getStr("?")
  ok("card stored")
  kv("NPK", truncStr(j.getOrDefault("npk").getStr("?"), 40))
  kv("Signature", sig)

  let pricing = j.getOrDefault("pricing")
  if not pricing.isNil and pricing.kind == JObject and pricing.len > 0:
    for skill, price in pricing:
      let lez = if price.kind == JInt: price.getInt() else: price.getFloat().int
      kv(skill, $lez & " LEZ")

  blankLine()
  # "stored" must not read as "usable": the payment path resolves a messaging key
  # and a payout account only from a card whose signature verifies against its
  # pinned identity. An unsigned card lists fine and can never be paid — say so
  # here rather than let it surface as a mystery failure at task time.
  if sig != "valid":
    warn("this card's signature is " & sig & " — the peer is listed but NOT payable")
    echo DIM & "  Ask the peer for its signed card (its agentCard output) and import that." & RESET
  else:
    echo DIM & "  This peer can now be given paid tasks." & RESET
  blankLine()

proc main() =
  let cfg = loadConfig()

  var args = commandLineParams()
  if args.len == 0:
    echo USAGE
    quit(0)

  let subcmd = args[0].toLowerAscii()
  let rest = args[1 .. ^1]

  case subcmd
  of "deploy":
    # --testnet used to change only this label while the wallet kept dialling the local
    # sequencer (the module's default, http://127.0.0.1:3040). It now points the wallet at the
    # public testnet for real, by setting the env the module reads — the daemon `deploy` starts
    # inherits this process's environment. An explicit PILOT_SEQUENCER_ADDR always wins, and the
    # label reports what will actually be dialled rather than a network name.
    const TESTNET_RPC = "https://testnet.lez.logos.co"
    for a in rest:
      if a == "--testnet":
        if getEnv("PILOT_SEQUENCER_ADDR") == "": putEnv("PILOT_SEQUENCER_ADDR", TESTNET_RPC)
        if getEnv("PILOT_CHAIN_WAIT_SECS") == "": putEnv("PILOT_CHAIN_WAIT_SECS", "600")   # ~60 s blocks
      elif a == "--mainnet":
        echo "There is no LEZ mainnet to deploy to; use --testnet or set PILOT_SEQUENCER_ADDR."
        quit(1)
    let network = if getEnv("PILOT_SEQUENCER_ADDR") == "": "local sequencer (http://127.0.0.1:3040)"
                  elif getEnv("PILOT_SEQUENCER_ADDR") == TESTNET_RPC: "public testnet (" & TESTNET_RPC & ")"
                  else: getEnv("PILOT_SEQUENCER_ADDR")
    runDeploy(cfg, network)

  of "chat":
    var dataDir = ""
    var i = 0
    while i < rest.len:
      if rest[i] == "--data-dir" and i + 1 < rest.len:
        dataDir = rest[i + 1]
        i += 2
      else:
        i += 1
    runRepl(cfg, dataDir)

  of "verify":
    let jsonOnly = "--json-only" in rest
    runVerify(cfg, jsonOnly)

  of "discover":
    # "" = the shared discovery topic every Agent Card is published to. A named
    # topic searches /pilot/1/discovery-<name>/proto, where nothing publishes.
    var topic = ""
    var timeout = 30
    var jsonOutput = false
    var i = 0
    while i < rest.len:
      if rest[i] == "--timeout" and i + 1 < rest.len:
        timeout = parseInt(rest[i + 1])
        i += 2
      elif rest[i] == "--json":
        jsonOutput = true
        i += 1
      elif not rest[i].startsWith("-"):
        topic = rest[i]
        i += 1
      else:
        i += 1
    runDiscover(cfg, topic, timeout, jsonOutput)

  of "contact", "contacts":
    # Paste the keys once, here — then it is "@name" everywhere, including in
    # natural-language chat, where raw key material is never allowed through.
    if rest.len == 0 or (rest.len == 1 and rest[0].toLowerAscii() == "list"):
      let names = listContacts(cfg)
      if names.len == 0:
        info("no saved addresses yet")
        echo DIM & "  add one: pilot contact b ~/npk-b.json" & RESET
      else:
        for n in names: echo "  @" & n
    elif rest.len >= 2:
      let (path, err) = saveContact(cfg, rest[0], rest[1 .. ^1].join(" "))
      if err != "":
        fail(err)
        quit(1)
      let clean = rest[0].strip(chars = {'@'})
      ok("saved @" & clean)
      kv("Use", "pilot chat  ->  /send @" & clean & " <amount> <reason>")
      kv("File", path)
    else:
      fail("Usage: pilot contact <name> <keys JSON or file>   (no args lists them)")
      quit(1)

  of "peer", "peers":
    # A peer becomes usable the moment its card is stored, with no dependence on
    # broadcast discovery — which reaches the relay but never comes back on this
    # network (2026-07-26). Listing reuses `discover`: imported cards are cached
    # under the same shared discovery topic, so they show up in the same table.
    if rest.len == 0 or (rest.len == 1 and rest[0].toLowerAscii() == "list"):
      runDiscover(cfg, "", 30, false)
    elif rest.len >= 2 and rest[0].toLowerAscii() == "add":
      let (card, err) = readPeerCard(rest[1 .. ^1].join(" "))
      if err != "":
        fail(err)
        quit(1)
      runPeerAdd(cfg, card)
    else:
      fail("Usage: pilot peer add <card.json | pasted card>   (no args lists known peers)")
      quit(1)

  of "open", "close":
    # Your agent does not put itself up for sale. Until `pilot open`, nobody can hire it:
    # its inbox is not on the wire and its Agent Card is built but never broadcast. The
    # decision is remembered, so a reboot comes back the way you left it.
    let opening = subcmd == "open"
    let m = if opening: "agentOpenForHire" else: "agentCloseForHire"
    let r = daemonCall(cfg, m)
    if r.strip().toLowerAscii() in ["true", "1"]:
      if opening:
        ok("Open for hire.")
        kv("Means", "strangers can now send your agent paid work, and its card is broadcast")
        kv("Stop", "pilot close")
      else:
        ok("Closed for hire.")
        kv("Means", "strangers can no longer send work; your agent still finds peers to hire")
        kv("Reopen", "pilot open")
    else:
      fail((if opening: "Could not open for hire: " else: "Could not close for hire: ") & r.strip())
      quit(1)

  of "configure", "config":
    if rest.len < 2:
      fail("Usage: pilot configure <key> <value>")
      quit(1)
    runConfigure(cfg, rest[0], rest[1])

  of "poll":
    # Pull inbound A2A traffic (peer cards, tasks for us, replies to our tasks) from the
    # relay's store. The current delivery host loses every push event after its first one
    # (measured 2026-08-25), so until that is fixed upstream this is how the agent learns
    # what arrived. Idempotent — safe to run any time, as often as you like.
    echo daemonCall(cfg, "agentPoll")

  of "status":
    let jsonOutput = "--json" in rest
    runStatus(cfg, jsonOutput)

  of "version", "--version", "-v":
    echo "pilot " & VERSION

  of "help", "--help", "-h":
    echo USAGE

  else:
    fail("Unknown command: " & subcmd)
    blankLine()
    echo USAGE
    quit(1)

when isMainModule:
  main()
