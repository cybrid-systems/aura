# Agent Orchestration — Status

This document captures the runtime semantics of the `aura.orch` module that
operators need to reason about (admission gates, decay windows, drain
policies). It is the agent-orchestration counterpart to the architecture
overview; the design rationale lives in commit messages + close comments,
not here.

## Mailbox backpressure admission gate (#2228 + #2465)

`spawn_agent_with_mailbox(...)` consults the process-wide
`g_orch_module_stats.mailbox_bp_recent_total` (atomic counter, bumped in
the two strong-def `MultiFiberMailbox::push` / `broadcast_fanout` BP
sites) when deciding whether to admit a new agent with `attach_mailbox`.

### Threshold

- Env override: `AURA_ORCH_BP_ADMIT_THRESHOLD=N` (uint64; invalid input
  falls back to default).
- Default: `0` (admission gate disabled — every spawn is admitted).
- When set to `N > 0`, the gate soft-rejects spawns with
  `quota_dimension = "mailbox-bp"` once the BP counter is `>= N`. The
  reject path is no-leak by `#2155` parity (`reserved_memory_bytes == 0`,
  no name-table put).

### Decay window (#2465)

Without decay, `mailbox_bp_recent_total` was process-wide monotonic
forever — once a deployment set `AURA_ORCH_BP_ADMIT_THRESHOLD > 0`, the
first BP event permanently denied every subsequent
`spawn_agent_with_mailbox` call (DoS for AI agent frameworks).

- Env override: `AURA_ORCH_BP_DECAY_MS=N` (uint64; invalid input falls
  back to default; `N = 0` disables decay — counter is then truly
  monotonic forever; useful for diagnostic captures only).
- Default: `30000` (30 seconds, matches `kJoinDrainResidualHardMsDefault`
  from `#2155` / `#2227`).
- Decay check runs in `spawn_agent_with_mailbox` preflight. Uses
  `compare_exchange_strong` on a process-wide
  `g_mailbox_bp_last_decay_us` so concurrent preflights only reset the
  counter once per window. Counter can race-increment between reset and
  read below — acceptable: a one-off admit-when-should-deny is far
  better than permanent-denial.
- Reset path: `mailbox_bp_recent_total.store(0,
  std::memory_order_release)`.

### Observed metrics

`query:orch-module-stats` exposes `mailbox-bp-recent-total` +
`spawn-bp-admit-reject-total` (schema `2228` + issue `2465` for the
decay semantics).

## Drain policies (cross-reference)

- `kDefaultJoinDrainMs = 2000` — primary cancel drain before
  force-reclaim (`#2153`).
- `kJoinDrainResidualHardMsDefault = 30000` — upper bound on
  `min(drain_ms * 8, hard_ms)` (`#2227`).
- `kMailboxBpDecayMsDefault = 30000` — mailbox BP counter decay window
  (`#2465`).

All three share the 30-second baseline so operators have one number to
reason about for "how long does the orch module remember recent events".