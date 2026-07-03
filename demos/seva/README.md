# SEVA Demo — Self-Evolving Verification Agent

End-to-end scaffolding that shows Aura powering an
agent-driven verification loop. The demo loads a
minimal synchronous FIFO DUT, feeds mock coverage
feedback (would be iverilog output in production),
queries the verify-dirty state, mutates the spec to
fix the bug, and re-verifies.

## Quick start

```bash
./build/aura demos/seva/seva_demo.aura
```

The demo prints:

```
═══ SEVA Demo: self-evolving verification agent ═══
DUT spec loaded: 5 properties

Step 1: set-code + eval-current → workspace loaded
Step 2: verify:parse-coverage-feedback → 3 nodes marked
Step 3: verify:parse-assert-failure → 2 nodes marked
Step 4: query:verify-dirty-stats → 5 total dirty nodes

Step 5: query:edsl-readiness →
  closure-stale-refresh     = ...
  atomic-batch-commits      = ...
  dirty-block-rate          = ...%

Step 6: mutate:replace-pattern → ...

Step 7: post-mutation readiness →
  atomic-batch-commits      = ...

═══ Demo complete ═══
Run with: ./build/aura demos/seva/seva_demo.aura
```

## Files

- `fifo_dut.aura` — minimal synchronous FIFO spec as
  Aura data (8-bit wide, 4-deep, active-low reset).
- `seva_demo.aura` — main demo script driving the loop.

## Primitives exercised

| Primitive | Issue | Purpose |
|---|---|---|
| `verify:parse-coverage-feedback` | #469 | parse iverilog-style coverage output |
| `verify:parse-assert-failure` | #469 | parse assertion failures |
| `query:verify-dirty-stats` | #437 | aggregate verify-dirty counters |
| `mutate:replace-pattern` | existing | apply a spec fix |
| `query:edsl-readiness` | #440 | EDSL production-readiness aggregator |

## What this demonstrates

The "self-evolution" loop is the core pattern: an
agent reads verification feedback → identifies
coverage gaps → mutates the spec → re-verifies →
loops until coverage converges. The Aura primitives
listed above are the building blocks; this demo
shows them wired together in a runnable script.

## What's NOT in this MVP

- Actual iverilog integration (the demo uses mock
  coverage strings — production would pipe real
  iverilog output into `verify:parse-coverage-feedback`)
- Testbench generation (the demo's "mutate" step
  modifies the spec data, not a generated .sv file)
- SVA / formal verification (the verify-dirty
  counters cover SVA but the demo doesn't use them)

## Tests

```bash
./build/test_issue_442_seva_demo
```

Tests verify:
- AC1: fifo_dut.aura exists + has the spec
- AC2: seva_demo.aura exists
- AC3-AC6: demo uses the documented primitives
- AC7-AC8: fifo spec details (bug + flags)

## Follow-ups

1. Real iverilog integration (subprocess + parse stdout
   into verify:parse-coverage-feedback).
2. Testbench generation — the agent should be able to
   auto-generate a SystemVerilog testbench from the spec
   + coverage gaps (separate issue).
3. SVA generation — when coverage holes point to SVA
   candidates, auto-generate the SVA.
4. Cross-spec mutation — currently mutate:replace-pattern
   operates on string patterns; a structural mutation
   primitive (mutate:add-coverpoint) would be cleaner.
5. Documentation in docs/demos/seva.md with screenshots
   of successful runs (deferred to a separate docs issue).