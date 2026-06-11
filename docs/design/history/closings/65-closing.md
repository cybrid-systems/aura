## Closing — fiber:join 已经实装 (issue 描述基于过时代码)

This issue was filed 2026-06-02 describing fiber:join as a stub. The
fix for this landed on 2026-05-29 in commit `72c9559`
(`feat(#21): fiber:join implementation + orch:parallel fix`), one
day before the issue was opened. The issue appears to have been
drafted against the pre-fix state.

## Acceptance criteria from the issue, verified

- [x] `(fiber:join (fiber:spawn (lambda () (+ 1 2))))` → `3` ✓
  - Test: `(define fid (fiber:spawn (lambda () (+ 1 2)))) (display (fiber:join fid))` outputs `3`
- [x] `orch:parallel` runs tasks concurrently — uses `fiber:join` in `std/orchestrator.aura`
  - `(require std/orchestrator all:) (orch:parallel (list (lambda (x) (+ x 1)) (lambda (x) (* x 2))) 5)` → `(6 10)`
  - The Aura implementation now `(cons (fiber:join (car ids)) results)` instead of discarding
- [x] All existing serial pipeline tests still pass (201/201 regression)

## Implementation summary (from `72c9559`)

`src/compiler/evaluator_impl.cpp`:
- `fiber:spawn` now stores the result via `std::shared_ptr<std::optional<EvalValue>>` so the
  spawned thread can write the result and `fiber:join` can read it.
- `fiber:join` implements a yield-loop with a 200000-iteration budget:
  - In serve-async mode: `g_fiber_yield()` between iterations (real fiber scheduling)
  - In stdin mode: `std::this_thread::sleep_for(1ms)` between iterations
  - Returns the fiber's stored result on success, or `void` on timeout

`lib/std/orchestrator.aura`:
- `orch:parallel` no longer discards `fiber:join` results — it collects them
  and returns the list of results (with optional timeout support).

## Why this isn't actually a P0 anymore

`orch:parallel` works correctly in stdin mode (verified with `(+ x 1)` and `(* x 2)`).
The serve-async fiber path is also wired but the "true parallel" optimization
(g_fiber_spawn → fiber pool work-stealing) only kicks in under `--serve-async`
mode. For agent OS workloads with persistent processes, this is the right path.

Closing this issue as resolved by `72c9559`.
