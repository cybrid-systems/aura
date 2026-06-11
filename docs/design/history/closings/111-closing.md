# Issue #111 — Multi-agent orchestration: tests, examples, observability

## Status: ✅ CLOSED — covered by existing work + #112 docs refresh

Issue #111 was a P1 "make the multi-agent orchestration foundation
demonstrable" ask. Most of it was already in place from #109 (fiber
scheduler + concurrent tests) and #112 (docs refresh).

## Coverage by sub-task

| # | Sub-task | Status | Where |
|---|----------|--------|-------|
| 1 | Full multi-agent code-evolution demo | ✅ | `lib/std/orchestrator.aura` Section 5 (`orch:conduct` with `if`+`retry`); tutorial §10.5 multi-agent pipeline; `projects/evo-kv/` (15+ .aura files including `evo-kv-evolve.aura`, `evo-kv-auto.aura`) |
| 2 | 20+ test cases | ✅ | `tests/suite/orchestrator.aura` (37 test-suites) + `tests/suite/concurrent.aura` (12+ covering serial/parallel pipelines, timeout/retry, fiber:join) |
| 3 | Agent-specific observability | ✅ | `orch:metrics` / `orch:reset-metrics` (Issue #32) expose scheduler/fiber counters; `mailbox-count` exposes pending messages; `GCCollector::Metrics` (`src/serve/gc_coordinator.h`) tracks GC pause / safepoint wait / root count |
| 4 | Tutorial updates | ✅ | #112 sub-task 4 just shipped `docs/tutorial.md` §10.5 Self-Modifying Agent quickstart + `README.md` 30-second quickstart |
| 5 | Thread-safe JIT + GC | 🟡 | Multi-threaded fiber scheduler + work-stealing shipped in #109. GC safepoint mechanism exists. Full root set collection + parallel mark are still P0 infrastructure work tracked in **#113**. |

## What was specifically added for #111

Mostly nothing new — the closing was a recognition that the work was
already in place. The recent work that contributed:

- **#109**: fiber:join + concurrent test suite (12/12 PASS)
- **#110**: qar primitive (composes query + replace)
- **#112**: docs refresh (sub-task 4 was exactly #111's sub-task 4)
- **#109 Phase 4**: orch:metrics / orch:reset-metrics exposed
- **`tests/suite/orchestrator.aura`**: 37 test-suites for orch:*
- **`projects/evo-kv/`**: 15+ .aura files showcasing real Agent
  code-evolution workflows (auto-evolve, metrics, pubsub, zset, etc.)

## What's NOT in #111 (tracked in #113)

The only meaningful remaining work is the **thread-safe GC** piece
(sub-task 5), which is a P0 infrastructure issue (4 phases, weeks of
work) — too big to fold into #111 and properly tracked in #113.

## Verification

- `tests/suite/orchestrator.aura`: 37 test-suites PASS
- `tests/suite/concurrent.aura`: 12/12 PASS in `--load` and `--serve` modes
- `projects/evo-kv/test-evo-kv-full.aura`: PASS
- Tutorial §10.5 examples are runnable
- `orch:metrics` returns valid JSON with all expected fields

## Final tally

| Sub-task | Status | Where |
|----------|--------|-------|
| 1: End-to-end demo | ✅ Done | orchestrator.aura + evo-kv + tutorial §10.5 |
| 2: 20+ tests | ✅ Done | orchestrator.aura (37) + concurrent.aura (12+) |
| 3: Observability | ✅ Done | orch:metrics, mailbox-count, GCCollector::Metrics |
| 4: Tutorial updates | ✅ Done | #112 sub-task 4 |
| 5: Thread-safe GC | 🟡 Tracked in #113 | (separate P0 issue) |

No code changes from #111 itself. Closing as completed (not no_planned)
because the scope is genuinely covered; the GC piece is in #113.
