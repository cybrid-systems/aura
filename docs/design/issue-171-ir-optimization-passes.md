# High-Impact IR Optimization Passes — Design (Issue #171)

**Status:** All 2 remaining passes shipped (2026-06-13, 1 session, 2 commits)
**Date:** 2026-06-13
**Workstream:** 3 of #143 (IR optimization passes)

## Scope (per issue body)

| Priority | Pass | Status | Commit | Notes |
|---|---|---|---|---|
| 1 | Escape Analysis | ✅ shipped (pre-existing) | (pre-#171) | Existed before this issue |
| 2 | Function Inliner | ✅ shipped | 5d632e9 | actually inlines, not just counts |
| 3 | TCO | ✅ shipped | 2ca98ce | tail Call+Return → Jump |

## Detailed scope for each pass

### Priority 2: Function Inliner (5d632e9)

Extends the #160 scaffold (InlinePass only counted call
sites). The minimal shippable piece: inlines single-block
callees that return a constant.

**What it does**:
- Data-flow: build slot → func_id map for the caller
  (MakeClosure writes func_id to a slot; Call reads it).
- For each Call, look up the static callee via the slot map.
- If the callee is "trivial-inlinable" (single block + Const
  + Return, with the Return's source = the Const's result),
  rewrite the Call instruction in place to be the ConstXxx.
- Recursion guard: skip when callee_fid == caller_fid.
- Idempotent: running twice is a no-op.

**Bugs caught during test development**:
- Initial implementation preserved operands[1] and [2] from
  the Call, but those hold arg_base / arg_count, not the
  const value. Fix: copy them from the callee's const.

**Limitations** (deferred to follow-ups):
- Multi-block callees (branch-aware inlining)
- Functions with parameters (local renaming)
- Indirect recursion detection
- Call site cloning for polymorphic dispatch

### Priority 3: TCO (2ca98ce)

The TCOPass brings Tail Call Optimization to the IR level.
The evaluator's tree-walking TCO already exists
(evaluator_impl.cpp:16786+); this adds it to the IR so the
JIT can benefit.

**Pattern detected**:
  Block ends with:
    Call(callee_slot, arg_base=0, arg_count, result_slot)
    Return(result_slot)
  Where callee_slot was set by MakeClosure(func_id)
  earlier in the same function.

**Transformation**:
- arg_base == 0: args are already in the callee's param
  slots (0..arg_count-1). No Local copies needed.
- Replace Call with Jump to callee's entry block.
- Remove Return (Jump is the new terminator).
- Result: constant stack usage for tail-recursive functions.

**Limitations** (documented in code):
- arg_base != 0: would need Local copies (deferred).
- Inter-block TCO: only Call+Return within the same block.
- Mutual recursion: not detected (would need call-graph).

## Test results at close

  Fresh build OK
  ./build.py test core → 9/9 suites green
  test_issue_171 → 30/30 PASS
  (5/5 baseline → 19/19 after inliner → 30/30 after TCO)
