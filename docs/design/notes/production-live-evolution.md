# Production Live Systems with Safe Continuous Evolution

**Status:** Design exploration for Scenario 3 of the
[Scenario issues series] (issue #87).

## Why this is a killer scenario

Most "evolve at runtime" systems are demos. Production systems
in finance, online services, and control systems have:

- **24/7 availability SLAs** — a 100ms hiccup is a Sev-1
- **Audit requirements** — every change must be traceable
- **Compliance gates** — some changes need human approval before deploy
- **State to preserve** — in-flight transactions, cached state, etc.

Aura provides primitives for all four. This doc is the playbook
for assembling them into a production-safe continuous evolution
loop.

## The five-stage production evolution loop

```
        ┌──────────────┐
        │  (1) Observe │
        │  metrics    │
        └──────┬───────┘
               │
               ▼
        ┌──────────────┐
        │  (2) Decide  │   Evolution policy
        │  candidate  │   (E4 evolve-strategy
        │  change     │    + domain heuristics)
        └──────┬───────┘
               │
               ▼
        ┌──────────────┐   (3) Verify off-traffic
        │  Test in    │   - replay recorded traffic
        │  shadow env │   - contract checks (AURA_CONTRACT)
        └──────┬───────┘   - type-check pass
               │          - ownership check (linear types)
               │ pass ↓   pass ↓
        ┌──────┴───────┐
        │  (4) Apply   │   Hot-swap (mutate:rebind + ast:snapshot)
        │  to live    │   or staged rollout (canary per shard)
        │  traffic    │
        └──────┬───────┘
               │
               ▼
        ┌──────────────┐   (5) Monitor + auto-rollback
        │  Watch for  │   p99 latency regression?
        │  regression │   error rate increase?
        │  in prod    │   memory growth?
        └──────┬───────┘   fail ↓
               │              ast:restore
               │ pass
               ▼
            log to
            *audit-log*
```

Each stage has Aura primitives:

| Stage | Primitive |
|---|---|
| Observe | `(gc-arena-info)`, `(memory-pressure)`, custom metrics |
| Decide | `(intend "..." strategy: "evolve-X")` + E4 `evolve-strategy` |
| Test | Aura-level replay harness + `AURA_CONTRACT_PRE/POST` |
| Apply | `mutate:rebind` + `ast:snapshot` or staged rollout |
| Monitor | Periodic re-eval of `(gc-arena-info)` + domain metrics |

## Protocol: Safe evolution in 4 phases

### Phase 0: Pre-flight checks (always run)

```aura
(define (pre-flight target-fn)
  (cond
    ;; Never evolve during known-bad windows
    ((in-trading-hours?)  ; user-defined
     (timeline-record! "evolve-deferred" target-fn "trading hours")
     #f)
    ;; Disk pressure too high?
    ((> (cdr (assq 'overall-pct (memory-pressure))) 90)
     (timeline-record! "evolve-deferred" target-fn "memory pressure")
     #f)
    ;; Too many recent failures (last 100 ops)?
    ((> (recent-failure-rate) 0.05)
     (timeline-record! "evolve-deferred" target-fn "high failure rate")
     #f)
    (else #t)))
```

If pre-flight fails, defer the evolution. Don't force it.

### Phase 1: Shadow test (off-traffic)

```aura
(define (shadow-test target-fn new-body traffic-replay-buffer)
  ;; 1. snapshot current state
  (define snap (ast:snapshot (string-append "shadow:" target-fn)))
  ;; 2. apply the candidate
  (define swap-result (hot-swap:define target-fn new-body))
  (define snap-id (cdr (assq 'snapshot-id swap-result)))
  ;; 3. replay recorded traffic
  (define (replay-one req)
    (define result
      (with-exception-handler
        (lambda (e)
          (timeline-record! "shadow-failure" target-fn req e)
          'error)
        (lambda ()
          (apply (eval target-fn) req)))))
  (define pass-count 0)
  (define fail-count 0)
  (for-each (lambda (req)
    (if (equal? (replay-one req) 'error)
        (set! fail-count (+ fail-count 1))
        (set! pass-count (+ pass-count 1))))
    traffic-replay-buffer)
  ;; 4. rollback (whether or not the shadow passed, we want to
  ;;   return to the original state before the live test)
  (ast:restore snap-id)
  ;; 5. return verdict
  (list 'pass-count pass-count 'fail-count fail-count))
```

The shadow test replays thousands of recorded requests against the
candidate implementation in a sandboxed snapshot. If pass-rate
isn't ≥ baseline, don't promote.

### Phase 2: Canary rollout (1% traffic)

Even after shadow tests pass, deploy the candidate to only 1% of
traffic. Monitor for:

```aura
(define (canary-monitor target-fn baseline-stats canary-stats)
  (let ((p99-delta (- (cdr (assq 'p99 canary-stats))
                       (cdr (assq 'p99 baseline-stats))))
        (err-delta (- (cdr (assq 'error-rate canary-stats))
                      (cdr (assq 'error-rate baseline-stats)))))
    (cond
      ;; Hard fail: p99 +50% or error rate 2x
      ((or (> p99-delta 50) (> err-delta 0.05))
       (timeline-record! "canary-rejected" target-fn p99-delta err-delta)
       #f)
      ;; Soft fail: p99 +20%
      ((> p99-delta 20)
       (timeline-record! "canary-degraded" target-fn p99-delta)
       'continue-with-warning)
      (else 'continue))))
```

### Phase 3: Full rollout

If canary passes for 10 minutes, route 100% of traffic to the
new implementation. Continue monitoring. If p99 regresses, auto-
rollback via `ast:restore` with the snapshot id from `hot-swap:define`.

## Audit trail

Every evolution step writes to `*audit-log*`:

```aura
(define (timeline-record! event target &rest details)
  (set! *audit-log*
        (cons (list (current-time) event target details)
              *audit-log*)))
```

For production, replace `*audit-log*` with a persistent store
(database, log file, Kafka topic). Every mutation must be in the
audit trail with:
- Timestamp
- Event type (evolve-attempted, evolve-succeeded, evolve-rolled-back)
- Target function
- Reason (why this evolution was chosen)
- Snapshot id (for rollback)
- Operator approval (if human-in-the-loop mode)

## Compliance gates

Some industries (finance, medical) require human approval before
certain classes of change. Aura's `*evolution-mode*` already
supports `'ask`:

```aura
(define *evolution-mode* 'ask)

(define (try-evolve target)
  (cond
    ((equal? *evolution-mode* 'autonomous)
     (run-full-protocol target))
    ((equal? *evolution-mode* 'ask)
     ;; Send to operator dashboard
     (send 'operator
          (format "Evolution proposed for ~a. Snapshot ~a. Approve? (y/n)"
                  target (current-snapshot-id)))
     (timeline-record! "evolution-pending" target))))
```

A proper operator UI (`dashboard.aura`) is a follow-up — the
mechanism here is the right shape; the UI is the missing piece.

## Memory safety during evolution

Long-running evolution loops can cause memory pressure. The
double-arena design keeps temp work isolated:

- `temp_arena_` for parse state and intermediate closures
- `arena_` (persistent) for the actual code being executed
- `arena_group_->module_arena(name)` for per-module code

Between evolution steps, call `(gc-temp)` to reclaim the temp
arena without touching persistent state. Use `(set-memory-policy
(hash "auto-gc" #t "critical-pct" 90 "cooldown-evals" 500))` to
let Aura auto-reclaim when the persistent arena hits 90% full.

## State preservation

Some state cannot be reconstructed from a snapshot. For these:

- **Cache invalidation** (`cache-reset` or `cache-promote-hot-keys`):
  schedule cache invalidation as a post-rollout step
- **Connection draining**: for long-lived connections, drain
  in-flight requests to the old code, then route new requests to
  the new code
- **Counter resets**: any monotonic counter should be either
  preserved (read from old state) or reset to a documented baseline

## Rollback strategies

| Rollback type | When | How |
|---|---|---|
| `ast:restore` | Pre-evolution snapshot available | `ast:restore` with the snapshot id |
| `gc-module` | Module-level regression | `gc-module "name.aura"` and reload from versioned source |
| Process restart | Catastrophic failure | Last resort — graceful shutdown, reload snapshot |

The first two are zero-downtime. The third is the documented
disaster-recovery path.

## Comparison vs industry patterns

| Pattern | Used by | Aura primitives |
|---|---|---|
| Blue/green deploy | AWS, Google | N/A (out of scope for runtime) |
| Feature flags | LaunchDarkly, etc | (define *flags* (hash ...)) + Aura-level switch |
| Hot reload | Erlang BEAM | `mutate:rebind` + `ast:snapshot` |
| Canary deploy | Netflix, etc | Pattern 2 above + domain traffic router |
| Safe rollback | Kubernetes, etc | `ast:restore` (zero-downtime) |

Aura combines hot reload + safe rollback + audit + contract checks
in one runtime — that's unique.

## Reference implementations

- `tests/contracts_test.aura` — AURA_CONTRACT usage in evolution
- `tests/multi_session_leak_test.aura` — long-running serve pattern
- `projects/evo-kv/evo-kv-evolve.aura` — full evolve loop (production-grade example)
- `docs/design/self-evolving-infrastructure.md` — broader patterns

## Open questions

- **Concurrent canary at scale**: this doc assumes 1 server.
  Multi-shard production needs shard-level coordination.
- **Cross-version schema migration**: when the data structure
  layout changes (not just a function), can Aura's evolution
  primitives handle that? (Aura has a `data.aura` stdlib that
  has some schema migration primitives, but not full support.)
- **Operator UI**: how does a human see the audit trail and
  approve / reject in real-time? Tracked as `#85` follow-up.
- **Compliance reporting**: how to generate a report that shows
  "every change made in the last 30 days, who approved it,
  and how it was verified" — usually required by regulation.
