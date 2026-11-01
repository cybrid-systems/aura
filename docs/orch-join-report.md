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
| `restart-denied` / `restart-deny-class` | int / string | spawn-admit deny |
| `tree` | bool | tree join requested (default local) |
| `tree-settled` / `descendants-live` | bool | tree-settlement blame fields |
| `swept-cleaned` / `swept-still-pending` | int | end-of-join Reclaimed sweep |
| `isolation-level`, `region-key-missing`, `region-key-missing-serialized`, `distinct-region-keys` | string / bool / int / int | isolation observation |

`:debug #t` additionally exposes the `schema-*` / `issue-*` / `*-wired`
self-test sentinels.
