# Agent Orchestration Evolution — Coordination Logic That Improves

**Status:** Design exploration for Scenario 5 of the
[Scenario issues series] (issue #89).

## Why this is a killer scenario

Traditional multi-agent systems (LangGraph, CrewAI, AutoGen)
ship with **fixed orchestration**: the pipeline is a DAG the
developer drew, and agents fit into the slots. To improve it,
you edit code, redeploy, restart.

Aura flips this. The **orchestration layer itself becomes
evolvable**:

1. Observe which coordination patterns work for which task
   shapes
2. Propose new patterns (e.g., "try a back-off retry on the
   reviewer before failing the whole pipeline")
3. Hot-swap the new pattern into a live system
4. Measure whether tasks complete faster / cheaper / better
5. Keep the winner, update the bandit posterior

The result: a platform where **the way agents work together
improves with use**, not with version bumps.

## What's already on `main` (and what's new in this doc)

| Doc | Scope | This doc adds |
|---|---|---|
| `agent_orchestration.md` | Static primitives (`spawn`, `conduct`, `pipeline`, `parallel`) | — |
| `inter_agent_messaging.md` | Channel + message-passing | — |
| `intent_orchestration.md` | `intend` primitive + E4 strategy | — |
| `autonomous-self-evolving-agents.md` | Agents self-tune tool selection / repair | Agent-level evolution |
| **`agent-orchestration-evolution.md`** (this) | — | **Coordination-pattern evolution** |

The new thing is: **the pattern that decides which agent
runs when, in which order, with what fallbacks, and with
what conflict resolution — that whole structure is data, not
code, and Aura's E4 strategies can evolve it.**

## Coordination pattern as data

A coordination pattern is a record:

```aura
(record coordination-pattern
  (name symbol)
  (stages (list-of stage))              ;; ordered or DAG
  (fallbacks (alist-of stage → action)) ;; on stage failure
  (conflict-resolution procedure)       ;; on agent disagreement
  (timeout-ms integer)
  (parallelism-cap integer)
  (history (list-of (run-id stats))))   ;; filled by E4

(record stage
  (agent-name symbol)
  (input-from (list-of stage-id))
  (retry-policy policy)
  (timeout-ms integer))
```

This is what gets stored in `*coordination-pool*` and what
E4 mutates:

```aura
(define *coordination-pool*
  (hash
    "default"      (load-pattern "default.aura")
    "research"     (load-pattern "research.aura")
    "code-review"  (load-pattern "code-review.aura")
    ;; ... more patterns discovered at runtime
    ))
```

## The orchestration evolution loop

```
            ┌─────────────────────────────────────┐
            │  Task arrives (intend / pipeline)   │
            └────────────────┬────────────────────┘
                             │
                             ▼
            ┌─────────────────────────────────────┐
            │  Pick pattern from pool (E4 bandit) │
            │  - posterior over (latency, cost,   │
            │    quality, conflict-rate)          │
            │  - UCB or Thompson sampling         │
            └────────────────┬────────────────────┘
                             │ chosen: pattern X
                             ▼
            ┌─────────────────────────────────────┐
            │  Execute pattern                    │
            │  - spawn agents per stage           │
            │  - channels for messaging           │
            │  - monitor for: timeout, conflict,  │
            │    exception, deadlock              │
            └────────────────┬────────────────────┘
                             │ outcomes
                             ▼
            ┌─────────────────────────────────────┐
            │  Update bandit + write history      │
            │  - success: posterior += reward     │
            │  - failure: posterior += penalty    │
            │  - log to *coordination-history*    │
            └────────────────┬────────────────────┘
                             │
                             ▼
            ┌─────────────────────────────────────┐
            │  Propose mutation (E4 LLM)          │
            │  "pattern X failed 5x in row on      │
            │   tasks with parallel-reviews.       │
            │   Try: serialize the review stage    │
            │   before merge."                    │
            │  → new candidate pattern X'         │
            └────────────────┬────────────────────┘
                             │
                             ▼
            ┌─────────────────────────────────────┐
            │  Shadow test X' (off-traffic)       │
            │  - replay recent task batch         │
            │  - measure X' vs X                  │
            │  - promote if pass-rate ≥ baseline  │
            └────────────────┬────────────────────┘
                             │
                             ▼
            ┌─────────────────────────────────────┐
            │  Promote (hot-swap) or discard      │
            │  - ast:snapshot before              │
            │  - mutate:rebind coordination-pool  │
            │  - AURA_CONTRACT_POST validates     │
            │    pattern X' has same shape as X   │
            └─────────────────────────────────────┘
```

## Pattern 1: Conflict resolution evolution

Agents disagree. The current resolution is "highest-priority
wins" (a hand-tuned property on each agent). This is bad
because:

- "Priority" is a static label
- Real disagreements are content-dependent
- A pattern that always defers to the reviewer is bad when
  the reviewer is wrong (e.g., nit-picking style over
  substance)

Evolve the conflict resolution:

```aura
(define-tunable conflict-resolver
  candidates: (priority-wins
               reviewer-defers
               majority-vote
               llm-arbitrator
               abstain-and-escalate)
  metric: (lambda (outcome) (cdr (assq 'quality outcome))))

;; After 100 tasks, the bandit learns:
;;   - On "code review" tasks: llm-arbitrator wins
;;   - On "factual Q&A" tasks: majority-vote wins
;;   - On "ethical" tasks: abstain-and-escalate wins
```

The interesting bit: the bandit is **task-shape conditional**.
The same conflict-resolver pattern is selected differently
per task. Aura's E4 supports this via `(intend goal strategy:
"evolve-conflict-resolver" context: task-shape)`.

## Pattern 2: Decomposition strategy evolution

"How do you break a goal into sub-tasks?" The current default
is sequential with a planner agent. But for some goals,
parallel decomposition works better; for others, hierarchical
(planner-of-planners).

```aura
(define-tunable decomposer
  candidates: (sequential-planner
               parallel-fanout
               hierarchical-planner-of-planners
               auction-based
               ;; discovered by E4:
               two-pass-with-reviewer-checkpoint)
  metric: (lambda (outcome)
           (- (* 1.0 (cdr (assq 'success outcome)))
              (* 0.01 (cdr (assq 'cost outcome))))))
```

For a 5-minute coding task, sequential wins. For a 10-doc
research task, parallel-fanout wins. The bandit learns
this mapping and the platform self-tunes per task.

## Pattern 3: Retry and backoff evolution

The current retry policy is fixed: 3 retries, exponential
backoff, then fail. Evolve it:

```aura
(define-tunable retry-policy
  candidates: (linear-backoff
               exponential-backoff
               jittered-exponential
               circuit-breaker
               adaptive-from-history)
  metric: (lambda (outcome)
           (cond
             ((equal? (cdr (assq 'result outcome)) 'success) 1.0)
             ((equal? (cdr (assq 'result outcome)) 'timeout) -0.5)
             (else 0.0))))
```

After running, the bandit discovers: "for tasks where the
first attempt fails with timeout, circuit-breaker is the
right call (don't hammer a dead agent). For tasks where
the first attempt fails with `error: cannot parse input`,
linear-backoff is fine (the input is the same, retries
help)."

## Pattern 4: Communication pattern evolution

How do agents talk? Current default: shared bus. Evolve to:

- pub-sub (broadcast with filters)
- request-reply (point-to-point)
- blackboard (shared memory with locking)
- gossip (eventually consistent)

```aura
(define-tunable comm-pattern
  candidates: (shared-bus
               pub-sub
               blackboard
               gossip
               hybrid-bus-plus-blackboard)
  trigger: (or (new-agent-joined) (comm-saturation?))
  metric: (lambda (outcome) (cdr (assq 'throughput outcome))))
```

The bandit picks based on agent count and message rate.
For 2-3 agents, shared-bus is fine. For 10+ agents, hybrid
wins. The platform self-tunes as the swarm grows.

## Conflict-of-interest handling

Two patterns can disagree about who runs first. Resolution:

- **Bandit vote**: each pattern votes with its posterior weight.
  Highest-weight pattern wins for this task.
- **Hierarchical**: a meta-bandit picks the sub-bandit for this
  task class.
- **Escalation**: if both bands are within 1σ of each other,
  run **both** in parallel and let the human or downstream
  consumer pick. (Aura's `parallel` primitive handles this.)

The first is the default; the others are configuration.

## Safety: never evolve during a task in flight

A coordination pattern is hot-swapped **between** tasks, not
**during** one. The runtime enforces this via:

```aura
;; AURA_CONTRACT_PRE: pattern hot-swap
(define-contract pre:coordination-swap
  (old-pattern new-pattern)
  (and (not (any-task-in-flight?))
       (same-shape? old-pattern new-pattern)
       (valid-stages? new-pattern)))
```

If a task is in flight, the swap is **queued** and applied at
task boundary. The bandit never races with itself.

## Memory: the coordination history is valuable

Every pattern execution writes to `*coordination-history*`:

```aura
(structure coordination-record
  (run-id integer)
  (pattern-name symbol)
  (task-shape hash)
  (agent-outcomes (list-of hash))
  (final-result symbol)
  (latency-ms integer)
  (cost-tokens integer)
  (conflicts-resolved integer)
  (rollbacks integer)
  (timestamp integer))
```

This is the training data for future bandit updates. Aura
keeps it in a `pmr::vector` in `temp_arena_`, with the last
10K records as the active learning set. Older records are
compacted to a `*coordination-history-archive*` for batch
re-learning.

## Metrics that matter

For an evolving orchestration platform, the meta-metrics are:

- **Time-to-solution** (vs. fixed-orchestration baseline)
- **Cost-per-solution** (tokens / compute)
- **Quality** (pass-rate, human-rating, downstream-test-result)
- **Conflict-rate** (how often agents disagree)
- **Rollback-rate** (how often E4's proposed pattern is rejected)
- **Bandit convergence** (how quickly posterior collapses to
  one winner per task class)

The first three are user-facing. The last three are
operator-facing — they tell you whether the system is
learning sanely or thrashing.

## Reference implementations

- `projects/chat/` — basic multi-agent chat with a conductor
  agent (foundation)
- `tests/fiber_join_test.aura` — `fiber:join` used to coordinate
  parallel stages
- `projects/evo-kv/evo-kv-aof.aura` — AOF rewrite pipeline
  uses parallel decomposition
- `docs/design/autonomous-self-evolving-agents.md` — pattern
  catalog at the agent level (companion doc)

## Comparison vs other multi-agent frameworks

| Framework | Orchestration | Evolves? | Hot-swap? |
|---|---|---|---|
| LangGraph | DAG (Python) | No (edit code) | No (restart) |
| CrewAI | Role-based, fixed | No | No |
| AutoGen | Conversation-based, fixed | No | No |
| Apache Airflow | DAG, declarative | No (DAG changes require deploy) | No |
| Temporal.io | Workflow + activity, durable | No (code changes require new version) | Limited (versioning) |
| **Aura** | Pattern pool, E4 bandit | **Yes** (bandit + LLM propose) | **Yes** (mutate:rebind) |

Aura is the only system where the **orchestration logic
itself** is a first-class evolvable entity. This is the gap
the scenario targets.

## Open follow-ups (not blocking this issue)

- **Cross-pattern composition** — can a successful sub-pattern
  from pattern A be reused in pattern B? E4 synthesis research.
- **Human-in-the-loop override** — when the bandit is uncertain,
  escalate to a human. Tracked as follow-up to `#85` (operator
  UI).
- **Cold-start** — for a new task class, the bandit has no data.
  The default is the most general pattern (sequential). After
  ~30 tasks, the bandit starts to differentiate. Documented in
  the cold-start subsection of `e4_evolvable_strategies.md`.
- **Swarm-scale testing** — this doc assumes 2-20 agents. The
  patterns for 100-1000 agents are open research.
