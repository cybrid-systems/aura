# Cap evolve-strategy name-collision loop (#1726)

**Issue:** [#1726](https://github.com/cybrid-systems/aura/issues/1726)  
**Files:** `evaluator_primitives_agent.cpp`, `observability_metrics.h`  
**Status:** P2 correctness — unbounded `for (;;)` name-bump loop.

## Problem

`evolve-strategy` avoided collisions with `for (int bump = 2;; ++bump)`.
If every `base` / `base-N` name was occupied, the loop never terminated
(O(n²) per attempt over `strategies_`).

## Fix

1. Cap attempts at `kMaxNameCollisions = 10000`.
2. On exhaustion: bump `agent_evolve_name_collision_exhausted`, return
   `make_void()` (caller-visible failure, no hang).
3. Keep shared unique lock on `strategies_mtx_` for the insert path.

## Tests

`tests/test_evolve_name_collision_1726.cpp`
