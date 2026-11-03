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

## Metrics (#4884)

The pool keeps relaxed atomic counters and fixed-bucket histograms; neither the
record nor the snapshot allocates per sample. In `--serve-async` they appear in
both metric faces:

* `(stats:get "orch:metrics")` (the `orch:metrics` stats entry) returns the
  scheduler JSON with an added `"offload"` section
  (`submitted_total`, `rejected_*`, `completed_*`, `queue_depth`, `running`,
  `threads`, `queue_depth_peak`, `queue_wait_us` / `exec_us` /
  `wake_latency_us` histograms, `by_class` and the bounded `by_tenant` array).
* `GET /metrics` (with `--health-port`) appends the Prometheus series
  `aura_offload_submitted_total`, `aura_offload_rejected_total{reason=…}`,
  `aura_offload_completed_total{status=…}`,
  `aura_offload_stale_completion_dropped_total`,
  `aura_offload_sync_fallback_total`, the gauges `aura_offload_queue_depth`,
  `aura_offload_running`, `aura_offload_threads`,
  `aura_offload_queue_depth_peak`, the histograms
  `aura_offload_{queue_wait,exec,wake_latency}_us`, and the per-class /
  per-tenant series.

Label cardinality is fixed: three classes and at most eight tenant slots.
`queue_depth` returns to zero once every accepted job reaches a terminal
state. Outside `--serve-async` the section is present with zeros.
