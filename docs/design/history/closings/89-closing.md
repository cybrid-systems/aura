# Issue #89 — Multi-Agent Orchestration Platforms with Evolving Coordination

## Status: ✅ ADDRESSED

The Scenario 5 design doc requested by #89 has been written and
committed in this same branch.

## Deliverable

`docs/design/agent-orchestration-evolution.md` — 12.7 KB design
document covering:

- **What's already on main vs. what's new in this doc** —
  explicit table distinguishing orchestration-evolution from
  agent-evolution and static orchestration
- **Coordination pattern as data** — `coordination-pattern`
  record schema, `*coordination-pool*` registry
- **The orchestration evolution loop** — full diagram from
  task arrival → bandit pick → execute → history update → E4
  propose mutation → shadow test → promote/discard
- **Pattern 1: Conflict resolution evolution** — bandit picks
  resolution strategy per task shape (priority vs reviewer
  vs majority vs LLM-arbitrator vs abstain)
- **Pattern 2: Decomposition strategy evolution** — sequential
  vs parallel-fanout vs hierarchical vs auction, task-shape
  conditional
- **Pattern 3: Retry and backoff evolution** — linear vs
  exponential vs jittered vs circuit-breaker, bandit learns
  when each is right
- **Pattern 4: Communication pattern evolution** — shared-bus
  vs pub-sub vs blackboard vs gossip, scaled by agent count
- **Conflict-of-interest handling** — bandit vote, hierarchical
  meta-bandit, parallel-races-and-pick
- **Safety** — `AURA_CONTRACT_PRE` enforces no in-flight swap;
  swaps are queued at task boundary
- **Memory** — `*coordination-history*` as `pmr::vector` in
  `temp_arena_` with 10K active + older archived for batch
  re-learning
- **Meta-metrics** — time-to-solution, cost, quality,
  conflict-rate, rollback-rate, bandit-convergence
- **Industry comparison** — LangGraph, CrewAI, AutoGen, Airflow,
  Temporal all have fixed orchestration; only Aura evolves it

## Why this is a closing comment, not a code change

The issue is a **design scenario** (not a feature request). It
asks for documentation patterns. All primitives cited are in
place:

| Cited primitive | Status / Doc |
|---|---|
| `spawn` / `conduct` / `pipeline` / `parallel` | `agent_orchestration.md` |
| `fiber:join` | Added in `72c9559` (pre-issue) |
| `mutate:rebind` | Hot-swap primitive, exists |
| `ast:snapshot` / `ast:restore` | Snapshot + restore, exists |
| `AURA_CONTRACT_PRE/POST` | Added in #83 (`89e8782`) |
| `E4 evolve-strategy` | Added in #63 Phase 3 (`0ee43c8`) |
| `intend` primitive | `intent_orchestration.md` |
| `intend-history` | Exists |
| `pmr::vector` in `temp_arena_` | `unify_cell_heap.md` + `double-arena.md` |

## How to close on GitHub

```bash
gh issue close 89 -c "See docs/design/agent-orchestration-evolution.md
(Scenario 5 design doc) — coordination patterns as evolvable
data, 4 patterns (conflict resolution, decomposition, retry,
communication), safety contracts, pmr-backed history, industry
comparison. All cited primitives are in place on main."
```

Or paste this file as a GitHub comment.
