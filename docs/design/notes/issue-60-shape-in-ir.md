# Design: Issue #60 — Type + shape information into IR for Speculative JIT

## Context

Issue #60 says: "Current type system design keeps type information out
of IR (only inserts CastOp). This prevents Speculative JIT from
performing effective type/layout specialization."

I read `src/compiler/ir.ixx`, `src/compiler/lowering.ixx`,
`src/compiler/lowering_impl.cpp`, `src/compiler/aura_jit.cpp`,
`src/compiler/shape_profiler.h`, `src/compiler/shape.h`,
`src/compiler/service.ixx`, `src/compiler/cache.ixx`,
`src/compiler/cache_impl.cpp`.

## What I found — Issue #60 is 50% done via #73

| Issue requirement                        | Status                              |
| ---------------------------------------- | ----------------------------------- |
| Add `TypeId` to IR instructions           | ✅ done (#73 Phase 1) — `IRInstruction.type_id` |
| Propagate type info during lowering      | ✅ done (#73 Phase 1) — `LoweringState::emit` reads `flat.type_id(current_source_id)` |
| Add `ShapeId` to IR instructions         | ❌ not done — `IRInstruction` has no `shape_id` |
| SpeculativeCompiler uses shape info      | 🟡 L1 done (via `shape_map` side channel), L2 not done |
| Keep CastOp backward compat              | ✅ done (#73) — `TypeSpecializationWrap` still works |

`#73` closed the `TypeId` half. The remaining work is `ShapeId` + L2
specialization.

## Current state — how `shape` flows today

1. **Profile** (`shape_profiler.cpp`): after each eval, record the
   observed shape ID per `(function, arg_slot)`. After enough
   evals, the per-slot shape is considered "stable".
2. **Per-function map** (`service.ixx:set_shape_map`): the
   `ShapeProfiler`'s `dominant_shape(fn_key)` is encoded into a
   `std::vector<std::uint8_t> shape_map_storage` of length
   `ir_fn.local_count`. Each byte is a `shape::ShapeID` (e.g.
   `SHAPE_INT=2`, `SHAPE_PAIR=10`).
3. **Side channel into JIT** (`FlatFunction.shape_map`): the
   `shape_map_storage` is passed to the JIT as a side-channel array.
4. **JIT consumes** (`aura_jit.cpp:382-385`): for each `OpAdd`
   etc., the JIT reads `shape_map[ops[1]]` to decide whether to
   emit a specialized fast path (L1: "is this Int?").

The shape lives in a **per-function array**, not on the IR
instruction that produced the value. Result:

- L1 (per-arg shape → op specialization) works.
- L2 (per-result layout → specialization) **cannot** work, because
  the JIT has no way to ask "what's the shape of slot N's value
  *right now* at this IR instruction?" The `shape_map` is the
  shape of the *arg slot when the function was called*, not the
  shape of every intermediate result.

To do L2 (e.g. specialize on `Pair<Int,Int>` vs `Pair<Str,Str>`
layouts), the shape must be **on the IR instruction** that
produced the value.

## The fix (4 iterations, small to big)

### Iter 1 — Add `shape_id` to `IRInstruction`

**File**: `src/compiler/ir.ixx`

```cpp
export struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
    std::uint32_t type_id = 0;     // from FlatAST (already present)
    std::uint32_t shape_id = 0;    // NEW. 0 = unknown/Dynamic.
                                   // Populated at JIT compile time from
                                   // the per-function shape_map, and
                                   // updated by future profile-driven
                                   // tools (#60 Iter 2).
};
```

**Why not in the FlatAST cache**: `shape_id` is a **runtime
profile property**, not a static AST property. The cache
(`cache_impl.cpp`) persists `flat.type_id(id)` because that's
static. `shape_id` will be populated when the IR is built, not
when the AST is parsed.

### Iter 2 — Populate `shape_id` in `FlatFnBuilder`

**File**: `src/compiler/service.ixx` (the `FlatFnBuilder::build_flat_fn`)

The `shape_map` is already computed for each function (from
`set_shape_map`). When building the `flat_instrs` array from the
`IRFunction`, the builder knows:
- The local slot the instruction writes to (`ops[0]`)
- The shape of that slot (`shape_map[ops[0]]`)

So for every IR instruction with a result slot, set
`flat_instrs[bi].back().shape_id = shape_map[ops[0]]` if
`ops[0] < shape_map_size`.

```cpp
for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
    auto& block = ir_fn.blocks[bi];
    for (auto& instr : block.instructions) {
        FlatInstruction fi{
            static_cast<std::uint32_t>(instr.opcode),
            {instr.operands[0], instr.operands[1], instr.operands[2], instr.operands[3]}};
        // Populate shape_id from the function-level shape_map
        if (final_shape_map && instr.operands[0] < ir_fn.local_count) {
            fi.shape_id = final_shape_map[instr.operands[0]];
        }
        flat_instrs[bi].push_back(fi);
    }
    ...
}
```

**File**: `src/compiler/aura_jit.h` — add `shape_id` to `FlatInstruction`.

### Iter 3 — JIT reads `instr.shape_id` for L1 fast paths

**File**: `src/compiler/aura_jit.cpp`

The JIT's `OpAdd` etc. currently reads `shape_map[ops[1]]` and
`shape_map[ops[2]]` (the **arg** slot shapes). With `shape_id` on
each instruction, the JIT can read the **result** slot shape too
(matching `ops[0]`). This unifies the L1 fast path:

```cpp
case OpAdd: {
    // Read shape of the instruction's RESULT, not just the args
    auto a = load(inst.ops[1]);
    auto b = load(inst.ops[2]);
    bool spec_int = (inst.shape_id == SHAPE_INT);
    if (spec_int) {
        store(inst.ops[0], irb->CreateAdd(a, b));
        return true;
    }
    // ...existing float path...
}
```

This is a **mechanical** change: the JIT no longer needs to
index into the side-channel `shape_map` for every instruction;
the shape is right there on the instruction.

**Acceptance** for Iter 3: same L1 perf as before (no regression),
plus the JIT is structurally ready for L2 (Iter 4).

### Iter 4 — L2 layout specialization on `shape_id`

**File**: `src/compiler/aura_jit.cpp`

Once `instr.shape_id` is on each IR instruction, the JIT can
specialize on layouts. The two highest-value L2 specializations:

#### 4a. `OpPairAlloc`: emit a fixed `AuraPair` struct directly

```cpp
case OpPairAlloc: {
    auto car = load(inst.ops[1]);
    auto cdr = load(inst.ops[2]);
    auto* t = irb->CreateAlloca(pair_struct_ty);
    irb->CreateStore(car, irb->CreateStructGEP(pair_struct_ty, t, 0));
    irb->CreateStore(cdr, irb->CreateStructGEP(pair_struct_ty, t, 1));
    // Encode as a "pair of <shape(car), shape(cdr)>" tagged value
    // (concrete layout, not ref-indexed)
    auto idx = alloc_pair(t, car, cdr);
    store(inst.ops[0], c64(idx));
    return true;
}
```

#### 4b. `OpVectorRef` / `OpVectorSet`: specialize on element type

When `shape_id` indicates `Vector<Int>`, emit a `std::vector<int>`
operation directly. When `Vector<Pair>`, emit the pair-element
variant. Etc.

This requires the runtime to support typed `vector<>` slots. The
existing `aura_jit_runtime.cpp` already has a `Vector` slot type
at runtime; we just need to add typed accessors.

**Acceptance** for Iter 4: L1 + L2 both show measurable gains on
a vector-of-int benchmark. (The #60 acceptance #1.)

## Out of scope (defer)

- **Profile-on-every-eval** that updates per-instruction shape
  incrementally: the current `ShapeProfiler` is per-(function,
  arg_slot). Per-instruction profiling is a bigger refactor
  (would need to map IR instructions back to AST nodes, then map
  AST nodes to the result of each eval). Punt to a follow-up.
- **Cross-function inlining with shape propagation**: if function
  A inlines function B, the inlined copy needs to inherit B's
  shape. Punt.
- **Speculative deopt** (issue #59 Iter 4 follow-up + #60
  interaction): the actual deopt state machine. Defer.

## Backward compat

- Iter 1: new field, defaults to 0 (unknown). Existing code that
  doesn't read `shape_id` is unaffected.
- Iter 2: `FlatFnBuilder` reads `final_shape_map`; if
  `final_shape_map == nullptr`, the `shape_id` stays 0 (unknown).
- Iter 3: JIT's `OpAdd` etc. fall through to the existing float
  path if `shape_id != SHAPE_INT` (existing behavior is
  equivalent to `shape_id == 0` or "unknown").
- Iter 4: only kicks in for specific shape IDs; the existing
  generic path is the fallback.

## Test plan

- `tests/test_ir.cpp`:
  - `TC60 OK: IRInstruction has shape_id field` (sizeof check).
  - `TC60 OK: shape_id defaults to 0`.
  - `TC60 OK: shape_id=2 round-trips through struct assign`.

- `tests/test_regression.py`:
  - Add a case that triggers L1 specialization (e.g. an inner
    loop of `(+ x y)` where `x` and `y` are always Int) and
    assert no perf regression (we can't easily assert JIT
    code-gen, but we can assert the program is correct).

- Smoke: `echo "(+ 1 2)" | ./build/aura --inspect ir` should show
  `shape_id: 2` (SHAPE_INT) for the const-i64 instructions
  (post-Iter 2; requires that the function-level shape_map
  propagates to the instructions).

## Affected files (incremental)

- Iter 1: `src/compiler/ir.ixx`
- Iter 2: `src/compiler/aura_jit.h`, `src/compiler/service.ixx`
- Iter 3: `src/compiler/aura_jit.cpp`
- Iter 4: `src/compiler/aura_jit.cpp`, `src/compiler/aura_jit_runtime.cpp`

## Acceptance

After all 4 iters:

- ✅ L1 type specialization: same as before (no regression).
- ✅ L2 layout specialization: measurable gain on vector-of-int,
  pair-of-int workloads.
- ✅ All existing type-related tests pass (we don't break
  anything that relied on `shape_id == 0` for the generic path).
- ✅ No regression in self-modification safety (#59 changes
  preserved).
