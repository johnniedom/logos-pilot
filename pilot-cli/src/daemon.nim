import os, osproc, strutils, times, json
import rpc, format

proc readPidFromState(cfg: Config): int =
  let stateFile = cfg.configDir / "daemon" / "state.json"
  if fileExists(stateFile):
    try:
      let j = parseJson(readFile(stateFile))
      return j["pid"].getInt()
    except: discard
  return 0

proc isProcessAlive(pid: int): bool =
  if pid <= 0: return false
  return dirExists("/proc/" & $pid)

proc isDaemonRunning*(cfg: Config): bool =
  try:
    let raw = execProcess("bash", args = ["-c",
      "timeout 3 " & quoteShell(cfg.logoscore) &
      " --config-dir " & quoteShell(cfg.configDir) & " status --json"],
      options = {poUsePath, poStdErrToStdOut}).strip()
    return raw.contains("\"running\"")
  except:
    return false

proc cleanStaleDaemon*(cfg: Config) =
  let pid = readPidFromState(cfg)
  if pid > 0 and not isProcessAlive(pid):
    let daemonDir = cfg.configDir / "daemon"
    if dirExists(daemonDir):
      removeDir(daemonDir)
  discard execProcess("bash", args = ["-c",
    "pkill -9 -f logos_host_qt 2>/dev/null; rm -f ~/.cache/storage/dht/providers/LOCK"],
    options = {poUsePath})

proc startDaemon*(cfg: Config): bool =
  if cfg.logoscore == "" or cfg.logoscore == "logoscore":
    fail("logoscore binary not found in nix store")
    return false
  createDir(cfg.dataDir)
  cleanStaleDaemon(cfg)

  let logFile = cfg.dataDir / "daemon.log"
  let scriptFile = cfg.dataDir / ".start-daemon.sh"

  # setsid detaches the daemon into its own session so it survives
  # the parent bash exit (required when launched from Nim execProcess).
  writeFile(scriptFile,
    "#!/bin/bash\n" &
    "setsid " & quoteShell(cfg.logoscore) &
    " --config-dir " & quoteShell(cfg.configDir) &
    " -D -m " & quoteShell(cfg.modulePath) &
    " > " & quoteShell(logFile) & " 2>&1 &\n")
  inclFilePermissions(scriptFile, {fpUserExec})
  discard execCmd("bash " & quoteShell(scriptFile))

  # Daemon writes its own PID to state.json — poll for it instead of $!
  var pid = 0
  for i in 0 ..< 10:
    sleep(500)
    pid = readPidFromState(cfg)
    if pid > 0: break

  if pid <= 0:
    fail("Daemon did not start — check " & logFile)
    return false

  if not isProcessAlive(pid):
    fail("Daemon died immediately — check " & logFile)
    return false

  # Poll until RPC responds (max 15s)
  for i in 0 ..< 15:
    spinTick("Starting daemon", i)
    if isDaemonRunning(cfg):
      break
    sleep(1000)

  # Load modules (15s timeout each)
  for m in MODULES.split(','):
    spinTick("Loading " & m, 15)
    discard execProcess("bash", args = ["-c",
      "timeout 15 " & quoteShell(cfg.logoscore) &
      " --config-dir " & quoteShell(cfg.configDir) & " load-module " & m],
      options = {poUsePath, poStdErrToStdOut})

  # Wait for pilot echo (max 10s)
  for i in 0 ..< 5:
    spinTick("Waiting for pilot", 20 + i)
    let resp = try:
      execProcess("bash", args = ["-c",
        "timeout 5 " & quoteShell(cfg.logoscore) &
        " --config-dir " & quoteShell(cfg.configDir) &
        " call pilot echo ready"],
        options = {poUsePath, poStdErrToStdOut}).strip()
    except: ""
    if resp.contains("ready"):
      spinTick("Initializing", 25 + i)
      discard execProcess("bash", args = ["-c",
        "timeout 30 " & quoteShell(cfg.logoscore) &
        " --config-dir " & quoteShell(cfg.configDir) &
        " call pilot initialize " & quoteShell(cfg.dataDir)],
        options = {poUsePath, poStdErrToStdOut})
      clearLine()
      return true
    sleep(2000)

  clearLine()
  warn("Daemon started but pilot module not responding yet")
  return true

proc stopDaemon*(cfg: Config) =
  discard execProcess("bash", args = ["-c",
    "timeout 5 " & quoteShell(cfg.logoscore) &
    " --config-dir " & quoteShell(cfg.configDir) & " stop"],
    options = {poUsePath, poStdErrToStdOut})
  discard execProcess("bash", args = ["-c",
    "pkill -f logos_host_qt 2>/dev/null; rm -f ~/.cache/storage/dht/providers/LOCK"],
    options = {poUsePath})

proc recordStartTime*(cfg: Config) =
  createDir(cfg.dataDir)
  writeFile(cfg.dataDir / ".pilot_start_time", $epochTime().int)

proc getUptime*(cfg: Config): string =
  let timeFile = cfg.dataDir / ".pilot_start_time"
  if not fileExists(timeFile): return "unknown"
  try:
    let start = parseInt(readFile(timeFile).strip())
    let elapsed = epochTime().int - start
    let h = elapsed div 3600
    let m = (elapsed mod 3600) div 60
    let s = elapsed mod 60
    return $h & "h " & $m & "m " & $s & "s"
  except:
    return "unknown"
