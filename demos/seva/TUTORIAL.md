# SEVA Tutorial — A Full Walkthrough

This tutorial walks through one successful run of the
SEVA demo, step by step, with the actual output you
should expect.

## Prerequisites

```bash
./build.py build          # builds aura + tests
```

## The run

```bash
cat demos/seva/seva_demo.aura | ./build/aura
```

## Expected output

```
═══ SEVA Demo: self-evolving verification agent ═══
DUT spec loaded: 5 properties

Step 1: set-code + eval-current → workspace loaded
Step 2: verify:parse-coverage-feedback → 3 nodes marked
Step 3: verify:parse-assert-failure → 0 nodes marked
Step 4: query:verify-dirty-stats → 0 total dirty nodes

Step 5: query:edsl-readiness →
  closure-stale-refresh     = 0
  atomic-batch-commits      = 0
  dirty-block-rate          = 0%
Step 6: mutate:replace-pattern → #t

Step 7: post-mutation readiness →
  atomic-batch-commits      = 0
═══ Demo complete ═══
Run with: ./build/aura demos/seva/seva_demo.aura
Step 8: strategy evolution loop
  iter 1: coverage-greedy (1 hit, 1 success)
  iter 2: bug-fix-priority (1 hit, 1 success)
  iter 3: minimal-mutation (1 hit, 0 success)
  iter 4: escalate → back to bug-fix-priority
Step 9: query:strategy-evolution-stats →
  active-strategy     = minimal-mutation
  greedy-successes    = 1
  bugfix-successes    = 2
  minimal-successes   = 0
  escalations         = 1
```

## What's happening

### Steps 1-2: Setup + coverage parse

The demo loads a FIFO DUT spec into the workspace via
`(set-code ...)` then lowers it via `(eval-current)`.
The mock coverage feedback string ("3 nodes flagged
for missing reset") is parsed by
`(verify:parse-coverage-feedback)`. Each line in the
string is a NodeId + human-readable hole name.

### Step 3-4: Assertion parse + verify-dirty aggregate

The mock assertion failures are parsed similarly. The
`query:verify-dirty-stats` aggregates all four dirty
categories (assertion / coverage / sva / formal-cex).

### Steps 5-6: EDSL readiness + first mutation

`query:edsl-readiness` returns 6 fields covering the
top EDSL production-readiness signals. The first
mutation is applied via `mutate:replace-pattern` —
turning `(missing-reset-bug . #t)` into
`(missing-reset-bug . #f)` (the demo's known bug is
fixed).

### Steps 7-9: Strategy evolution + pheromone

The strategy controller runs 4 iterations:
1. `coverage-greedy` strategy tried — 1 success
2. `bug-fix-priority` strategy tried — 1 success
3. `minimal-mutation` strategy tried — 0 successes
4. Escalation triggered — back to `bug-fix-priority`

The `query:strategy-evolution-stats` hash shows the
final state: 1 greedy success, 2 bug-fix successes,
0 minimal successes, 1 escalation.

## Variations to try

### Run with OpenClaw skill

```bash
# In one terminal
cat demos/seva/seva_demo.aura | ./build/aura

# In another terminal
python3 demos/seva/openclaw-skill/seva_skill.py \
  "Achieve 95% coverage on FIFO"
```

### Run with metrics

```bash
# TBD: seva:run-demo-with-metrics primitive is a follow-up
# — for now, query:seva-audit-log + query:strategy-evolution-stats
# are the metrics primitives.
```

### Run with a different DUT spec

Edit `demos/seva/fifo_dut.aura` (or define your own
spec inline in the demo) — the loop works on any
Aura data structure.

## Common issues

### "unbound variable: foo"

The REPL doesn't auto-load `.aura` files. Use
`set-code` to install code:

```bash
echo '(set-code "(define (f x) (+ x 1))")(eval-current)(display (f 41))' \
  | ./build/aura
```

### "no workspace"

`(set-code ...)` not called. Call it first.

### "bad-arg"

A primitive got the wrong type. Check the docstring
of the primitive in `docs/demos/seva.md`.

## Next steps

- Read `docs/demos/seva.md` for the architecture diagram
- Read `docs/generated/primitives.md` for the full
  primitive reference
- Run `./build/test_issue_442_seva_demo` to see the
  structure assertions
- Run `./build/test_issue_445_openclaw_integration`
  to see the OpenClaw integration tests