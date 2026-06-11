# Self-Evolving Infrastructure Components — Patterns for Aura

**Status:** Design exploration for scenarios 1 of the
[Top Insights issue #98] / Scenario 1 issue #85.

## Why this is a killer scenario

Traditional infrastructure (Redis, Postgres, Nginx) ships as
static binaries. Operators tune them at config-file level and
restart on changes. A self-evolving infrastructure component:

1. **Observes its own workload** — request patterns, hot keys,
   latencies, memory pressure
2. **Decides what to change** — swap a data structure, retune
   eviction, rebuild an index
3. **Hot-swaps the new code in** — no restart, in-flight requests
   see either old or new behavior (never broken state)
4. **Verifies the change worked** — contract checks, metric
   deltas, auto-rollback on regression

Aura provides all four primitives. This doc is the pattern catalog
for assembling them.

## Pattern 1: Metrics → Trigger → Evolve → Verify

```
   ┌──────────────┐    ┌──────────────┐
   │  Hot-path    │    │  Background  │
   │  requests    │    │  evolve loop  │
   └──────┬───────┘    └──────▲───────┘
          │                    │
          ▼                    │
   ┌──────────────┐    ┌──────────────┐
   │  record      │    │  read        │
   │  metrics     ├───►│  metrics     │
   │  (per-op)    │    │  (aggregate) │
   └──────────────┘    └──────┬───────┘
                              │
                              ▼
                       ┌──────────────┐
                       │  decide:     │
                       │  swap?       │  ← E4 evolve-strategy
                       │  (heuristic) │
                       └──────┬───────┘
                              │ yes
                              ▼
                       ┌──────────────┐
                       │  snapshot +  │  ← ast:snapshot
                       │  rebind +    │    mutate:rebind
                       │  verify      │    contract check
                       └──────┬───────┘
                              │ fail
                              ▼
                       ┌──────────────┐
                       │  ast:restore │  ← rollback
                       └──────────────┘
```

### Aura-level implementation skeleton

```aura
(define (evolve-loop)
  (define info (gc-arena-info))
  (define (cache-info)
    (car (filter (lambda (i) (member? "cache.aura" (cdr (assq 'name i)))) info)))

  (let ((pct (cdr (assq 'used-pct (cache-info)))))
    (when (> pct 80)
      ;; 1. snapshot for rollback
      (define snap (ast:snapshot (string-append "evolve:" (current-time))))

      ;; 2. attempt the swap
      (define result (hot-swap:define 'cache-get
        '(lambda (key) (let ((v (hash-ref *cache* key)))
                       (if v v (default-lookup key))))))

      ;; 3. contract check: new impl respects invariants
      (if (post-condition-passed? result)
          (timeline-record! "evolve succeeded")
          (begin
            (ast:restore (cdr (assq 'snapshot-id result)))
            (timeline-record! "evolve rolled back"))))))

(evolve-loop)  ; run periodically
```

## Pattern 2: Layered evolution

Different parts of a component evolve on different timescales:

| Layer | Timescale | Examples | Tool |
|---|---|---|---|
| Cache policy (LRU/LFU/ARC) | Minutes | Eviction strategy | `mutate:rebind` |
| Data structure layout | Hours | Hash table → B-tree | `mutate:replace-pattern` |
| Index strategy | Days | Skip list → ART | New module + `gc-module` |
| Replication policy | Weeks | Sync → async batched | New evolved strategy |

Each layer can use the same evolve pattern but with different
trigger thresholds and rollback policies.

## Pattern 3: Safe hot-swap in flight

When a function is rebound mid-execution, two guarantees:

1. **In-flight requests complete with old code**: closures captured
   before the swap keep their `func_id` and execute the old body.
2. **New requests get new code**: the new Lambda is reachable from
   the bound name, and any newly-constructed closure captures it.

This means **no half-state**, ever. The work needed to upgrade
"hot-swap mid-execution" semantics to true zero-downtime (e.g.
drain connection pool before swap) is the **operator's** concern;
Aura provides the underlying invariant.

## Pattern 4: Memory budget adaptation

```aura
(define (adapt-memory-budget workload-class)
  (case workload-class
    ('read-heavy (set-memory-policy
                  (hash "auto-gc" #t "critical-pct" 95)))
    ('write-heavy (set-memory-policy
                  (hash "auto-gc" #t "critical-pct" 80)))
    ('memory-constrained (set-memory-policy
                          (hash "auto-gc" #t "critical-pct" 70
                                "cooldown-evals" 2000)))))
```

`(set-memory-policy)` exists today. Workload classification can
come from a separate metrics goroutine.

## Why Aura is uniquely positioned

| Feature | Redis (static) | Custom rewrite (months) | Aura |
|---|---|---|---|
| Self-evolution | ❌ restart | ❌ rewrite | ✅ `evolve-strategy` + `mutate:rebind` |
| Hot-swap | ❌ restart | ❌ rewrite | ✅ `hot-swap:define` (in progress, `#80` follow-up) |
| Safe rollback | ❌ backup+restore | ⚠️ manual | ✅ `ast:snapshot` + `ast:restore` |
| Memory observability | ⚠️ INFO command | ⚠️ custom | ✅ `gc-arena-info` + `memory-pressure` |
| Type system for evolved code | ❌ none | ⚠️ depends | ✅ gradual typing + ownership |
| Contracts on evolve path | ❌ none | ⚠️ custom | ✅ `AURA_CONTRACT_PRE/POST` (`#83`) |

## Reference implementations

- `projects/evo-kv/` — full self-evolving KV store example
- `projects/chat/` — message-passing pattern (for orchestrator-style)
- `tests/contracts_test.aura` — AURA_CONTRACT usage
- `tests/multi_session_leak_test.aura` — long-running serve memory
- `tests/nested_intend_test.aura` — nested evolution (`#68`)

## Open questions

- **Operator UI**: how does a human operator approve a candidate
  evolution? Currently: read the `*evo-log*` and either let the
  next evolve proceed or restore. A `dashboard.aura` UI is a natural
  follow-up.
- **Multi-component evolution**: a KV + a cache + a queue all
  evolving concurrently — cross-component contention on memory and
  CPU is an open problem.
- **Evolution policy migration**: when the evolve-strategy itself
  needs to change (the "meta-evolution"), the strategy is itself
  subject to mutate. E4's `parent` field tracks this lineage but the
  meta-strategy is not yet designed.
