import strutils, os, rdstdin
import rpc, daemon, selector, format

const LLM_PROVIDERS = @[
  "Anthropic (Claude)",
  "OpenAI (GPT)",
  "Google (Gemini)",
  "OpenRouter (multi-model)",
  "Skip — command-only mode"
]

const LLM_PROVIDER_KEYS = @[
  ("anthropic", "ANTHROPIC_API_KEY"),
  ("openai", "OPENAI_API_KEY"),
  ("google", "GOOGLE_API_KEY"),
  ("openrouter", "OPENROUTER_API_KEY"),
  ("", "")
]

proc runDeploy*(cfg: Config, network: string) =
  header("Deploying Pilot Agent")
  kv("Network", network)
  blankLine()

  step("Creating agent identity...")
  spinner("Starting daemon")

  if not startDaemon(cfg):
    clearLine()
    fail("Failed to start daemon")
    return

  clearLine()
  recordStartTime(cfg)

  discard daemonCall(cfg, "initialize", @[cfg.dataDir])
  let npk = daemonCall(cfg, "getAgentNpk")
  let accountId = daemonCall(cfg, "getAccountId")

  if npk == "" or npk.contains("error"):
    fail("Identity generation failed")
    stopDaemon(cfg)
    return

  ok("Agent identity created")
  kv("NPK", truncStr(npk, 32))
  kv("Account", truncStr(accountId, 32))

  blankLine()
  let providerIdx = arrowSelect("Select LLM provider:", LLM_PROVIDERS)
  if providerIdx < 0:
    warn("Skipped LLM configuration")
  elif providerIdx < 4:
    let (providerName, envKey) = LLM_PROVIDER_KEYS[providerIdx]
    var apiKey = getEnv(envKey)
    if apiKey == "":
      var input: string
      discard readLineFromStdin(DIM & "  API key: " & RESET, input)
      apiKey = input.strip()

    if apiKey != "":
      putEnv(envKey, apiKey)
      discard daemonCall(cfg, "metaConfigure", @["llm.provider", providerName])
      ok("LLM provider → " & providerName)
    else:
      warn("No API key — running in command-only mode")
  else:
    info("Command-only mode (no LLM)")

  blankLine()
  step("Bind owner identity")
  echo DIM & "  Your secp256k1 public key. The agent only obeys this identity." & RESET

  var ownerNpk = getEnv("PILOT_OWNER_NPK")
  if ownerNpk == "":
    var input: string
    discard readLineFromStdin(DIM & "  Owner NPK: " & RESET, input)
    ownerNpk = input.strip()

  if ownerNpk != "":
    discard daemonCall(cfg, "metaConfigure", @["owner.npk", ownerNpk])
    ok("Owner bound → " & truncStr(ownerNpk, 24))
  else:
    warn("No owner key — agent accepts commands from anyone")

  blankLine()
  step("Checking wallet...")
  let balance = daemonCall(cfg, "walletBalance")
  info(balance)

  blankLine()
  step("Publishing Agent Card...")
  let cardResult = daemonCall(cfg, "agentCard")
  if cardResult.contains("error"):
    warn("Agent Card publish failed (network may be unavailable)")
  else:
    ok("Agent Card published")

  blankLine()
  hrule()
  echo GREEN & BOLD & "✓ Agent deployed" & RESET
  hrule()
  blankLine()
  kv("NPK", truncStr(npk, 40))
  kv("Account", truncStr(accountId, 40))
  kv("Network", network)
  blankLine()
  echo "  Run " & BOLD & "pilot chat" & RESET & " to start talking to your agent"
  blankLine()

  stopDaemon(cfg)
