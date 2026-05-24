import os, osproc, strutils, times
import rpc, format

proc readPid(cfg: Config): int =
  let pidFile = cfg.dataDir / "daemon.pid"
  if fileExists(pidFile):
    try:
      return parseInt(readFile(pidFile).strip())
    except: discard
  return 0

proc writePid(cfg: Config, pid: int) =
  createDir(cfg.dataDir)
  writeFile(cfg.dataDir / "daemon.pid", $pid & "\n")

proc removePid(cfg: Config) =
  let pidFile = cfg.dataDir / "daemon.pid"
  if fileExists(pidFile):
    removeFile(pidFile)

proc isProcessAlive(pid: int): bool =
  if pid <= 0: return false
  try:
    discard execProcess("kill", args = ["-0", $pid],
                        options = {poUsePath})
    return true
  except: return false

proc isDaemonRunning*(cfg: Config): bool =
  let raw = daemonCallRaw(cfg, "status", @["--json"])
  return raw.contains("\"running\"")

proc cleanStaleDaemon*(cfg: Config) =
  let pid = readPid(cfg)
  if pid > 0 and not isProcessAlive(pid):
    info("Cleaning stale daemon state...")
    discard daemonCallRaw(cfg, "stop")
    removePid(cfg)
    let daemonDir = cfg.configDir / "daemon"
    if dirExists(daemonDir):
      removeDir(daemonDir)

proc startDaemon*(cfg: Config): bool =
  createDir(cfg.dataDir)
  cleanStaleDaemon(cfg)

  let p = startProcess(cfg.logoscore,
    args = ["--config-dir", cfg.configDir, "-D", "-m", cfg.modulePath],
    options = {poUsePath})

  let pid = p.processID
  writePid(cfg, pid)

  # poll until responding (max 15s)
  for i in 0 ..< 15:
    sleep(1000)
    if isDaemonRunning(cfg):
      break

  if not isProcessAlive(pid):
    fail("Daemon failed to start")
    removePid(cfg)
    return false

  # load modules
  for m in MODULES.split(','):
    discard daemonCallRaw(cfg, "load-module", @[m])

  # wait for pilot module to respond (max 10s)
  for i in 0 ..< 5:
    let resp = daemonCall(cfg, "echo", @["ready"])
    if resp.contains("ready"):
      discard daemonCall(cfg, "initialize", @[cfg.dataDir])
      return true
    sleep(2000)

  warn("Daemon started but pilot module not responding yet")
  return true

proc stopDaemon*(cfg: Config) =
  discard daemonCallRaw(cfg, "stop")
  removePid(cfg)

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
