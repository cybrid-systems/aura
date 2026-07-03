# SEVA Demo — Self-Evolving Verification Agent

## Architecture overview

SEVA is structured as 4 layers:

1. **Setup layer** — `set-code` loads the DUT spec into
   the workspace; `eval-current` lowers it to IR.
2. **Feedback layer** — `verify:parse-coverage-feedback`
   + `verify:parse-assert-failure` parse the iverilog
   output and mark AST nodes dirty.
3. **Strategy layer** — the strategy evolution controller
   (#444) picks the next mutation strategy based on
   pheromone + verify-dirty state.
4. **Mutation layer** — `mutate:replace-pattern` (or
   `mutate:set-body` for closures) applies the fix; the
   audit log records the result.

The 4 layers are connected by `query:*` primitives for
observability + `mutate:*` for state changes. The
agent's main loop reads the audit log + verify-dirty
state to decide what to do next.

## What is SEVA?

SEVA is the flagship Aura demo: a self-evolving verification
agent that drives a hardware verification closed-loop
using Aura's mutation + query primitives.

The loop:

```
   ┌──────────────────────────────────────────────────────────────┐
   │                                                              │
   │   ┌────────────────┐         ┌────────────────┐              │
   │   │  set-code      │ ──────► │  eval-current  │              │
   │   │  (load DUT)    │         │  (lower IR)    │              │
   │   └────────────────┘         └────────────────┘              │
   │                                     │                        │
   │                                     ▼                        │
   │   ┌────────────────────────────────────────────┐              │
   │   │  verify:parse-coverage-feedback            │              │
   │   │  (iverilog-style output → dirty nodes)    │              │
   │   └────────────────────────────────────────────┘              │
   │                                     │                        │
   │                                     ▼                        │
   │   ┌────────────────────────────────────────────┐              │
   │   │  query:verify-dirty-stats                 │              │
   │   │  (read verify-dirty counters)            │              │
   │   └────────────────────────────────────────────┘              │
   │                                     │                        │
   │                                     ▼                        │
   │   ┌────────────────────────────────────────────┐              │
   │   │  strategy evolution controller            │              │
   │   │  (pick next mutation strategy)            │              │
   │   │                                          │              │
   │   │  coverage-greedy → bug-fix-priority →     │              │
   │   │  minimal-mutation → escalate-back-to-…   │              │
   │   └────────────────────────────────────────────┘              │
   │                                     │                        │
   │                                     ▼                        │
   │   ┌────────────────┐         ┌────────────────┐              │
   │   │  mutate:replace│ ──────► │  query:seva-   │              │
   │   │  pattern       │         │  audit-log     │              │
   │   └────────────────┘         └────────────────┘              │
   │                                     │                        │
   │                                     ▼                        │
   │                                     └──── (loop back) ───────┘
   │
   └──────────────────────────────────────────────────────────────┘
```

The agent decides which strategy to apply next based on
the verify-dirty state and the pheromone counters
(`query:strategy-evolution-stats`). When coverage
plateaus, it escalates from random testing to directed
testing. When a bug-fix succeeds, it bumps the strategy's
success counter.

## Files

| File | Purpose |
|---|---|
| `demos/seva/fifo_dut.aura` | Minimal synchronous FIFO DUT spec as Aura data |
| `demos/seva/seva_demo.aura` | Main demo script that runs the loop |
| `demos/seva/README.md` | Demo docs |
| `demos/seva/openclaw-skill/seva_skill.py` | Example OpenClaw skill driving the demo |
| `demos/seva/TUTORIAL.md` | Step-by-step walkthrough |

## How to run

```bash
# Build (one-time)
./build.py build

# Run the demo
cat demos/seva/seva_demo.aura | ./build/aura

# Drive it from an external agent (OpenClaw skill)
python3 demos/seva/openclaw-skill/seva_skill.py "Achieve 95% coverage on FIFO"
```

## Key Aura primitives used

### Verification primitives (Issue #437, #469)
- `(verify:assertion-failed node-id)` — mark node with assertion-failure bit
- `(verify:report-coverage node-id)` — mark node with coverage-hole bit
- `(verify:parse-coverage-feedback text)` — parse iverilog output, mark dirty
- `(verify:parse-assert-failure text)` — parse assertion output, mark dirty
- `(query:verify-dirty-stats)` — aggregate verify-dirty counters

### Mutation primitives
- `(mutate:replace-pattern source replacement)` — apply a mutation
- `(mutate:atomic-batch ...)` — rollback-safe batch mutations
- `(mutate:set-body name new-body)` — replace function body

### Query primitives
- `(query:filter predicate ...)` — query nodes matching predicates
- `(query:by-marker marker)` — query by syntax marker
- `(query:verify-dirty-stats)` — verify-dirty aggregate

### EDSL readiness (Issue #440)
- `(query:edsl-readiness)` — single hash for EDSL production-readiness
- `(query:soa-dirty-stats)` — live SoA dirty state

### Strategy evolution (Issue #444)
- `(strategy:set-strategy name)` — select active strategy
- `(strategy:report-success name)` — bump success counter
- `(strategy:escalate reason)` — bump escalation counter
- `(query:strategy-evolution-stats)` — pheromone state

### SEVA goals (Issue #445)
- `(seva:achieve-coverage name target-pct)` — coverage goal
- `(seva:fix-reset-bugs)` — reset-related bug targets
- `(seva:generate-regression)` — emit regression script
- `(seva:approve-mutation id flag)` — safety gate
- `(query:seva-audit-log)` — agent audit trail

## Self-evolution: the "why"

The agent improves its own verification strategy over
time. After 100+ AI multi-round mutations, the strategy
controller accumulates pheromone:

- `coverage-greedy` strategy — tried first; if it stops
  improving coverage, the controller escalates.
- `bug-fix-priority` strategy — chosen when coverage
  plateaus but assertion failures remain.
- `minimal-mutation` strategy — chosen when both are
  saturated; the agent tries the smallest change that
  might help.

The escalation is rule-based for the MVP; a PID
controller + pheromone accumulation is a follow-up
issue. The point: the agent doesn't just run the loop
once — it adapts to what works.

## What this demonstrates

The SEVA demo shows Aura can deliver a closed-loop
verification agent that:

1. **Loads a DUT spec** (no SystemVerilog — the demo uses
   Aura data structures for the MVP)
2. **Feeds coverage feedback** (mock coverage strings
   stand in for iverilog output)
3. **Reads verify-dirty state** (what nodes are unfixed)
4. **Picks a mutation strategy** (greedy / bug-fix /
   minimal)
5. **Applies the mutation** (mutate:replace-pattern)
6. **Records the result** (audit log)
7. **Loops until coverage target is met**

This is L4 capability (an agent that can drive a
verification loop). L5 capability (full autonomy
including testbench generation + SVA synthesis) is
tracked as separate issues.

## Metrics collected

The demo's `seva:run-demo-with-metrics` primitive (or
the manual `query:*` calls in `seva_demo.aura`)
emits:

