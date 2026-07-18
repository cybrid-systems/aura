# Remove auto-evolve-tick debug fprintf (#1712)

**Issue:** [#1712](https://github.com/cybrid-systems/aura/issues/1712)  
**Files:** `evaluator_primitives_agent.cpp`  
**Status:** P2 perf — debug `fprintf(stderr)` on every production tick.

## Problem

`auto-evolve-tick` printed `[DBG tick]`, `no detect result`, and
`detect.val=...` to stderr every cycle — log pollution, stdio lock,
possible pipe backpressure.

## Fix (Option A)

Remove the three debug `fprintf` calls. Hot path logic unchanged.

## Tests

`tests/test_auto_evolve_tick_no_dbg_1712.cpp`
