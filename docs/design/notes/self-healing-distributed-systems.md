# Self-Healing & Self-Optimizing Distributed Systems

**Status:** Design exploration for Scenario 8 of the
[Scenario issues series] (issue #92).

## Why this is a killer scenario

Traditional distributed systems (Cassandra, Kafka, etcd) are
**manually tuned**: operators read dashboards, write config
changes, do rolling restarts. Self-healing exists (Liveness
probes + restart, leader election on failure) but it's
**reactive** — the system recovers to *known-good* state,
not to *evolved-better* state.

Aura's primitives enable systems that are:

1. **Self-healing** — detect anomalies, mutate the offending
   function, verify, hot-swap
2. **Self-optimizing** — propose better strategies, shadow-
   test, promote cluster-wide
3. **Self-aware of consistency** — every node knows what the
   others are running; quorum-based promotion prevents
   split-brain
4. **Evolution-correct** — `AURA_CONTRACT` gates every
   evolution step; if invariants break, the system rolls
   back

This is the gap the scenario targets.

## Architecture overview

```
                  ┌─────────────────────────────────┐
                  │         Cluster                 │
                  │                                 │
                  │   ┌───────┐   ┌───────┐         │
                  │   │ Node1 │   │ Node2 │   ...   │
                  │   │       │   │       │         │
                  │   │ Evolve│   │ Evolve│         │
                  │   │ Loop  │   │ Loop  │         │
                  │   └───┬───┘   └───┬───┘         │
                  │       │           │             │
                  │   ┌───┴───────────┴───┐         │
                  │   │ Gossip + Quorum   │         │
                  │   │ (consensus on     │         │
                  │   │  strategy version)│         │
                  │   └─────────┬─────────┘         │
                  │             │                   │
                  │   ┌─────────┴─────────┐         │
                  │   │ Shared versioned  │         │
                  │   │ *strategy-store*  │         │
                  │   └───────────────────┘         │
                  │                                 │
                  └─────────────────────────────────┘
```

Every node runs the same Evolve Loop. The Gossip protocol
synchronizes *which strategies* are running. Quorum gates
cluster-wide promotion.

## The per-node Evolve Loop

```
        ┌──────────────────────────────────┐
        │  Observe (local metrics)         │
        │  - p99 latency                   │
        │  - error rate                    │
        │  - memory pressure               │
        │  - gc-temp duration              │
        └────────────────┬─────────────────┘
                         │
                         ▼
        ┌──────────────────────────────────┐
        │  Detect anomaly?                 │
        │  - threshold breach              │
        │  - contract violation            │
        │  - external signal (e.g., page)  │
        └────────────────┬─────────────────┘
                         │ yes
                         ▼
        ┌──────────────────────────────────┐
        │  Self-heal: mutate offending fn  │
        │  1. ast:snapshot "pre-heal"      │
        │  2. propose patch (LLM or rule)  │
        │  3. shadow-test in CaaS          │
        │  4. AURA_CONTRACT_POST verify    │
        │  5. mutate:rebind (local)        │
        │  6. re-measure 60s               │
        │  7. if regress, ast:restore      │
        └────────────────┬─────────────────┘
                         │
                         ▼
        ┌──────────────────────────────────┐
        │  Self-optimize: propose better   │
        │  1. read *local-experience*       │
        │  2. read *cluster-experience*    │
        │     (gossiped aggregate)         │
        │  3. intend "improve X"           │
        │  4. shadow-test in CaaS          │
        │  5. propose to cluster via gossip│
        └────────────────┬─────────────────┘
                         │
                         ▼
        ┌──────────────────────────────────┐
        │  Cluster consensus               │
        │  - quorum accept?                │
        │  - if yes, all nodes hot-swap    │
        │  - if no, fall back to local-best│
        └──────────────────────────────────┘
```

## Pattern 1: Self-healing on contract violation

When `AURA_CONTRACT_POST` fails, the system auto-heals.

```aura
;; Define the contract for the load-balancer's output
(define-contract post:load-balancer-fair
  (assignments server-loads)
  ;; max server load ≤ 2x min server load
  (let ((loads (map cdr server-loads))
        (max (apply max loads))
        (min (apply min loads)))
    (<= max (* 2 min))))

;; When the contract fails (e.g., due to a traffic spike
;; the heuristic didn't anticipate), self-heal:

(define (on-contract-fail target-fn violation)
  (let* ((snap (ast:snapshot (string-append "pre-heal-"
                                             (symbol->string target-fn))))
         (current-code (current-source-of target-fn))
         (diagnosis (pid:analyze target-fn violation))
         (patch-proposal
           (intend
             "The load-balancer violates fairness under skewed
              traffic. Current code: ~a. Diagnosis: ~a. Propose
              a patch."
             (list current-code diagnosis)
             strategy: "evolve-load-balancer"))
         (patch (synthesize:define patch-proposal)))
    (cond
      ((shadow-test target-fn patch)
       (mutate:rebind target-fn patch)
       (gossip-propose 'strategy-change
                       (list 'target target-fn
                             'new-version (version-of patch)
                             'reason 'contract-violation)))
      (else
       (timeline-record! 'heal-attempt-failed
                         target-fn patch)
       (ast:restore snap)))))
```

The cluster-wide healing happens via gossip + quorum (see
Pattern 4 below).

## Pattern 2: Self-optimizing load balancer

```aura
;; E4 tunes the load-balancing strategy
(define-tunable load-balancer
  candidates: (round-robin
               least-connections
               weighted-response-time
               power-of-two-choices
               ;; E4-discovered variants:
               locality-aware
               tenant-affinity)
  metric: (lambda (outcome)
           (- (* 1.0 (cdr (assq 'p99-fairness outcome)))
              (* 0.01 (cdr (assq 'cross-rack-bytes outcome))))))

;; The metric balances fairness against network cost.
;; Power-of-two-choices is good for fair p99 but does
;; cross-rack traffic. Tenant-affinity wins in
;; multi-tenant clusters.
```

The E4 bandit runs on each node with its own local
posterior. Periodically, nodes **gossip their posteriors**
and **merge** them. This is **distributed Bayesian
learning** — the system converges to the globally-best
strategy faster than any single node could.

## Pattern 3: Self-healing replication factor

```aura
;; Replication factor adapts to failure rate
(define-tunable replication-factor
  candidates: (1 2 3 5 7)
  trigger: (or (> (recent-failure-rate) 0.01)
                (and (< (recent-failure-rate) 0.001)
                     (> (read-load) 0.7)))
  metric: (lambda (outcome)
           ;; balance durability against storage cost
           (- (* 1.0 (cdr (assq 'availability outcome)))
              (* 0.1 (cdr (assq 'storage-cost outcome))))))

;; On a healthy cluster, drop RF from 3 to 2 (save storage).
;; On a flaky cluster, bump RF from 3 to 5 (more durability).
```

The `trigger` clause makes this a **reactive** tuner — it
wakes up when failure rate changes, not on a fixed schedule.

## Pattern 4: Gossip + quorum for cluster-wide evolution

This is the critical piece. We need:

- **Gossip protocol** for propagating strategy versions
- **Quorum** for cluster-wide consensus on promotion
- **Versioned snapshots** for safe rollback

```aura
;; Gossip message types
(structure gossip-message
  (type symbol)              ;; 'strategy-propose / 'strategy-accept /
                             ;; 'strategy-reject / 'snapshot-share
  (from-node-id symbol)
  (timestamp integer)
  (proposal proposal))       ;; varies by type

;; Strategy proposal: "node N wants to switch to strategy
;; version V for target T, reason R"
(structure proposal
  (target symbol)
  (new-version integer)
  (reason symbol)
  (expected-metric-delta real)
  (contract-check (cons symbol (list-of any))))

;; Quorum rule
(define (quorum-accept? proposal)
  (let ((accepts (count-votes proposal 'accept))
        (rejects (count-votes proposal 'reject))
        (total (cluster-size)))
    (and (>= accepts (quorum-threshold total))
         (< rejects (- total (quorum-threshold total))))))

(define (quorum-threshold n)
  ;; Strict majority: ⌈n/2⌉ + 1
  (+ (ceiling (/ n 2)) 1))
```

**Flow:**

1. Node A detects a candidate strategy change.
2. Node A gossips a `strategy-propose` to all peers.
3. Each peer runs a **local shadow test** independently.
4. Each peer votes `'accept` or `'reject` (with reason).
5. Node A tallies votes. If quorum-accept?, the change is
   promoted cluster-wide.
6. Each peer hot-swaps to the new version **only after**
   the proposer confirms quorum.
7. If quorum is not reached in `proposal-ttl`, the
   proposal expires; the cluster stays on the current
   version.

This is essentially **Raft for strategies**, with the
"log" being the version chain and the "state machine"
being the running cluster.

## Pattern 5: Self-healing for stuck fibers

Aura's fibers can deadlock. A self-healing cluster detects
this:

```aura
(define (fiber-watchdog-loop)
  (let loop ((ticks 0))
    (sleep 1)  ;; 1 second tick
    ;; Find fibers blocked for > 30s
    (define stuck (filter (lambda (f) (> (fiber-blocked-secs f) 30))
                          (all-fibers)))
    (cond
      ((null? stuck) (loop (+ ticks 1)))
      (else
        (for-each
          (lambda (f)
            (timeline-record! 'fiber-stuck
                              (fiber-id f)
                              (fiber-blocked-on f))
            (cond
              ;; Stuck on a missing message? Resend.
              ((channel-stuck-on-closed-msg? f)
               (fiber-cancel f)
               (fiber:spawn (retry-task (fiber-task f))))
              ;; Stuck on a slow peer? Notify the peer.
              ((fiber-blocked-on-remote-call? f)
               (send 'peer-watchdog
                     (list 'stuck-call
                           (fiber-id f)
                           (fiber-blocked-on f))))
              (else
                ;; Last resort: cancel and log
                (fiber-cancel f))))
          stuck)
        (loop (+ ticks 1))))))
```

The watchdog runs as a background fiber on every node.
Aura's `fiber:spawn` + `fiber:join` (added in `72c9559`)
make this tractable.

## Pattern 6: Self-optimizing consensus parameters

The consensus protocol itself has tunables: heartbeat
interval, election timeout, batch size, snapshot interval.

```aura
(define-tunable raft-params
  candidates: ((heartbeat 50  election 300  batch 64)
               (heartbeat 100 election 500  batch 128)
               (heartbeat 200 election 1000 batch 256)
               ;; discovered by E4:
               (heartbeat 75  election 400  batch 96)))
```

E4 tunes these per cluster (latency-sensitive cluster
prefers `50/300/64`; throughput-sensitive prefers
`200/1000/256`). The bandit learns this once and the
cluster converges.

## Memory safety under evolution

Long-running distributed systems accumulate state. Aura's
double-arena + `gc-temp` keep this in check:

- `temp_arena_` for inter-node messages (cleared per batch)
- `arena_` for persistent state (GC'd per `set-memory-policy`
  threshold)
- `pmr` allocators prevent fragmentation across the
  long-running process

For a self-healing cluster, the **memory footprint under
evolution is bounded** — the system never grows
unboundedly while exploring strategy variants.

## Safety invariants

Every cluster-wide evolution respects three invariants:

1. **At-most-one-version-active** — at any moment, all
   nodes run the same strategy version (no split-brain)
2. **Snapshot-before-swap** — every swap has a rollback
   path via `ast:restore`
3. **Contract-gated promotion** — no promotion without
   `AURA_CONTRACT_POST` passing on quorum-many nodes

These are encoded as Aura contracts:

```aura
(define-contract post:cluster-evolution
  (cluster-state proposal)
  (and (at-most-one-version-active? cluster-state)
       (snapshot-exists? (cdr (assq 'pre-snapshot proposal)))
       (>= (count-passes proposal 'contract-check)
           (quorum-threshold (cluster-size)))))
```

If any invariant breaks, the system auto-rolls back the
cluster to the last known-good state.

## Reference implementations

- `projects/evo-kv/evo-kv-pubsub.aura` — pub/sub for
  gossip messages
- `projects/evo-kv/evo-kv-core.aura` — has tunable load
  balancing
- `docs/design/concurrency_model.md` — fiber + scheduler
  primitives
- `docs/design/concurrent_channels.md` — CSP channels
- `docs/design/inter_agent_messaging.md` — agent messaging
  patterns

## Industry comparison

| System | Self-healing | Self-optimizing | Cluster-wide evolution |
|---|---|---|---|
| Kubernetes | Yes (restart) | No (HPA via metrics) | No (manual apply) |
| Cassandra | Yes (Gossip) | No (manual config) | No (rolling restart) |
| etcd / Raft | Yes (leader election) | No (fixed protocol) | No (consensus on data only) |
| AWS Auto Scaling | Yes (replace) | No (scaling rules) | N/A |
| TensorFlow distributed | No (manual) | No | No |
| **Aura distributed** | **Yes (contract + auto-heal)** | **Yes (E4 bandit + CaaS)** | **Yes (gossip + quorum)** |

Aura is the only system where **strategy improvements**
(not just data) propagate cluster-wide with **formal
safety contracts** at every step.

## Open follow-ups (not blocking this issue)

- **Byzantine fault tolerance** — what if a node lies
  about its shadow-test result? Aura's gossip should
  cross-verify results; cryptographic signing is a
  follow-up.
- **Cross-cluster evolution** — this doc assumes one
  cluster. Multi-cluster (geo-distributed) evolution is
  open research.
- **Drift detection** — when a strategy evolves on one
  node and gossip lags, the cluster has brief
  inconsistency. Bounded staleness contracts are a
  follow-up.
- **Operator override** — operators need a UI to force-
  pin a strategy, reject an evolution, or roll back
  manually. Tracked as follow-up to `#85` (operator UI).
