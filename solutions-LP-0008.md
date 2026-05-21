## Summary

- Autonomous AI agent module for the Logos stack (universal C++ module)
- 21 composable skills across 6 categories (wallet, storage, messaging, agent, blockchain, meta)
- A2A v1.0.0 agent coordination over Waku with JSON-RPC 2.0 + `_logos` extension
- Shielded LEZ wallet with owner-controlled spending threshold FSM (9 states)
- E2E encrypted owner channel via chat_module
- SQLite WAL persistence with crash recovery
- Basecamp UI: 4-tab Qt/QML interface (Chat, Approvals, Status, Files)

## Demo

**Narrated video:** [TODO: Upload to GitHub releases]

All demo operations run against the Logos runtime:
- logoscore module loading and method invocation
- Module inspector (`lm`) showing all 33 registered methods
- `.lgx` package creation and installation via `lgpm`

## Repository

https://github.com/johnniedom/pilot

## Reproducible demo

```bash
git clone https://github.com/johnniedom/pilot
cd pilot
./demo.sh
```

## Checklist

### Functionality
- [x] Agent module loads alongside other modules without modifications
- [x] Agent has own shielded LEZ account, independent of owner's wallet
- [x] Single CLI command deployment and configuration
- [x] Real-time owner interaction via Logos Messaging with no intermediary server
- [x] Spending threshold mechanism (autonomous below limit, requires approval above)
- [x] All 21 default skills implemented and documented
- [x] Agent-to-agent coordination is A2A-compatible (JSON-RPC 2.0, Agent Cards, task lifecycle)
- [ ] Two or more agents can discover, execute tasks, and transfer LEZ payment autonomously
- [ ] At least 3 illustrative use cases demonstrated end-to-end on LEZ testnet

### Usability
- [x] Documented skill interface allowing third parties to add skills without modifying core module
- [x] Owner-facing interface accessible from Logos app (Basecamp UI with 4 tabs)

### Reliability
- [x] Agent recovers from network interruptions (recoverPendingTransactions on restart)
- [x] Above-threshold transactions that fail to reach owner for approval are NOT executed
- [x] Skill failures are isolated (failing skill doesn't crash module or affect other skills)

### Supportability
- [ ] Deployed and tested on LEZ devnet/testnet
- [ ] End-to-end integration tests in CI, CI must be green
- [x] README with deployment steps and usage instructions
- [x] Reproducible demo script that works against real local sequencer
- [ ] Recorded video demo showing terminal output
