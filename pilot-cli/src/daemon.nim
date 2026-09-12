import os, osproc, strutils, times, json
import rpc, format, modules

# True only when THIS process launched the daemon. `pilot chat` used to stop the daemon on exit
# no matter who started it, so quitting the chat took down an agent that a service (and
# Basecamp's Pilot Remote) were relying on (2026-09-09).
var daemonStartedHere* = false

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
  # 10 s, not 3: a daemon in the middle of an agentPoll (an LLM turn, wallet calls) answered
  # `status` late on 2026-09-09, the CLI took "late" for "dead" and started a second daemon on
  # top of the live one — which killed it (cleanStaleDaemon below, and the port collision).
  try:
    let raw = execProcess("bash", args = ["-c",
      "timeout 10 " & quoteShell(cfg.logoscore) &
      " --config-dir " & quoteShell(cfg.configDir) & " status --json"],
      options = {poUsePath, poStdErrToStdOut}).strip()
    return raw.contains("\"running\"")
  except:
    return false

# A daemon whose pid is alive IS running, whether or not it answers `status` in time: a
# module deep in a chain call keeps the daemon's RPC busy for longer than any probe, and on
# 2026-09-12 `pilot status` took that silence for "no daemon", started one over the live
# agent and stopped it on exit. Callers that would start a daemon of their own ask this.
proc daemonPresent*(cfg: Config): bool =
  isProcessAlive(readPidFromState(cfg)) or isDaemonRunning(cfg)

proc cleanStaleDaemon*(cfg: Config) =
  # Only a daemon whose pid is gone is stale. Until 2026-09-09 the pkill below ran
  # unconditionally, so any code path that reached startDaemon while a daemon was alive
  # killed every module host on the machine, the live agent's included.
  let pid = readPidFromState(cfg)
  if pid > 0 and not isProcessAlive(pid):
    let daemonDir = cfg.configDir / "daemon"
    if dirExists(daemonDir):
      removeDir(daemonDir)
    discard execProcess("bash", args = ["-c",
      "pkill -9 -f " & quoteShell("instance-persistence-path " & cfg.configDir) & " 2>/dev/null; rm -f ~/.cache/storage/dht/providers/LOCK"],
      options = {poUsePath})

# Keys at rest (2026-09-05). The module wraps the agent's private keys (ecies.priv, enc.priv)
# with AES-256-GCM under a passphrase-derived key whenever PILOT_KEY_PASSPHRASE is set, and
# migrates plaintext rows in place on the next load — but deploy never set one, so every agent
# deployed by the CLI kept its keys in clear in pilot.db. Now: an explicit PILOT_KEY_PASSPHRASE
# wins (the headless path); else the one saved beside the agent's data is reused; else a fresh
# random one is generated, saved with mode 0600, and its LOCATION (never its value) is printed
# once. Losing the file loses A2A/owner crypto for that identity — it is part of the agent's
# backup, like pilot.db. Pure so the precedence is unit-tested.
proc resolveKeyPassphrase*(envValue, fileValue, fresh: string): tuple[value: string, generated: bool] =
  if envValue.strip() != "": return (envValue.strip(), false)
  if fileValue.strip() != "": return (fileValue.strip(), false)
  (fresh, true)

proc freshPassphrase(): string =
  ## 32 random bytes from the OS as 64 hex chars.
  const hexDigits = "0123456789abcdef"
  var f: File
  if not open(f, "/dev/urandom", fmRead): return ""
  var buf: array[32, byte]
  discard f.readBytes(buf, 0, 32)
  f.close()
  for b in buf:
    result.add hexDigits[int(b shr 4)]
    result.add hexDigits[int(b and 0x0f)]

proc keyPassphraseFor(cfg: Config): string =
  let passFile = cfg.dataDir / ".key-passphrase"
  let fileValue = if fileExists(passFile): readFile(passFile) else: ""
  let (value, generated) = resolveKeyPassphrase(getEnv("PILOT_KEY_PASSPHRASE"), fileValue, freshPassphrase())
  if generated and value != "":
    createDir(cfg.dataDir)
    writeFile(passFile, value & "\n")
    setFilePermissions(passFile, {fpUserRead, fpUserWrite})
    info("Key passphrase generated at " & passFile & " (mode 0600): the agent's private keys are stored encrypted with it — back it up together with pilot.db")
  elif not generated and getEnv("PILOT_KEY_PASSPHRASE") == "" and fileValue.strip() != "":
    info("Using the key passphrase saved at " & passFile)
  value

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
  # A live daemon is never replaced from here. If it exists but did not answer `status` in
  # time, say so and stop — a second daemon on the same config dir kills the first.
  let livePid = readPidFromState(cfg)
  if livePid > 0 and isProcessAlive(livePid):
    for i in 0 ..< 20:
      if isDaemonRunning(cfg): return true
      sleep(1000)
    fail("A daemon is already running (pid " & $livePid & ") but is not answering; wait a moment and try again")
    return false
  cleanStaleDaemon(cfg)

  let logFile = cfg.dataDir / "daemon.log"
  let scriptFile = cfg.dataDir / ".start-daemon.sh"
  let keyPass = keyPassphraseFor(cfg)

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
    (if keyPass != "": "export PILOT_KEY_PASSPHRASE=" & quoteShell(keyPass) & "\n" else: "") &
    "if [ -z \"$LOGOS_BLOCKCHAIN_CIRCUITS\" ]; then\n" &
    "  export LOGOS_BLOCKCHAIN_CIRCUITS=$(find /nix/store -maxdepth 1 -name '*logos-blockchain-circuits*' -type d 2>/dev/null | head -1)\n" &
    "fi\n" &
    "setsid " & quoteShell(cfg.logoscore) &
    " --config-dir " & quoteShell(cfg.configDir) &
    " -D -m " & quoteShell(cfg.modulePath) &
    " > " & quoteShell(logFile) & " 2>&1 &\n")
  inclFilePermissions(scriptFile, {fpUserExec})
  discard execCmd("bash " & quoteShell(scriptFile))
  daemonStartedHere = true

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
  # Only this config dir's module hosts (their command line carries its data path): another
  # agent's daemon on the same machine is not ours to kill.
  discard execProcess("bash", args = ["-c",
    "pkill -f " & quoteShell("instance-persistence-path " & cfg.configDir) & " 2>/dev/null; rm -f ~/.cache/storage/dht/providers/LOCK"],
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
