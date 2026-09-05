import os, osproc, strutils, times, json
import rpc, format, modules

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
  # The daemon loads modules from cfg.modulePath. Install the ones that are not there yet
  # (fresh machine, wiped /tmp) from the nix store instead of starting over an empty
  # directory and failing later with "storage_module not found".
  let missing = missingModules(cfg.modulePath, REQUIRED_MODULES)
  if missing.len > 0:
    clearLine()
    step("Installing modules missing from " & cfg.modulePath & ": " & missing.join(", "))
    if not installMissingModules(cfg, missing):
      fail("Module install incomplete — not starting a daemon that would lack " & missing.join(", "))
      return false
  createDir(cfg.dataDir)
  createDir(cfg.dataDir / "wallet_storage")
  cleanStaleDaemon(cfg)

  let logFile = cfg.dataDir / "daemon.log"
  let scriptFile = cfg.dataDir / ".start-daemon.sh"

  # setsid detaches the daemon into its own session so it survives
  # the parent bash exit (required when launched from Nim execProcess).
  # Start daemon without -m to avoid loading all modules simultaneously.
  # Modules are loaded individually below with delays to prevent crashes.
  #
  # RISC0 env is baked into the script: the wallet (inside lez_core)
  # proves transfers in-process, and a daemon booted from a bare shell without
  # RISC0_DEV_MODE=1 silently grinds a REAL proof per transfer (~45 min, GBs of
  # RAM) — seen live 2026-07-08. Passthrough keeps real-proof demos possible:
  # export RISC0_DEV_MODE=0 before deploy and it is honored.
  writeFile(scriptFile,
    "#!/bin/bash\n" &
    "export RISC0_DEV_MODE=\"${RISC0_DEV_MODE:-1}\"\n" &
    "if [ -z \"$LOGOS_BLOCKCHAIN_CIRCUITS\" ]; then\n" &
    "  export LOGOS_BLOCKCHAIN_CIRCUITS=$(find /nix/store -maxdepth 1 -name '*logos-blockchain-circuits*' -type d 2>/dev/null | head -1)\n" &
    "fi\n" &
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

  # Load modules one at a time with stabilization delay
  var moduleIdx = 0
  for m in MODULES.split(','):
    spinTick("Loading " & m, 15 + moduleIdx)
    discard execProcess("bash", args = ["-c",
      "timeout 15 " & quoteShell(cfg.logoscore) &
      " --config-dir " & quoteShell(cfg.configDir) & " load-module " & m],
      options = {poUsePath, poStdErrToStdOut})
    sleep(2000)
    inc moduleIdx

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
      # Give dependency modules time to fully stabilize
      for w in 0 ..< 3:
        spinTick("Waiting for modules", 25 + w)
        sleep(2000)
      spinTick("Initializing", 30 + i)
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
