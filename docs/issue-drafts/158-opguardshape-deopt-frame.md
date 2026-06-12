# Issue #158 — OpGuardShape full deopt frame (Issue #61 follow-up)

**Title (proposed)**: `feat(jit): full deopt frame for OpGuardShape — entry-guard to generic trampoline`

**Labels**: jit, performance, correctness, design-needed

**Estimated scope**: 1-2 days (4 iterations, small to big, per #61 design doc)

**Related**:
- #61 — Original OpGuardShape design (4-iter plan)
- #157 — Workspace memory model + lock protocol (Phase 4 doc)
- #115 — Stale `fn_ptr` fix (already shipped, closes one of the integration risks)
- #59 — `std::atomic<ScalarFn>` swap (provides the atomic-pointer primitive)

**Follows from**: #157 Phase 3 (deferred — see `docs/issue-closings/157-close-comment.md`)

---

## Context

The deopt-guard machinery for shape-specialized JIT code is partially
shipped but **inert**: the opcode exists, the JIT handler exists, the
`specialized_for` field exists, but no path actually emits the
guard. The current state is:

| Component | Status | Location |
|-----------|--------|----------|
| `IRFunction::specialized_for` + `generic_id` fields | ✅ done (Iter 1) | `src/compiler/ir.ixx:413,418` |
| `OpGuardShape` opcode in `IROpcode` enum | ✅ done (Iter 2 partial) | `src/compiler/ir.ixx:94` |
| `OpGuardShape` in `kOpcodeInfo` | ✅ done | `src/compiler/ir.ixx:226` |
| JIT handler for `OpGuardShape` (computes shape + writes 1/0) | ✅ done (Iter 3 partial) | `src/compiler/aura_jit.cpp:644` |
| IR interpreter for `OpGuardShape` | ❌ missing | — |
| Lowering emits `OpGuardShape` for specialized functions | ❌ missing | `src/compiler/lowering.ixx` |
| Lowering emits `OpGuardShape → Branch → generic trampoline` | ❌ missing | — |
| Generic trampoline (function compiled with `specialized_for = 0`) | ❌ missing | — |
| Pipeline compiles both specialized + generic when shape is stable | ❌ missing | — |
| Tracing (`AURA_DEOPT_TRACE=1`) | ❌ missing | — |

**Net effect**: the deopt-guard design is a scaffold with no active
path. Specialization for shape is **not happening** in practice (the
lowering never emits the guard), and if it did happen, the deopt
target (generic version) doesn't exist — so the deopt would fail or
crash.

This issue ships the missing pieces. Per the #61 design doc, four
iterations, small to big.

---

## Why now

#157 Phase 3 was the original "ship this" attempt. After
re-scoping, it became clear that the full deopt frame is 1-2 days
of focused work (lowering emit + generic trampoline + interpreter
re-entry + pipeline integration). It doesn't fit a late-night
marathon session; it deserves its own scoped issue with proper
attention.

#157 itself is **closed** with Phases 0/1/1b/1c/2/4/5 shipped.
The follow-up for Phase 3 is this issue.

---

## Phases (per #61 design doc, revised)

### Phase 1 — `IRFunction` fields (already shipped, Iter 1)

**Status**: ✅ already done in #61 Iter 1.

`src/compiler/ir.ixx:413,418` — `specialized_for` (default 0) +
`generic_id` (default 0xFFFFFFFF). The fields exist but are never
set by the lowering.

### Phase 2 — `OpGuardShape` opcode + JIT handler (mostly done)

**Status**: 🟡 mostly done; missing the actual branch emit.

- ✅ `OpGuardShape` opcode in `IROpcode` enum (`ir.ixx:94`)
- ✅ `kOpcodeInfo` entry (`ir.ixx:226`)
- ✅ JIT handler in `aura_jit.cpp:644` — computes shape of arg,
  compares to expected, writes 1/0 to result slot
- ❌ JIT handler doesn't emit `CreateCondBr(matches, spec_blk, generic_blk)`
  — currently the deopt decision lives in a subsequent `OpBranch`
  instruction that the lowering never emits

**Ship it**: Update the JIT handler to emit the branch (acquire
`fn_lock_workspace_*` mutex state? No — `OpGuardShape` is
read-only; the version check happens at fn entry, see Phase 3).
Or, simpler: keep the bool-to-result-slot pattern and let the
existing `OpBranch` handler do the work. The branch is already
handled correctly in `OpBranch` (aura_jit.cpp:630).

**Decision needed**: which pattern does the IR use? The current
`OpGuardShape` is `(result, arg, expected, generic_block_id)` —
4 operands. If we keep the bool pattern, the IR also has an
`OpBranch` after. If we use the inline-branch pattern, the
branching is in `OpGuardShape` itself.

For now: keep the bool pattern (already implemented in JIT).
The lowering (Phase 4) emits `OpGuardShape` followed by
`OpBranch`.

### Phase 3 — IR interpreter for `OpGuardShape` + `runtime_shape_of`

**Status**: ❌ missing. `runtime_shape_of()` exists in
`evaluator_impl.cpp` (used by `cast_op`); need to expose it and
have the IR interpreter call it.

`src/compiler/ir_executor_impl.cpp` — add a `case IROpcode::GuardShape`:
```cpp
case IROpcode::GuardShape: {
    auto arg_val = locals[ops[1]];
    auto expected = ops[2];
    auto actual = runtime_shape_of(arg_val);
    locals[ops[0]] = make_bool(actual == expected);
    break;
}
```

This is the **source of truth** for shape — the JIT's approximation
may say "matches" but the interpreter always says the exact thing.
If the JIT says "matches" and the interpreter says "doesn't match",
that's a JIT bug (over-approximation), not a soundness issue
(both paths are correct, just one is slower).

### Phase 4 — Lowering emits `OpGuardShape` + `OpBranch` for specialized functions

**Status**: ❌ missing. This is the biggest piece.

`src/compiler/lowering.ixx` — when a function is being specialized
for a stable shape:

1. Compile the function **twice**:
   - Once with `specialized_for = 0` (generic version) — the
     existing lowering.
   - Once with `specialized_for = shape_id` (specialized version) —
     same code, but the entry block gets a `OpGuardShape` for each
     arg slot.
2. Both versions go into `IRModule.functions[]`.
3. The specialized version sets `generic_id` to the index of the
   generic version.
4. The specialized version's entry block emits:
   ```
   OpGuardShape result=0, arg=arg_slot, expected=shape_id, generic_block=2
   OpBranch result=0, true_block=1, false_block=2
   block 1 (specialized body): existing function body
   block 2 (generic trampoline):
       OpCall result=0, callee=generic_id, args=[arg_slots...], arg_count=N
       OpReturn result=0
   ```
5. The first call to a specialized function captures the shape via
   the existing `shape_profiler_` (#60).

**Design questions to resolve during implementation**:
- Where does the "compile twice" live? In `lower_module` or a new
  `monomorphize_module` pass? Probably a new pass that runs after
  `lower_module` and before JIT compilation.
- What's the linkage between specialized and generic in the cache?
  When the generic version is recompiled (e.g. after `mutate:*`),
  does the specialized version get invalidated too?
- For now: keep the linkage simple — `generic_id` is an index into
  `IRModule.functions[]`; if either version is recompiled, both
  are. The atomic pointer swap from #59 Iter 4 ensures other fibers
  see a consistent (specialized, generic) pair.

### Phase 5 — Tracing + micro-bench + integration

**Status**: ❌ missing. Per the #61 design doc.

- `AURA_DEOPT_TRACE=1` env var — when set, every deopt logs
  `[deopt] fn=<name> expected=<shape> actual=<shape>` to stderr.
- `tests/test_ir.cpp` — `TC61` cases: specialized_for round-trip,
  generic_id round-trip, OpGuardShape enum export.
- `tests/test_regression.py` — define `(f x) (+ x 1)`, call 10×
  with Int (triggers specialization), then call with String.
  Assert Int call returns correctly; String call should not crash
  (deopt to generic path).
- Micro-bench: `tests/test_ir_bench.cpp` — per-call overhead of
  specialized function should be < 2ns for the guard itself
  (the deopt-to-generic call adds fn call overhead separately).
- Integration with `mutate:*` (#59 Iter 3) — when the function
  body is mutated, `jit_cache_["__lambda__"]` is erased + the
  generic version is recompiled. The next call goes to generic.
  No race (atomic swap from #59).

---

## Out of scope (defer to follow-up issues)

- **Type Guard** — separate from ShapeID, static at compile time
  (computed from the type system), doesn't change at runtime.
  Less interesting; punt.
- **Version Guard** — per-function version counter bumped on
  `mutate:*`. The guard checks "is this function's version
  still the one I specialized for?" An alternative to the
  current invalidation; future enhancement.
- **Side-effect replay** — the hardest part of real deopt. If
  the guard is at function entry, no state has been mutated
  yet, so replay is trivial. If the guard is mid-function
  (e.g. at a loop backedge), state may have been mutated and
  we need to rewind. **This issue only ships entry guards**
  (80% of the value). Mid-function guards are a follow-up.
- **Speculative inlining + deopt** — inline a function with a
  guard at the inlined-call site. Punt.
- **Epoch/RCU lock-free read path** — #157 design doc Phase 5,
  long-term, optional.

---

## Test plan

- `tests/test_ir.cpp` (unit):
  - TC61-1: `IRFunction.specialized_for` round-trip (0 default,
    set, read).
  - TC61-2: `IRFunction.generic_id` round-trip (default
    `0xFFFFFFFF`, set, read).
  - TC61-3: `OpGuardShape` enum is exported and resolvable.
  - TC61-4: JIT handler for `OpGuardShape` writes 1/0 to result
    slot, no crash, no segfault.

- `tests/test_regression.py` (integration):
  - Define `(f x) (+ x 1)`, call 10× with Int (triggers
    specialization), then call with String. Assert Int call
    returns correctly; String call should not crash.
  - Mutate `f`'s body via `mutate:rebind`, then call again.
    Assert no crash, no infinite loop (deopt path works).

- `tests/test_ir_bench.cpp` (perf):
  - Hot loop of `(f x y)` where `f` is specialized for Int.
  - Assert per-call overhead < 2ns for the guard itself.

- `tests/test_serve_async_concurrent.cpp` (concurrency):
  - Multi-fiber serve with concurrent `mutate:rebind` + JIT
  - execution of the same function. Assert no torn reads,
    no crashes, every call returns a valid value.

---

## Acceptance criteria

After all 5 phases:

- ✅ Specialized functions are emitted by the lowering (when shape
  is stable per the profiler).
- ✅ The deopt path is sound: a mismatched shape call lands in
  the generic version, not a crash.
- ✅ The deopt latency is acceptable: single icmp + br for the
  guard itself (~1-2ns on modern x86/ARM), plus fn call overhead
  for the trampoline.
- ✅ Integration with `mutate:*` works: mutated functions are
  recompiled as generic; the next call goes to the generic
  version; no race (atomic swap from #59).
- ✅ Tracing: `AURA_DEOPT_TRACE=1` logs every deopt with fn
  name + expected/actual shape.
- ✅ All existing tests pass (no regression).
- ✅ New tests pass: test_ir TC61 cases, test_regression.py
  deopt cases, test_ir_bench perf bound, concurrent stress.

---

## Affected files (incremental)

- Phase 1: ~~`src/compiler/ir.ixx`~~ (already done)
- Phase 2: ~~`src/compiler/aura_jit.cpp` (JIT handler)~~ (already done)
- Phase 3: `src/compiler/ir_executor_impl.cpp` (interpreter
  case for `OpGuardShape` + `runtime_shape_of` exposure)
- Phase 4: `src/compiler/lowering.ixx` (or new
  `monomorphize.ixx`) — compile-twice for specialized
  functions; `src/compiler/service.ixx` (link generic +
  specialized in the cache)
- Phase 5: `src/compiler/aura_jit.cpp` (tracing),
  `tests/test_ir.cpp`, `tests/test_regression.py`,
  `tests/test_ir_bench.cpp`

---

## Why this issue (not just "ship #61")

The #61 design doc has been on the books for a while but the
work was de-prioritized because:
- The performance benefit (specialization) wasn't measurable
  in the common case — most functions in the regression suite
  are called once or twice, so JIT overhead dominates anyway.
- The complexity (compile-twice + generic trampoline + interpreter
  re-entry) is substantial.
- #157 was a higher-priority P0 fix that needed shipping first.

With #157 closed and the memory model + lock protocol in
place, the infrastructure is ready for #61 to land safely.
Specialization will get a measurable benefit when:
- The evo-kv project uses more polymorphic call sites
  (frequent type-changing arguments).
- A new high-performance numerical workload ships.
- The JIT's per-function cache (#114) makes the compile-twice
  cost amortize.

**This issue is the right time to ship #61.**

---

## References

- `docs/design/notes/issue-61-deopt-guards.md` — Full design (4-iter plan)
- `docs/design/notes/issue-157-jit-workspace-invariant.md` — Memory model + lock protocol
- `docs/design/core/memory_model.md` — Single-page formalization
- `docs/issue-closings/157-close-comment.md` — #157 close-out (Phase 3 deferred)
- `src/compiler/ir.ixx:94,226,413,418` — Existing fields + opcode
- `src/compiler/aura_jit.cpp:644` — Existing JIT handler
- `docs/design/notes/issue-59-thread-safe-jit.md` — Atomic ScalarFn swap primitive
- `docs/design/notes/issue-60-shape-in-ir.md` — ShapeID on IR instructions
