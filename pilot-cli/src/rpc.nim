import os, osproc, strutils, json, algorithm

const
  # Fallbacks used only when $HOME is unset (rare: some CI/sandbox shells).
  # Normal runs default to a PERSISTENT per-user dir ($HOME/.pilot) — see
  # loadConfig — so agent identity, wallet and installed modules survive a
  # reboot / a /tmp wipe instead of silently vanishing.
  FALLBACK_DATA_DIR* = "/tmp/pilot-data"
  FALLBACK_MODULE_PATH* = "/tmp/pilot-logoscore/modules"
  DEFAULT_WAKU_ADDR* = "/ip4/127.0.0.1/tcp/30303"
  MODULES* = "capability_module,logos_execution_zone,delivery_module,storage_module,chat_module,pilot"

type
  Config* = object
    dataDir*: string
    configDir*: string
    modulePath*: string
    wakuAddr*: string
    logoscore*: string
    logosHost*: string

proc findBinary(pattern, pathFilter: string): string =
  let cmd = "find /nix/store -maxdepth 3 -name " & quoteShell(pattern) &
            " -path " & quoteShell(pathFilter) & " -type f 2>/dev/null | head -1"
  result = execProcess("bash", args = ["-c", cmd],
                       options = {poUsePath}).strip()

proc findDir(pattern: string): string =
  let cmd = "find /nix/store -maxdepth 1 -name " & quoteShell(pattern) &
            " -type d 2>/dev/null | head -1"
  result = execProcess("bash", args = ["-c", cmd],
                       options = {poUsePath}).strip()

proc loadConfig*(): Config =
  # Default to a PERSISTENT per-user home ($HOME/.pilot) so the agent survives a
  # reboot or /tmp wipe; fall back to /tmp only when $HOME is unavailable.
  let home = getHomeDir()
  let dataDefault = if home.len > 0: home / ".pilot" else: FALLBACK_DATA_DIR
  let moduleDefault = if home.len > 0: home / ".pilot" / "modules" else: FALLBACK_MODULE_PATH
  result.dataDir = getEnv("PILOT_DATA_DIR", dataDefault)
  result.configDir = getEnv("PILOT_CONFIG_DIR", result.dataDir / ".logoscore")
  result.modulePath = getEnv("PILOT_MODULE_PATH", moduleDefault)
  result.wakuAddr = getEnv("PILOT_WAKU_ADDR", DEFAULT_WAKU_ADDR)

  result.logoscore = getEnv("LOGOSCORE")
  if result.logoscore == "":
    result.logoscore = findBinary("logoscore", "*logoscore-cli-bin*")
  if result.logoscore == "":
    result.logoscore = findBinary("logoscore", "*logoscore-cli*")
  if result.logoscore == "":
    result.logoscore = "logoscore"

  result.logosHost = getEnv("LOGOS_HOST_PATH")
  if result.logosHost == "":
    result.logosHost = findBinary("logos_host", "*liblogos-bin*")
  if result.logosHost == "":
    result.logosHost = findBinary("logos_host", "*liblogos*")
  if result.logosHost != "":
    putEnv("LOGOS_HOST_PATH", result.logosHost)

proc extractDaemonResult*(raw: string): string =
  if raw.contains("\"status\":\"ok\""):
    try:
      let start = raw.find('{')
      let finish = raw.rfind('}')
      if start >= 0 and finish > start:
        let j = parseJson(raw[start .. finish])
        if j.hasKey("result"):
          # Bool-returning module methods (approveSpend/rejectSpend/...) arrive as
          # JSON booleans; getStr() on those collapses to "" and callers misread a
          # successful approval as "no response from agent" / "Transaction Failed".
          # Surface non-string results as their JSON text ("true", "false", ...).
          if j["result"].kind == JString:
            return j["result"].getStr()
          return $j["result"]
    except JsonParsingError:
      discard
    let resIdx = raw.find("\"result\":\"")
    if resIdx >= 0:
      var s = raw[resIdx + 10 .. ^1]
      let endIdx = s.find("\",\"status\":\"ok\"")
      if endIdx >= 0:
        s = s[0 ..< endIdx]
      return s.replace("\\\"", "\"")
  elif raw.contains("Result:"):
    let idx = raw.find("Result:")
    return raw[idx + 8 .. ^1].strip()
  return raw.strip()

