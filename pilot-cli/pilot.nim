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
  contact <name> <keys|file>   Save an address as @name (list them with: contact)
  configure <key> <value>      Set configuration
  help                         Show this help

""" & DIM & "FLAGS" & RESET & """
  --testnet, --mainnet         Network for deploy
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
    var network = "testnet"
    for a in rest:
      if a == "--mainnet": network = "mainnet"
      elif a == "--testnet": network = "testnet"
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

  of "configure", "config":
    if rest.len < 2:
      fail("Usage: pilot configure <key> <value>")
      quit(1)
    runConfigure(cfg, rest[0], rest[1])

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
