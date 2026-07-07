import strutils, os, rdstdin, terminal, json
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

  # startDaemon already fires `initialize` inside the module; this one is only a safety
  # net for the "module not responding yet" startup path. Short client timeout: a fresh
  # wallet (create + register + fund) legitimately outlives any single RPC window, and a
  # client-side timeout does NOT stop the module's work.
  discard daemonCall(cfg, "initialize", @[cfg.dataDir], timeoutSec = 5)

  # Poll for the identity instead of declaring failure while the module is still working.
  # NEVER stop the daemon on a slow start: the fresh keys only reach disk after a wallet
  # save, so a kill here orphans the identity and the next boot's divergence guard mints
  # another one (the identity-churn bug of 2026-07-07).
  var npk = ""
  var accountId = ""
  for i in 0 ..< 60:
    spinTick("Creating identity (registration + chain scan can take a couple of minutes)", i)
    npk = daemonCall(cfg, "getAgentNpk", timeoutSec = 5)
    if npk != "" and not npk.contains("error"):
      accountId = daemonCall(cfg, "getAccountId", timeoutSec = 5)
      break
    sleep(2000)
  clearLine()

  if npk == "" or npk.contains("error"):
    fail("Agent identity not ready after 2 minutes")
    info("The module may still be initializing — daemon left running so no work is lost.")
    info("Check again shortly with: pilot status")
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
      # Masked input: the deploy screen is exactly what the demo video records, and a
      # plain readLine echoed the full key on screen (and into terminal scrollback).
      # A hidden prompt gives zero feedback, so a key pasted twice concatenates
      # silently (a 102-char sk-…sk-…sk- landed in pilot.db this way). If the key's
      # own prefix reappears inside the value, re-prompt; the echoed length lets a
      # human catch anything the guard misses.
      for _ in 0 ..< 3:
        apiKey = readPasswordFromStdin(DIM & "  API key: " & RESET).strip()
        let sig = if apiKey.len >= 6: apiKey[0 ..< 6] else: apiKey
        if apiKey.len >= 12 and apiKey.find(sig, 1) > 0:
          warn("That looks pasted more than once (" & $apiKey.len & " chars) — try once, then Enter")
          apiKey = ""
        else:
          break
      if apiKey.len > 6:
        echo DIM & "  API key: " & apiKey[0..4] & repeat("•", 12) & "  (" & $apiKey.len & " chars, hidden)" & RESET

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
  # Funding (register + pinata claim + shielded transfer) runs inside the module and the
  # wallet only hits disk once it COMPLETES. Wait for it honestly instead of printing a
  # premature 0 and moving on — interrupted funding is how identities were lost.
  var balance = ""
  var funded = false
  for i in 0 ..< 90:
    spinTick("Funding agent (waiting for the on-chain claim to land)", i)
    balance = daemonCall(cfg, "walletBalance", timeoutSec = 10)
    try:
      let bj = parseJson(balance)
      if bj.hasKey("balance") and bj["balance"].getStr("0") notin ["", "0"]:
        funded = true
        break
    except: discard
    sleep(2000)
  clearLine()
  info(balance)
  if not funded:
    warn("Balance still 0 — funding may still be running, or the sequencer is unreachable")
    info("The daemon keeps working in the background; re-check with: pilot status")

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
  echo DIM & "  The agent daemon stays running; pilot chat / pilot status attach to it." & RESET
  blankLine()

  # Deliberately NOT stopping the daemon here. stopDaemon() escalates to pkill after 5s,
  # and a kill while funding / wallet writes are in flight corrupts wallet_storage.json
  # or drops the unsaved account keys — the next boot then resets the identity.