# Resolve a send recipient: "@name" -> <dataDir>/contacts/<name>.json, else
# "@path" -> literal keys file, else pass the string through untouched. File
# contents are stripped of ALL whitespace (a trailing newline inside the keys
# JSON reached the wallet verbatim once). Returns (keys, "") or ("", error).
# Shared by the /send command and the LLM send action so raw key material never
# has to travel through (and be retyped by) the model.
# True when every '{' in the line has its '}' — i.e. a pasted keys blob arrived
# whole. The line editor flushes long pastes in chunks, so a /send can land with
# its tail still to come; the REPL uses this to keep reading instead of firing a
# half command at the wallet.
proc jsonBracesBalanced*(line: string): bool =
  var depth = 0
  var inStr = false
  var esc = false
  for c in line:
    if esc: esc = false; continue
    if c == '\\' and inStr: esc = true
    elif c == '"': inStr = not inStr
    elif not inStr:
      if c == '{': inc depth
      elif c == '}':
        dec depth
        if depth < 0: return false      # a '}' with no opener = head was eaten
  return depth == 0

# "" when this blob is a keys object the wallet can actually spend to, else a
# plain-English reason. Guards the one seam where a mangled paste used to sail
# through to walletSend and only fail minutes later at approve time.
proc recipientKeysError*(keys: string): string =
  if keys.len == 0: return "no recipient given"
  if not keys.startsWith("{") or not keys.endsWith("}") or not jsonBracesBalanced(keys):
    return "those recipient keys look cut off — paste lost its start or end.\n" &
           "  Use /send @<contact> <amount> <reason> instead (no pasting)."
  var parsed: JsonNode
  try: parsed = parseJson(keys)
  except: return "those recipient keys look cut off — not valid JSON.\n" &
                 "  Use /send @<contact> <amount> <reason> instead (no pasting)."
  if parsed.kind != JObject: return "recipient must be a keys object, not " & $parsed.kind
  for field in ["nullifier_public_key", "viewing_public_key"]:
    if not parsed.hasKey(field) or parsed[field].getStr("") == "":
      return "recipient keys are missing " & field & " — the paste is incomplete.\n" &
             "  Use /send @<contact> <amount> <reason> instead (no pasting)."
  return ""

# A long "/send {keys} 20 coffee" paste can lose its "/send " head to the line
# editor, leaving the keys blob as the first thing on the line — which then looks
# like chat. Rebuild the command, but ONLY when the keys parse and carry both
# fields, so corrupted key material is still refused rather than guessed at.
# Returns "" when the line is not a decapitated send.
proc salvageSendLine*(line: string): string =
  let s = line.strip()
  let open = s.find('{')
  let close = s.rfind('}')
  if open < 0 or close < open: return ""
  let keys = s[open .. close]
  if recipientKeysError(keys) != "": return ""
  let tail = s[close + 1 .. ^1].strip()
  if tail.len == 0: return ""
  let bits = tail.splitWhitespace()
  var amount = bits[0]
  for c in amount:
    if c notin {'0'..'9', '.'}: return ""
  let reason = if bits.len > 1: bits[1 .. ^1].join(" ") else: "(no reason given)"
  return "/send " & keys & " " & amount & " " & reason

proc resolveRecipient*(cfg: Config, recipient: string): tuple[keys, err: string] =
  if not recipient.startsWith("@"):
    return (recipient, "")
  var path = cfg.dataDir / "contacts" / (recipient[1..^1] & ".json")
  if not fileExists(path):
    path = expandTilde(recipient[1..^1])
  if not fileExists(path):
    return ("", "no contact or keys file named " & recipient[1..^1] &
            " (save one: " & cfg.dataDir / "contacts" & "/<name>.json)")
  var keys = ""
  for c in readFile(path):
    if c notin Whitespace: keys.add(c)
  if keys == "":
    return ("", "recipient file is empty: " & path)
  return (keys, "")

