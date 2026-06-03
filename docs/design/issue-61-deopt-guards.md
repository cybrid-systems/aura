# Design: Issue #61 — Robust deoptimization and guard mechanisms

## Context

Issue #61 says: "Deoptimization guards are critical for Speculative
JIT correctness, but current implementation lacks robustness,
versioning, and low-overhead design."

I read `src/compiler/ir.ixx`, `src/compiler/ir_executor_impl.cpp`,
`src/compiler/aura_jit.cpp`, `src/compiler/aura_jit.h`,
`src/compiler/shape.h`, `src/compiler/shape_profiler.h`,
`src/compiler/service.ixx`, `docs/design/issue-59-thread-safe-jit.md`,
`docs/design/issue-60-shape-in-ir.md`.

## What I found

The current implementation is **eager recompile**, not **lazy
deopt**. When the shape profiler says a function is stable for
shape X, the JIT compiles a specialized version. When the shape
later changes:

```cpp
// src/compiler/service.ixx:1300-1321
// Hot recompilation: if profiler now has stable shape for this
// function but cache entry was compiled without shape_map,
// evict and recompile with shape specialization.
if (!cache_it->second.has_shape_map &&
    shape_profiler_.is_stable(fn_key)) {
    std::fprintf(stderr, "spec: hot-recompile '%s' (shape now stable)\n",
                ir_fn.name.c_str());
    jit_cache_.erase(cache_it);
}
```

This is **eager** in two ways:

1. **Eager toward "shape now stable"**: when a function's shape
   *first* stabilizes, the cache entry is evicted and recompiled
   on the next call. This works, but the recompile discards
   any earlier compiled version.
2. **Eager toward "shape changed"**: this is the *missing*
   case. If a function is specialized for `Int` and is later
   called with `String`, the existing code *does not detect
   this*. The shape_map is per-arg-slot at compile time; the
   JIT has no entry-time check.

