import os, osproc, strutils, json

const
  DEFAULT_DATA_DIR* = "/tmp/pilot-data"
  DEFAULT_MODULE_PATH* = "/tmp/pilot-logoscore/modules"
  DEFAULT_WAKU_ADDR* = "/ip4/127.0.0.1/tcp/30303"
  MODULES* = "capability_module,lez_wallet_module,delivery_module,storage_module,pilot"

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
  result.dataDir = getEnv("PILOT_DATA_DIR", DEFAULT_DATA_DIR)
  result.configDir = getEnv("PILOT_CONFIG_DIR", result.dataDir / ".logoscore")
  result.modulePath = getEnv("PILOT_MODULE_PATH", DEFAULT_MODULE_PATH)
  result.wakuAddr = getEnv("PILOT_WAKU_ADDR", DEFAULT_WAKU_ADDR)

  result.logoscore = getEnv("LOGOSCORE")
  if result.logoscore == "":
    result.logoscore = findBinary("logoscore", "*logoscore-cli*")
  if result.logoscore == "":
    result.logoscore = "logoscore"

  result.logosHost = getEnv("LOGOS_HOST_PATH")
  if result.logosHost == "":
    let dir = findDir("*-logos-liblogos")
    if dir != "":
      result.logosHost = dir / "bin" / "logos_host"
      putEnv("LOGOS_HOST_PATH", result.logosHost)

proc extractDaemonResult*(raw: string): string =
  if raw.contains("\"status\":\"ok\""):
    try:
      let start = raw.find('{')
      let finish = raw.rfind('}')
      if start >= 0 and finish > start:
        let j = parseJson(raw[start .. finish])
        if j.hasKey("result"):
          return j["result"].getStr()
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

proc pilotCall*(cfg: Config, methodExpr: string): string =
  let fullCmd = "pilot." & methodExpr
  result = execProcess(cfg.logoscore,
    args = ["-m", cfg.modulePath, "-l", MODULES,
            "-c", fullCmd, "--quit-on-finish"],
    options = {poUsePath, poStdErrToStdOut})

proc daemonCall*(cfg: Config, meth: string, args: seq[string] = @[]): string =
  var cmdArgs = @["--config-dir", cfg.configDir, "call", "pilot", meth]
  cmdArgs.add(args)
  let raw = execProcess(cfg.logoscore, args = cmdArgs,
                        options = {poUsePath, poStdErrToStdOut})
  result = extractDaemonResult(raw)

proc daemonCallRaw*(cfg: Config, subcmd: string, args: seq[string] = @[]): string =
  var cmdArgs = @["--config-dir", cfg.configDir, subcmd]
  cmdArgs.add(args)
  result = execProcess(cfg.logoscore, args = cmdArgs,
                       options = {poUsePath, poStdErrToStdOut}).strip()
