# Offload: background verification on a frozen snapshot

Issues #4881–#4888. The offload pool runs jobs that see only an immutable
`SnapshotInput` and return a plain `Result`; it never receives the session
`Evaluator`. Each job builds a **thread-private `CompilerService`**, so a
parked fiber's eval/body mutexes can never deadlock a worker.

## Language primitives

```scheme
(define snap (offload:snapshot))            ; current workspace source → handle
(define t (offload:submit snap
            '(typecheck test)               ; stages run in order
            :class 'compile))               ; optional: interactive | compile | batch
(offload:poll t)                            ; :queued :running :done :failed
                                            ; :cancelled :rejected
(offload:await t)                           ; park the fiber, then return the hash
(offload:await t :timeout-ms 2000)          ; bounded wait → (merr timeout ...)
(offload:cancel t)                          ; request cancellation
```

The result hash:

```scheme
{status: :done
 diagnostics: (…)
 tests: {passed: n failed: m failures: (…)}
 timing: {queue-us: q exec-us: e}
 mode: :async}                              ; :sync outside a fiber / without a pool
```

`offload:submit` returns `(merr queue-full …)`, `(merr quota-exceeded …)` or
`(merr offload-unavailable …)` when the pool rejects the submission.

## Snapshot isolation

`offload:snapshot` copies the workspace source (O(source)) at the call site.
The job cannot see session modifications made after that point, and its
results are not written back into the session. Compiled artifacts stay out of
the session: load them explicitly with the existing hot-swap / `aot:reload`
path once verification passes. This keeps **verify** and **commit** separate.

## Mode notes

* Fiber + started service → `mode: :async`, completion resumes only the
  matching wait generation.
* Non-fiber (plain `aura`, `--load`, `--serve-async-bench` without a started
  service) → the job runs inline, `mode: :sync`, same result shape.
* Sandbox: `offload:*` is read-only/compute capability and works with
  `AURA_SANDBOX` unset (production default) and `AURA_SANDBOX=off`.

## eval:async

`eval:async` now submits to the same pool (`--serve-async`) and evaluates the
code string in a private `CompilerService` snapshot taken before the session.
It therefore **cannot see session defines**; the non-fiber modes keep the
synchronous `eval` fallback. `thread_pool:enqueue` is unchanged for now and is
removed by the follow-up after `offload:*` lands.
