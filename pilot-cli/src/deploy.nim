import strutils, os, rdstdin
import rpc, daemon, selector, format

const LLM_PROVIDERS = @[
  "Anthropic (Claude)",
  "OpenAI (GPT)",
  "DeepSeek",
  "Google (Gemini)",
  "OpenRouter (multi-model)",
  "Groq (fast inference)",
  "Skip — command-only mode"
]

const LLM_PROVIDER_KEYS = @[
  ("anthropic", "ANTHROPIC_API_KEY"),
  ("openai", "OPENAI_API_KEY"),
  ("deepseek", "DEEPSEEK_API_KEY"),
  ("google", "GOOGLE_API_KEY"),
  ("openrouter", "OPENROUTER_API_KEY"),
  ("groq", "GROQ_API_KEY"),
  ("", "")
]

const LLM_MODELS: seq[seq[string]] = @[
  @["claude-sonnet-4-6-20250514", "claude-opus-4-7-20250506", "claude-haiku-4-5-20251001"],
  @["gpt-4.1", "gpt-4.1-mini", "gpt-4o"],
  @["deepseek-v4-pro", "deepseek-v4-flash", "deepseek-chat"],
  @["gemini-2.5-flash", "gemini-2.5-pro"],
  @["anthropic/claude-sonnet-4-6", "anthropic/claude-opus-4-7", "openai/gpt-4.1",
    "deepseek/deepseek-v4-pro", "google/gemini-2.5-flash", "google/gemini-2.5-pro",
    "meta-llama/llama-4-maverick", "qwen/qwen3-235b-a22b",
    "mistralai/mistral-large-2411", "Custom (type model ID)"],
  @["llama-3.3-70b-versatile", "llama-3.1-8b-instant"],
  @[]
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

  discard daemonCall(cfg, "initialize", @[cfg.dataDir], timeoutSec = 30)
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
  elif providerIdx < LLM_PROVIDERS.len - 1:
    let (providerName, envKey) = LLM_PROVIDER_KEYS[providerIdx]
    var apiKey = getEnv(envKey)
    if apiKey == "":
      stdout.write(DIM & "  API key: " & RESET)
      stdout.flushFile()
      var input: string
      discard readLineFromStdin("", input)
      apiKey = input.strip()

    if apiKey != "":
      putEnv(envKey, apiKey)
      discard daemonCall(cfg, "metaConfigure", @["llm.provider", providerName])
      discard daemonCall(cfg, "metaConfigure", @["llm.api_key", apiKey])

      let models = LLM_MODELS[providerIdx]
      if models.len > 0:
        let modelIdx = arrowSelect("Select model:", models)
        if modelIdx >= 0 and modelIdx < models.len:
          var modelId = models[modelIdx]
          if modelId.startsWith("Custom"):
            stdout.write(DIM & "  Model ID: " & RESET)
            stdout.flushFile()
            var customModel: string
            discard readLineFromStdin("", customModel)
            modelId = customModel.strip()
          if modelId != "":
            discard daemonCall(cfg, "metaConfigure", @["llm.model", modelId])
            ok("LLM → " & providerName & " / " & modelId)
          else:
            ok("LLM → " & providerName & " (default model)")
        else:
          ok("LLM → " & providerName & " (default model)")
      else:
        ok("LLM → " & providerName)
    else:
      warn("No API key — running in command-only mode")
  else:
    info("Command-only mode (no LLM)")

  blankLine()
  step("Bind owner identity")
  echo DIM & "  Your secp256k1 public key. The agent only obeys this identity." & RESET

  var ownerNpk = getEnv("PILOT_OWNER_NPK")
  if ownerNpk == "":
    stdout.write(DIM & "  Owner NPK: " & RESET)
    stdout.flushFile()
    var input: string
    discard readLineFromStdin("", input)
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
