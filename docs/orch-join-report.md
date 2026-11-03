# orch join report schema

Issue #4832. `orch:agent-join` and `orch:scope-join-all` return a hash whose
default key set is a stable, documented API. Development markers named
`issue-NNNN`, `schema-NNNN`, and `*-wired` are diagnostics, not business
fields: they used to be mixed into the same layer, drifted with every issue,
and made up roughly half of a scope-join report.

Pass `:debug #t` (or `:verbose #t`) to get the diagnostic view; the extra
keys are additive and no default key changes meaning.

## `orch:agent-join name [:timeout-ms n] [:drain-ms n] [:debug bool]`

| key | type | meaning |
| --- | --- | --- |
| `ok` | bool | join completed and the body did not fail |
| `status` | string | `ok` / `timeout` / `cancelled` / `invalid` / `reclaimed` / `error` |
| `wait-us` | int | microseconds spent waiting |
| `drain-ms` | int | cancel/drain window applied |
| `schema` | int | report schema version |
| `error` | string | body failure message (only when the body failed) |

`:debug #t` adds the Reclaimed/identity-plane markers (`still-running`,
`reclaim-age-ms`, `schema-*`, `issue-*`, `*-wired`).

## `orch:scope-join-all [:timeout-ms n] [:drain-ms n] [:tree bool] [:debug bool]`

| key | type | meaning |
| --- | --- | --- |
| `ok` | bool | batch joined, no guard deny, no failed body |
| `status` | string | aggregate status (`ok` / `partial` / `timeout` / `cancelled` / `invalid` / `reclaimed`) |
| `err-count` | int | number of failed bodies |
| `errors` | list | `(name . message)` pairs, spawn-completion order |
| `body-ok-count` | int | bodies that finished successfully |
| `cancelled-count` | int | bodies cancelled during this join |
| `wait-us` | int | microseconds spent waiting |
| `drain-ms` | int | cancel/drain window applied |
| `schema` | int | report schema version |
| `reclaimed-count` | int | handles whose body is still running |
| `must-wait-count` | int | handles that still owe a reclaimed wait |
| `reservation-held-count` | int | handles still holding a reservation |
| `cleanup-pending-count` | int | handles that still owe the SSOT second-wait |
| `wait-reclaimed` | bool | any per-handle auto-wait ran |
| `wait-timeout` | bool | any per-handle auto-wait timed out |
| `on-join-fail-effective` | string | effective failure policy |
| `join-fail-action-taken` | int | failure action actually taken |
| `restart-attempted` / `restart-skipped-no-spec` / `restart-ok` | int | RestartN counters |
| `restart-max` | int | restart fuel configured for this join (`:max-restarts`); 0 when no restart policy is active |
| `restart-exhausted` | int | children that ran out of restart fuel (reached `restart-max` or the consecutive-stall circuit opened) |
| `attempt-errors` | int | failed body attempts, including attempts that a restart later recovered; `err-count` / `errors` report final failures only |
| `restart-denied` / `restart-deny-class` | int / string | spawn-admit deny |
| `tree` | bool | tree join requested (default local) |
| `tree-settled` / `descendants-live` | bool | tree-settlement blame fields |
| `swept-cleaned` / `swept-still-pending` | int | end-of-join Reclaimed sweep |
| `isolation-level`, `region-key-missing`, `region-key-missing-serialized`, `distinct-region-keys` | string / bool / int / int | isolation observation |

`:debug #t` additionally exposes the `schema-*` / `issue-*` / `*-wired`
self-test sentinels.

## Example: a `restart-n` join whose body failed once and recovered

```lisp
(define *n* 0)
(orch:scope-spawn "flaky" (lambda () (set! *n* (+ *n* 1)) (if (< *n* 2) (error "boom") 42)))
(define r (orch:scope-join-all :timeout-ms 2000 :on-join-fail "restart-n" :max-restarts 1))
```

The body failed its first attempt and succeeded on the restart, so the
report is:

| key | value |
| --- | --- |
| `ok` | `#t` |
| `status` | `ok` |
| `err-count` | `0` (no final failure) |
| `errors` | `()` |
| `attempt-errors` | `1` (the failed attempt, recovered by the restart) |
| `restart-attempted` | `1` |
| `restart-ok` | `1` |
| `restart-max` | `1` (the `:max-restarts` fuel for this join) |
| `restart-exhausted` | `0` (the child never ran out of fuel) |
| `body-ok-count` | `1` |

`restart-max` and `restart-exhausted` are counted PER CHILD (the fuel
one child may spend), while `attempt-errors` counts failed ATTEMPTS
across all children. Neither feeds `err-count` / `errors`, which stay
on final failures only: a child that exhausted its fuel shows up in
`restart-exhausted` and in `errors` / `err-count`, not in
`attempt-errors` alone.
