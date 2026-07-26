import strutils, json, times, os
import rpc, daemon, format

# Discovery is not request/response — it is "subscribe and wait". A card that was
# published before we subscribed is gone, and one published a second after our
# single query is missed. The module's agentDiscover subscribes, asks the store
# ONCE, and returns whatever it has; asking once and reporting "no agents found"
# made a working network look dead (2026-07-26). So poll until the deadline the
# caller already passes in (--timeout, previously accepted and ignored), keeping
# the daemon subscribed throughout so live cards land in its cache between polls.
proc runDiscover*(cfg: Config, topic: string, timeout: int, jsonOutput: bool) =
  if not jsonOutput:
    header("Agent Discovery")
    info("Topic: " & (if topic == "": "(shared discovery channel)" else: topic))
    blankLine()

  let startedDaemon = not isDaemonRunning(cfg)
  if startedDaemon:
    if not jsonOutput: spinner("Starting daemon")
    if not startDaemon(cfg):
      fail("Failed to start daemon")
      return
    if not jsonOutput: clearLine()

  let deadline = epochTime() + timeout.float
  var raw = ""
  var agents: JsonNode = newJArray()
  var polls = 0

  while true:
    inc polls
    raw = daemonCall(cfg, "agentDiscover", @[topic], timeoutSec = 30)
    try:
      let j = parseJson(raw)
      let found = j.getOrDefault("agents")
      if not found.isNil and found.kind == JArray and found.len > 0:
        agents = found
        break
    except JsonParsingError:
      discard

    let left = int(deadline - epochTime())
    if left <= 0: break
    if not jsonOutput:
      # Say what is actually happening — a bare spinner is why "no peers yet" and
      # "the network is broken" looked identical for hours.
      spinTick("Listening for agent cards — " & $left & "s left, " & $polls & " checks", polls)
    sleep(3000)

  if not jsonOutput: clearLine()

  if jsonOutput:
    echo raw
    if startedDaemon: stopDaemon(cfg)
    return

  try:
    let j = parseJson(raw)
    if j.hasKey("error"):
      fail(j["error"].getStr())
      if startedDaemon: stopDaemon(cfg)
      return
  except JsonParsingError:
    warn("Could not parse response")
    echo raw
    if startedDaemon: stopDaemon(cfg)
    return

  if agents.len == 0:
    info("No agent cards received in " & $timeout & "s (" & $polls & " checks)")
    echo DIM & "  A peer is only discoverable while it is running and publishing." & RESET
    echo DIM & "  On the peer, run: pilot chat -> /skills   (publishing happens on agent.card)" & RESET
    blankLine()
    if startedDaemon: stopDaemon(cfg)
    return

  ok($agents.len & " agent(s) found after " & $polls & " check(s)")
  blankLine()

  tableHeader(("NAME", 22), ("NPK", 20), ("URL", 30))
  for agent in agents:
    let name = agent.getOrDefault("name").getStr("?")
    let npk = agent{"_logos", "npk"}.getStr(
                agent.getOrDefault("npk").getStr("?"))
    let url = agent.getOrDefault("url").getStr("?")
    tableRow((name, 22), (truncStr(npk, 16), 20), (url, 30))

  blankLine()
  if startedDaemon: stopDaemon(cfg)
