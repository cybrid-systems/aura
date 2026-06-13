# LLVM JIT Backend Completion — Design (Issue #170)

**Status:** Phase 1 (AOT entry points) shipped 2026-06-13 (f432d4b)
**Date:** 2026-06-13
**Workstream:** 2 of #143 (LLVM JIT Backend completion)

## Problem (from issue #170)

After the #143 top-3 improvement proposals were scoped, workstream 2
(Accelerate LLVM JIT Backend) was identified as the biggest remaining
performance gap. The JIT currently:

1. **Silently produces wrong output for unhandled opcodes.** The
   `lower()` switch in `src/compiler/aura_jit.cpp:447-1255` has a
   `default:` branch (L1253-1257) that writes 0 to the result slot
   and reports `return true` to the caller. So an unhandled opcode
   doesn't fall back to the interpreter, doesn't error — it just
   silently gives a wrong answer. This is the most critical
   soundness issue.

2. **Has a gap between IR opcodes (53) and JIT-lowered opcodes (41).**
   The 12 missing are: Nop (skip), Apply, CaptureRef, Raise, IsError,
   TryBegin, TryEnd, ConstF64 (sometimes hits the wrong case shape),
   and a few that the grep lost in formatting. Need exact list before
   shipping.

3. **Lacks the AOT path entirely** — Phase 1 of #170 (AOT entry
   points) shipped in f432d4b: `compile_to_llvm_ir()` and
   `compile_to_object_file()` on `AuraJIT`. Verified: 5/5 tests,
   9/9 core suites green.

4. **No LLVM exception handling** for `try`/`catch`. The interpreter
   side already supports it (#124); the LLVM side has no lowering.

## Scope (4 items, 4-6 weeks total, "High" difficulty)

| # | Item | Effort | Status | Commit |
|---|---|---|---|---|
| Phase 2 / item #2 | AOT entry points (compile_to_llvm_ir + compile_to_object_file) | 2h | ✅ SHIPPED | f432d4b |
| Phase 1 / item #1 | Complete core lowering: Apply + CaptureRef + visible default | 1-2w | TODO | — |
| Phase 1 / item #2 | try/catch IR opcodes + LLVM exception handling | 1-2w | TODO | — |
| Phase 2 / item #1 | spec_jit_controller + shape specialization integration | 1w | TODO | — |
| Phase 2 / item #3 | runtime → LLVM intrinsics selective migration | 1-2w | TODO | — |

Each item is a verify+close cycle, modeled on #169 Phase 1 (config
flag, single commit, design-doc-tracked deferrals for the rest).

## Detailed scope for each item

### Phase 1 / item #1 — Complete core lowering

**Why first:** ships the most-impactful soundness fix (visible
default) plus completes the two closure opcodes that fall through
to wrong output today (Apply, CaptureRef). Without this, the JIT
silently produces incorrect results for any Aura function that uses
`apply` or `letrec`-style cells with capture-by-ref.

**Scope:**
- `OpApply` lowering in `aura_jit.cpp::lower()` — similar to
  `OpCall` but with closure + inline-args encoding. Reuse
  `aura_closure_call` runtime bridge (already exists in
  `aura_jit_runtime.cpp:493`).
- `OpCaptureRef` lowering — captures a *cell* (not a value).
  Needs a new runtime bridge `aura_closure_capture_ref(closure, env_idx, cell_id)`
  or extend `aura_closure_capture` with a flag.