- `iterations-to-closure` (number of loop iterations
  until coverage target met)
- `coverage-improvement-per-mutation` (delta of
  verify-dirty per mutation)
- `human-intervention-count` (should be 0 after
  initial set-code)
- `mutation-success-rate` (successes / attempts)
- `time-breakdown` (query vs mutate vs sim time —
  not exposed in MVP, but the primitive calls return
  monotonic timestamps if needed)

## Tests

```bash
./build/test_issue_442_seva_demo    # demo structure (17 tests)
./build/test_issue_444_strategy_evolution  # controller (22 tests)
./build/test_issue_445_openclaw_integration  # goal primitives (19 tests)
```

## Related issues

- #442 — SEVA Demo scaffolding (this demo)
- #444 — Strategy evolution controller (the strategy loop)
- #445 — OpenClaw integration (the agent interface)
- #437 — Verification feedback primitives
- #469 — Coverage/assertion parsers

## Future work

The full SEVA roadmap includes:
- Real iverilog integration (subprocess + parse stdout)
- Testbench generation (mutate:add-coverpoint primitive)
- SVA generation (auto-generate SVA from coverage holes)
- Cross-spec mutation (structural mutation primitive)
- Real iverilog-backed coverage feedback loop
- Persistent human approval UI for high-impact mutations
- L5: full autonomous bug-fix loop (agent decides what
  to mutate without human intervention)