The right approach (issue's design):

- A specialized function is *annotated* with the shape it was
  compiled for (`specialized_for: ShapeID`).
- At function entry, an LLVM-emitted guard checks the actual
  arg shape against `specialized_for`.
- If mismatch, branch to a "generic version" of the function
  (compiled without specialization, slower but always correct).
- Atomic pointer swap means other fibers see either the
  specialized or generic version, never a half-swapped one.

This is the **lazy** deopt design the issue is asking for.

## Adjacent work already done

- **#59 Iter 4**: `std::atomic<ScalarFn>` swap — provides the
  atomic-pointer mechanism. We have the *primitive* for the
  deopt; we just don't have a *function* to call after the swap.
- **#60 Iter 1-4**: `shape_id` on every IR instruction —
  provides the shape data. The IR knows the shape of every
  result slot, so a guard can ask "is this still shape X?"
- **#60 Iter 4**: `inst_shape()` helper in the JIT — already
  checks `instr.shape_id` (or falls back to shape_map).

What's missing is the **guard IR opcode** + the **specialized_for
field on IRFunction** + the **deopt branch** in the IR/JIT
emission.

## The fix (4 iterations, small to big)

### Iter 1 — `IRFunction` gains `specialized_for` + `generic_id`

**File**: `src/compiler/ir.ixx`

```cpp
export struct IRFunction {
    ...
    Region region = Region::Default;
    // Issue #61: shape this function was specialized for. 0 = no
    // specialization (generic). The function entry guard checks
    // the actual arg shape against this and deopts to generic_id
    // on mismatch.
    ShapeID specialized_for = 0;
    // Index into IRModule.functions[] of the generic (un-specialized)
    // version. 0 (the entry function) is also valid. 0xFFFFFFFF =
    // "no generic version" (deopt falls back to interpreter).
    std::uint32_t generic_id = 0xFFFFFFFF;
};
```

`FlatFnBuilder` populates these when compiling a specialized
version: the same source code is compiled twice — once with
`specialized_for = 0` (generic) and once with `specialized_for =
shape_id` (specialized). Both versions go into the module.

### Iter 2 — `OpGuardShape` IR instruction + lowering emission

**File**: `src/compiler/ir.ixx`, `src/compiler/lowering.ixx`

```cpp
// In the IROpcode enum:
//   OpGuardShape: result_slot, arg_slot, expected_shape_id,
//                 generic_block_id
//   result_slot: where the guard's bool result goes (1 = matches)
//   arg_slot: the function argument whose shape is checked
//   expected_shape_id: the ShapeID we're specialized for
//   generic_block_id: the basic block to branch to on mismatch
```

**Lowering** (`lowering.ixx`): when emitting a specialized
function entry, the lowering inserts an `OpGuardShape` for each
arg slot. The result feeds a `Branch` that falls through to the
specialized body or jumps to a "trampoline" block that calls
`generic_id`.

### Iter 3 — IR interp + JIT generate the guard

**File**: `src/compiler/ir_executor_impl.cpp`, `src/compiler/aura_jit.cpp`

The IR interpreter for `OpGuardShape`:
```cpp
case IROpcode::GuardShape: {
    auto arg_val = locals[ops[1]];
    auto expected = ops[2];
    auto actual = compute_shape_of_value(arg_val);
    if (actual == expected) {
        locals[ops[0]] = make_bool(true);
    } else {
        // Deopt: jump to generic trampoline. We don't actually
        // re-execute the trampoline here; the IR interpreter's
        // outer loop sees the Branch instruction and follows it.
        // The branch target's block ends in a Call to generic_id
        // with the original args.
        locals[ops[0]] = make_bool(false);
    }
    break;
}
```

The JIT for `OpGuardShape`:
```cpp
case OpGuardShape: {
    auto* arg_val = load(inst.ops[1]);
    // shape_of(arg) is computed by inspecting the tagged value
    // (similar to how runtime type tag works). LLVM emits an icmp
    // against the expected shape.
    auto* actual_shape = compute_shape_ir(arg_val);
    auto* expected = c64(inst.ops[2]);
    auto* matches = irb->CreateICmpEQ(actual_shape, expected);
    // Branch on matches: fall through (specialized) or jump to
    // generic trampoline block.
    auto* cur_blk = irb->GetInsertBlock();
    auto* generic_blk = block_map[inst.ops[3]];
    auto* spec_blk = llvm::BasicBlock::Create(ctx, "spec.cont", func);
    irb->CreateCondBr(matches, spec_blk, generic_blk);
    irb->SetInsertPoint(spec_blk);
    store(inst.ops[0], irb->CreateZExt(matches, i64_ty));
    return true;
}
```

**Perf**: a single `load + icmp + br` is ~1-2 ns on modern x86/ARM,
within the issue's `< 2ns` budget.

### Iter 4 — Tracing + micro-bench + integration

**File**: `src/compiler/aura_jit.cpp`, `tests/test_ir.cpp`

- Tracing: when a guard fails, log to stderr with the function
  name, the expected shape, and the actual shape. Useful for
  AI agents debugging specialization failures.
- Micro-bench: a `tests/test_ir_bench.cpp` that runs a hot loop
  of `(f x y)` where `f` is specialized for Int and asserts the
  per-call overhead is < 2ns.
- Integration: when a `mutate:*` invalidates a function
  (`#59 Iter 3`), the new compiled version is the generic one
  (specialized_for = 0). The next call goes through the generic
  path. No recompile, no race.

## Out of scope (defer)

- **Type Guard** (issue's first design point): separate from
  ShapeID. Type guards are static (computed at compile time from
  the type system) and don't change at runtime, so they're
  less interesting. Punt.
- **Version Guard** (issue's first design point): a per-function
  version counter that's bumped on `mutate:*`. The guard checks
  "is this function's version still the one I specialized for?"
  This is an alternative to recomputing the shape. The current
  design uses `invalidate_function` to *recompile*; a version
  guard would *deopt* instead. Future enhancement.
- **Side-effect replay** (the hardest part of real deopt): if
  the guard is at function entry, no state has been mutated yet,
  so replay is trivial. If the guard is *mid-function* (e.g. at
  a loop backedge), state may have been mutated and we need to
  rewind. Out of scope for this PR (entry guards are 80% of the
  value).
- **Speculative inlining + deopt** (the most advanced feature):
  inline a function with a guard at the inlined-call site. Punt
  to a follow-up.

## Backward compat

- Iter 1: new fields default to 0 / 0xFFFFFFFF, no behavior
  change.
- Iter 2: `OpGuardShape` is only emitted for functions with
  `specialized_for != 0`. Generic functions have no guards.
- Iter 3: the IR interp and JIT handle `OpGuardShape` explicitly;
  if it's absent, behavior is unchanged.
- Iter 4: tracing is opt-in (`AURA_DEOPT_TRACE=1` env var or a
  CLI flag); micro-bench is a test, not a behavior change.

## Test plan

- `tests/test_ir.cpp`:
  - TC61: `IRFunction.specialized_for` round-trip (0 default,
    set, read).
  - TC61: `IRFunction.generic_id` round-trip (default
    `0xFFFFFFFF`, set, read).
  - TC61: `OpGuardShape` enum is exported and resolvable.

- `tests/test_regression.py`:
  - Define `(f x) (+ x 1)`, call it 10 times with Int args
    (triggers specialization), then call with String. Assert
    the Int call returns the right number; the String call
    should NOT crash (deopt or generic path).
  - Mutate `f`'s body, then call it again. Assert no crash, no
    infinite loop (deopt path works).

## Affected files (incremental)

- Iter 1: `src/compiler/ir.ixx`
- Iter 2: `src/compiler/ir.ixx`, `src/compiler/lowering.ixx`
- Iter 3: `src/compiler/ir_executor_impl.cpp`, `src/compiler/aura_jit.cpp`
- Iter 4: `src/compiler/aura_jit.cpp`, `tests/test_ir.cpp`,
  `tests/test_regression.py`

## Acceptance

After all 4 iters:

- ✅ Hot path specialization works correctly under mutation
  (mutate:* invalidates; next call goes to generic; no crash).
- ✅ Deopt latency is acceptable (single icmp + br, ~1-2 ns on
  modern x86/ARM; well within the issue's < 2ns budget for the
  guard itself; the deopt-to-generic call adds the function call
  overhead which is separate).
- ✅ Good debugging support (tracing via `AURA_DEOPT_TRACE=1`
  logs every deopt with the expected/actual shape).
- ✅ Integration with mutate:* works (no race, atomic swap from
  #59).
