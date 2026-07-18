# Validate intend ClosureIds before apply (#1719)

**Issue:** [#1719](https://github.com/cybrid-systems/aura/issues/1719)  
**Files:** `evaluator_primitives_agent.cpp`, `observability_metrics.h`  
**Status:** P1 UAF — intend apply_closure without live check (#1713 sibling).

## Problem

`intend` called `apply_closure` on generator/verifier/fixer ClosureIds
without verifying they were still live after free.

## Fix

Shared helpers (also used by auto-evolve #1713):

- `agent_cid_live(ev, cid)` — TW `find_active_closure` or JIT exists+!freed  
- `agent_note_closure_freed_call` → `agent_closure_freed_during_call`

`intend` `call_fn` refuses freed cids, records timeline, returns empty.

Also gated parallel-orch apply and `orch:spawn-agent` thunk apply.

## Tests

`tests/test_intend_closure_live_1719.cpp`