# Saving an address under a name is the only way to keep long key material off the
# owner's keyboard entirely: paste it ONCE here, then say "@b" forever after. This
# is also the right place to validate — a contact written wrong is a wrong recipient
# on every later send, discovered ~40 minutes into a real-proof transfer.
# `keysOrPath` accepts the keys JSON itself or a file containing them.
proc saveContact*(cfg: Config, name, keysOrPath: string): tuple[path, err: string] =
  var n = name.strip()
  if n.startsWith("@"): n = n[1 .. ^1]        # "/contact @b <keys>" is the natural spelling
  if n.len == 0: return ("", "give the contact a name, e.g. b")
  for c in n:
    if c notin {'a'..'z', 'A'..'Z', '0'..'9', '-', '_'}:
      return ("", "contact names can use letters, numbers, - and _ only (got: " & name & ")")

  var raw = keysOrPath.strip()
  if not raw.startsWith("{") and fileExists(expandTilde(raw)):
    raw = readFile(expandTilde(raw))
  var keys = ""
  for c in raw:
    if c notin Whitespace: keys.add(c)

  let err = recipientKeysError(keys)
  if err != "": return ("", err)

  let dir = cfg.dataDir / "contacts"
  try:
    createDir(dir)
    let path = dir / (n & ".json")
    writeFile(path, keys)
    return (path, "")
  except:
    return ("", "could not write the contact: " & getCurrentExceptionMsg())

# Names of every saved contact, sorted, without the .json.
proc listContacts*(cfg: Config): seq[string] =
  result = @[]
  let dir = cfg.dataDir / "contacts"
  if not dirExists(dir): return
  for kind, path in walkDir(dir):
    if kind == pcFile and path.endsWith(".json"):
      result.add(path.splitFile().name)
  result.sort()

# RPC_FAILED means the module behind the call is not there to answer — it does
# NOT mean the agent rejected the request. The usual cause: the sequencer went
# unreachable mid-transfer, upstream wallet code unwrapped the connect error
# (wallet/src/privacy_preserving_tx.rs), and the module process aborted; every
# later wallet call then reads like a refusal. Say what actually happened and
# what to do about it. `logTail` is the end of the daemon log; returns "" when
# the response is not an RPC failure.
proc explainRpcFailure*(resp, logTail: string): string =
  if not resp.contains("RPC_FAILED"): return ""
  var crashed = ""
  for line in logTail.splitLines():
    let idx = line.find("Module process crashed:")
    if idx >= 0: crashed = line[idx + 23 .. ^1].strip()
  if crashed == "":
    return "the agent is not answering — the daemon may be down or busy (try /status)"
  if crashed.contains("logos_execution_zone"):
    return "the wallet module crashed, so nothing was sent — no tokens moved.\n" &
           "  Usual cause: the sequencer stopped answering on :3040 mid-transfer.\n" &
           "  Fix: check the sequencer is up, then /quit and start pilot chat again."
  return crashed & " crashed — the agent needs a restart (/quit, then start pilot chat again)"

proc pilotCall*(cfg: Config, methodExpr: string): string =
  let fullCmd = "pilot." & methodExpr
  result = execProcess(cfg.logoscore,
    args = ["-m", cfg.modulePath, "-l", MODULES,
            "-c", fullCmd, "--quit-on-finish"],
    options = {poUsePath, poStdErrToStdOut})

proc daemonCall*(cfg: Config, meth: string, args: seq[string] = @[], timeoutSec = 10): string =
  var shellArgs = "timeout " & $timeoutSec & " " & quoteShell(cfg.logoscore) &
    " --config-dir " & quoteShell(cfg.configDir) & " call pilot " & meth
  for a in args:
    shellArgs &= " " & quoteShell(a)
  let raw = execProcess("bash", args = ["-c", shellArgs],
                        options = {poUsePath, poStdErrToStdOut})
  result = extractDaemonResult(raw)

proc daemonCallRaw*(cfg: Config, subcmd: string, args: seq[string] = @[]): string =
  var shellArgs = "timeout 10 " & quoteShell(cfg.logoscore) &
    " --config-dir " & quoteShell(cfg.configDir) & " " & subcmd
  for a in args:
    shellArgs &= " " & quoteShell(a)
  result = execProcess("bash", args = ["-c", shellArgs],
                       options = {poUsePath, poStdErrToStdOut}).strip()