- Replace the silent `default:` with:
  - An `unhandled_opcode_counter_` atomic counter in `AuraJIT::Impl`
  - The default case: increment counter, write a sentinel value
    (e.g., tagged `#eof` or `0`) and emit a `llvm::IRBuilder` trap
    (`llvm::Function::Create(..., UnreachableInst)`) so the
    optimizer can't elide the wrong-result computation.
  - Public accessor `unhandled_opcode_count()` for
    `spec_jit_controller` (Phase 2 / item #1) to consume.
- A test (`test_issue_170_item1.cpp`?) that:
  - Verifies Apply + CaptureRef actually lower (test on a
    synthetic IR program with apply + letrec)
  - Verifies the unhandled-opcode counter is exposed
  - Verifies the unhandled-opcode counter doesn't increment for
    any well-formed Aura function in the test suite

**Acceptance criteria:**
- Build clean, all existing tests still pass
- `test_issue_170_item1` 100% green
- Counter accessible via public API
- Diff in `aura_jit.cpp` is small and well-commented

**Out of scope (deferred):**
- Raise / IsError / TryBegin / TryEnd → Phase 1 / item #2
- Run-time exception handling → Phase 1 / item #2
- ConstF64 fix (verify it's actually missing) → may be folded
  into this item if quick

### Phase 1 / item #2 — try/catch IR opcodes + LLVM exception handling

**Why after item #1:** item #1 establishes the lowering-skeleton
infrastructure (counter, runtime-bridge convention). Item #2 adds
the largest single piece of new functionality: LLVM `invoke`/`landingpad`
instructions + the runtime exception stack + a personality function.

**Scope:**
- New runtime function: `aura_exception_push(handler_block, payload_slot)`
  / `aura_exception_pop()`. The interpreter already has `ex_stack_`
  (`ir_executor_impl.cpp:658-678`) — port the same data structure to
  a C-linkage bridge in `aura_jit_runtime.cpp`.
- Lowering for `OpRaise`, `OpIsError`, `OpTryBegin`, `OpTryEnd` in
  `aura_jit.cpp::lower()`. For Raise: emit an `invoke` to a
  "personality function" wrapper that calls `aura_exception_throw`.
- New personality function: a C function annotated with
  `__attribute__((personality))` (or equivalent) that LLVM uses to
  walk the stack. Should call `_Unwind_RaiseException`.
- A test that exercises a real `try`/`catch` and verifies the
  LLVM-compiled function takes the catch path.

**Acceptance criteria:** 100% green, no interpreter fall-back, perf
benefit measurable (deferred benchmark to `tests/bench/`).

### Phase 2 / item #1 — spec_jit_controller + shape specialization

**Why after item #2:** items #1 and #2 make the JIT
correctness-complete. Item #1 is the perf layer — feeding the
shape-id metadata (#149) into the spec controller so the JIT
specializes per call site.

**Scope:**
- Wire `AuraJIT::unhandled_opcode_count()` to `spec_jit_controller`
  so a hot function that hits an unhandled opcode auto-deopts to
  the interpreter.
- Extend `GuardShape` (already exists in `aura_jit.cpp:645-689`) to
  consume the rich inferred types from #149. Already partial work
  from #149 Phase 4 (`make_bool(true)` skip on narrow evidence).

### Phase 2 / item #3 — runtime → LLVM intrinsics selective migration

**Why last:** depends on the rest being stable. Migrating runtime
helpers to LLVM intrinsics is a per-function perf optimization.

**Scope (examples, defer to implementation):**
- `aura_alloc_pair_arena` → `llvm::Intrinsic::experimental_gc_statepoint`
  or inlined allocation with `alloca` + bump-ptr
- `aura_pair_car_unchecked` → inline GEP into the LLVM caller when
  the shape is statically known (L2 specialization)
- `aura_float_ref` / `aura_alloc_float` → native double load/store
  in the JIT local (skip the heap round-trip for ephemeral floats)

Each migration is independent and ships as a small commit.

## Status table (updated per cycle)

| Date | Item | Commit | Tests | Notes |
|---|---|---|---|---|
| 2026-06-13 | Phase 2 / item #2 (AOT entry points) | f432d4b | 5/5 | shipped |
| 2026-06-13 | Phase 1 / item #1 (Apply + CaptureRef + visible default) | 1f8c097 | 18/18 | shipped |
| TBD | Phase 1 / item #2 (try/catch IR + LLVM EH) | TBD | TBD | next |

## Notes for future sessions

- The silent-default in `lower()` is a **soundness bug** that
  predates #170 — call it out in the close-out comment when
  the full issue is closed.
- The "High" difficulty rating is in the issue body. Treat
  each item as a real PR with a real test, not a drive-by
  commit.
- Keep this doc updated as items ship. The close-out comment
  on #170 should reference this doc and list all 4 commits.
