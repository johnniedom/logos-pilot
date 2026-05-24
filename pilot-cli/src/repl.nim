import strutils, json, rdstdin, os
import rpc, daemon, format

var gCfg: Config

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
    return daemonCall(cfg, "walletBalance")
  of "/history":
    return daemonCall(cfg, "walletHistory")
  of "/skills":
    return daemonCall(cfg, "metaSkills")
  of "/status":
    return daemonCall(cfg, "metaStatus")
  of "/files":
    return daemonCall(cfg, "storageList")
  of "/discover":
    let topic = if parts.len > 1: parts[1] else: "pilot"
    return daemonCall(cfg, "agentDiscover", @[topic])
  of "/send":
    if parts.len < 4:
      return RED & "  usage: /send <recipient> <amount> <reason>" & RESET
    return daemonCall(cfg, "walletSend", @[parts[1], parts[2], parts[3]])
  of "/upload":
    if parts.len < 3:
      return RED & "  usage: /upload <path> <label>" & RESET
    return daemonCall(cfg, "storageUpload", @[parts[1], parts[2]])
  of "/download":
    if parts.len < 3:
      return RED & "  usage: /download <cid> <path>" & RESET
    return daemonCall(cfg, "storageDownload", @[parts[1], parts[2]])
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
    return daemonCall(cfg, "walletBalance")
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
      return daemonCall(cfg, "storageUpload",
        @[p.getOrDefault("path").getStr(""),
          p.getOrDefault("label").getStr("")])
  of "skills":
    return daemonCall(cfg, "metaSkills")
  of "status":
    return daemonCall(cfg, "metaStatus")
  of "discover":
    return daemonCall(cfg, "agentDiscover", @["pilot"])
  of "none":
    return ""
  else:
    return DIM & "  (unhandled: " & act & ")" & RESET
  return ""

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

  ok("Agent online")
  if accountId != "":
    kv("Account", truncStr(accountId, 24))
  echo DIM & "  Type /help for commands, or just chat." & RESET
  blankLine()

  setControlCHook(cleanup)

  while true:
    var line: string
    try:
      let prompt = GREEN & "❯ " & RESET
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
      try:
        let raw = daemonCall(gCfg, "processOwnerMessage", @[line])
        let j = parseJson(raw)
        response = dispatchAction(gCfg, j)
      except JsonParsingError:
        response = daemonCall(gCfg, "processOwnerMessage", @[line])
      except:
        response = RED & "  error: " & getCurrentExceptionMsg() & RESET

    if response == "\x00QUIT":
      cleanup()
      return

    if response != "":
      echo response
