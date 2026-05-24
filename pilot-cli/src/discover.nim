import strutils, json
import rpc, daemon, format

proc runDiscover*(cfg: Config, topic: string, timeout: int, jsonOutput: bool) =
  if not jsonOutput:
    header("Agent Discovery")
    info("Topic: " & topic)
    blankLine()
    spinner("Searching")

  let startedDaemon = not isDaemonRunning(cfg)
  if startedDaemon:
    if not startDaemon(cfg):
      fail("Failed to start daemon")
      return

  let raw = daemonCall(cfg, "agentDiscover", @[topic])

  if jsonOutput:
    echo raw
    if startedDaemon: stopDaemon(cfg)
    return

  clearLine()

  try:
    let j = parseJson(raw)

    if j.hasKey("error"):
      fail(j["error"].getStr())
      if startedDaemon: stopDaemon(cfg)
      return

    let agents = j.getOrDefault("agents")
    if agents.isNil or agents.kind != JArray or agents.len == 0:
      let note = j.getOrDefault("note").getStr("")
      if note != "":
        info(note)
      else:
        info("No agents found on '" & topic & "'")
      if startedDaemon: stopDaemon(cfg)
      return

    ok($agents.len & " agent(s) found")
    blankLine()

    tableHeader(("NAME", 22), ("NPK", 20), ("URL", 30))
    for agent in agents:
      let name = agent.getOrDefault("name").getStr("?")
      let npk = agent{"_logos", "npk"}.getStr(
                  agent.getOrDefault("npk").getStr("?"))
      let url = agent.getOrDefault("url").getStr("?")
      tableRow((name, 22), (truncStr(npk, 16), 20), (url, 30))

  except JsonParsingError:
    warn("Could not parse response")
    echo raw

  blankLine()
  if startedDaemon: stopDaemon(cfg)
