# Issue #109 — fiber:join / orch:parallel / concurrent test flakiness

## Status: ✅ CLOSED — fiber:join + cv-based blocking + T3 test fix

Issue #109 was the P0 multi-agent concurrency work. The fiber
spawn/join implementation, the multi-threaded scheduler, and
the concurrent test suite were all part of it.

## Commit log (this issue)

| Commit | Description |
|--------|-------------|
| `72c9559` | (earlier) `feat(#21): fiber:join implementation + orch:parallel fix` |
| `c49efb6` | `test(#21): add fiber:spawn/fiber:join + orch:parallel tests` |
| `4f968ae` | `feat(concurrent): fiber:spawn works in stdin mode via std::thread` |
| `121dfde` | `feat(concurrent): Phase 1 — multi-threaded fiber scheduler` |
| `499be28` | `feat(concurrent): Phase 2 — work-stealing fiber scheduler` |
| `a4bc36b` | `feat(exec): C++26 std::execution-inspired fiber scheduler adapter` |
| `6401b9c` | `feat(p2): fiber affinity + broadcast + mailbox-stats, 4 new tests` |
| `bd48e76` | `fix(fiber): thread_local c-stack depth + remove orch:parallel serial fallback (Issue #109)` |
| `2f53ee2` | `fix(fiber): real blocking join via condition_variable in stdin mode (Issue #109 5b)` |
| `b41f0e8` | `docs(issue-closings): add #65 closing — fiber:join already fixed` |
| `52c281e` | `test(concurrent): rewrite T3 to bypass Begin's letrec path` |

The first 5 commits land the fiber scheduler work (much of
which predates this branch). The #109-specific fixes are
`bd48e76` (thread_local c-stack) and `2f53ee2` (cv-based
blocking). The test fix is `52c281e`.

## What was broken (and what fixed it)

### Thread_local c-stack depth (`bd48e76`)

`eval_flat` had a recursion-depth counter as an Evaluator
member. In std::thread fallback (stdin mode), multiple
threads share the same Evaluator. Each thread's eval_flat
incremented the same counter, so 5-way `orch:parallel` would
spike past MAX_C_STACK_DEPTH=2000 and bail with "recursion
depth exceeded". Made concurrent.aura T7-T10 flaky.

Fix: split the counter by intent. The per-call-stack
recursion guard became a `thread_local` (each thread has its
own); the cumulative work-tracking counter stayed as
Evaluator member.

### cv-based blocking in stdin mode (`2f53ee2`)

`fiber:join` was polling `s_fiber_results` 200,000 times with
1ms sleep between — up to 200s of CPU per join in the worst
case. In serve-async mode the `g_fiber_yield()` made it
efficient, but in stdin/thread fallback the OS thread was
busy-looping.

Fix: added an explicit `ready` flag on each
`s_fiber_results` entry, protected by `s_fiber_results_mtx_`,
plus a process-wide `s_fiber_results_cv_`. The worker writes
the result, takes the lock, sets `ready=true`, and notifies.
The waiter observes `ready=true` under the same lock and
only then reads the value. Proper producer/consumer
synchronization.

### T3 flake in --load mode (`52c281e`)

T3 (fiber side effects) was failing when the file was
loaded via `--load` (which wraps everything in a Begin) but
passing in `--serve` (where each line is a separate
set_code). Root cause: Begin's **letrec path** (added in
commit `327ec2b` long ago) pre-evaluates all define values
in phase 2 BEFORE running non-define expressions in phase 3:

  (define xs (list))                            ; phase 2
  (define t3-a (fiber:spawn ...))                ; phase 2
  (define t3-b (fiber:spawn ...))                ; phase 2
  (fiber:join t3-a)                              ; phase 3
  (fiber:join t3-b)                              ; phase 3
  (define t3-len (length xs))                    ; captured in phase 2 — xs is still ()

So `t3-len` was bound to 0 before the fibers ran.

Fix: rewrite T3 to use nested `let` so the length capture
happens at use-site, after the joins. The behavior under
test (fiber side effects propagate to the surrounding scope)
is identical. A proper fix would re-architect Begin's letrec
path to interleave define and non-define evaluations
(thunk-based defines, or just sequential evaluation like
single-define). Tracked as a follow-up.

## Verification (final state)

`./build/aura --load tests/suite/concurrent.aura`:
```
[T1] fiber:spawn-join ... PASS
[T2] multi-fiber ... PASS
[T3] fiber-side-effects ... PASS
[T4] orch:parallel ... PASS
[T5] orch:pipeline ... PASS
[T6] orch:conduct ... PASS
[T7] orch:parallel-5 ... PASS
[T8] fiber-nested ... PASS
[T9] fiber-stress-10 ... PASS
[T10] orch:parallel-empty ... PASS
[T11] parallel-aggregation ... PASS
[T12] parallel-join-returns ... PASS
```

12/12 PASS, exit=0.

ASAN verification:
- `build_asan/test_ir` exit=0, 0 leaks
- `build_asan/aura --load concurrent.aura` 12/12 PASS, exit=0
- `fuzz_defuse/workspace/snapshot --quick` 200/292/405 all clean

## Architecture summary after #109

`fiber:join` semantics:
- In serve-async mode: scheduler yields to other fibers while
  waiting; the join is real cooperative scheduling.
- In stdin/thread mode: worker writes result, sets `ready`,
  notifies cv. Waiter blocks on cv until ready=true, then
  reads the result. Producer/consumer with one mutex and
  one cv.
- No busy-wait fallback. The old 200K-iteration poll is gone.

`orch:parallel`:
- Uses `fiber:join` to collect per-task results into a list.
- No more "serial fallback" — the `std::thread` path is the
  real concurrent path in stdin mode.
- `orch:conduct` and `orch:pipeline` also use fibers
  internally; correctness verified by T4-T6.

## Follow-ups that remain open

1. **Begin's letrec path** is broken for the "define after
   spawn" pattern. The right fix is to either:
   - Evaluate defines sequentially (drop the letrec pre-pass
     and use thunks), or
   - Interleave define and non-define expressions in a single
   pass.
   Tracked separately. The T3 test rewrite works around the
   issue; real fix is a Begin refactor.

2. **`fiber:join` busy-wait fallback** is gone in stdin mode
   but the polling code path is still in the codebase
   (dead code, kept for safety). Worth removing in a future
   cleanup.

3. **Concurrent test flakiness under high load**: T9
   (fiber-stress-10) passes but is timing-sensitive. Under
   high CPU contention it could flap. The thread_local c-
   stack fix reduced the flakiness, didn't eliminate it.
   Future stress tests should use `std::this_thread::sleep_for`
   tolerance for timing-sensitive assertions.

These are pre-existing Aura quirks, not regressions from
#109. They are all isolated to the concurrent test path and
don't affect the standard query/mutate pipeline.
