# Prim heap soft quotas (Issue #2916)

Shared soft limits for **pairs / string / vector** heap growth on core
constructors under multi-fiber Agent self-evolution loops (query + mutate +
list/json/string construction).

Default is **unlimited** (limit `0`) so single-fiber hot paths
(`list-ref`, `member`, pure math) are unchanged: they never call the allow
helper.

## Agent API

| Call | Effect |
|------|--------|
| `(resource:quota-set "pairs" N)` | Soft max `pairs_.size()` after growth |
| `(resource:quota-set "strings" N)` | Soft max `string_heap_.size()` after growth |
| `(resource:quota-set "vectors" N)` | Soft max `vector_heap_.size()` after growth |
| `(resource:quota-get "pairs"\|"strings"\|"vectors")` | Configured limit (0 = unlimited) |
| `(engine:metrics "query:prim-heap-quota-stats")` | Pressure + limits (schema **2916**) + lock SLO / recommend (**2997**) |

On breach, participating constructors return **`make_primitive_error`** with a
stable message prefix:

- `prim-heap-quota: pairs soft limit exceeded`
- `prim-heap-quota: strings soft limit exceeded`
- `prim-heap-quota: vectors soft limit exceeded`

## Stats fields (`query:prim-heap-quota-stats`)

| Key | Meaning |
|-----|---------|
| `checks-total` | Allow checks while a limit is set |
| `rejects-total` | Soft-limit breaches |
| `pairs-high-water` / `strings-high-water` / `vectors-high-water` | Peak sizes observed |
| `pairs-limit` / `strings-limit` / `vectors-limit` | Configured soft limits |
| `pairs-size` / `strings-size` | Live sizes |
| `schema` | `2916` |
| `lock-hold-ns` | Accumulated `alloc_storage_lock_` hold time in list-like constructors (#2997) |
| `lock-samples` | Number of timed constructor lock holds |
| `soft-hit-total` | Allow checks that reached ≥70% of a set limit |
| `unlimited-bypass-total` | `allow()` calls with limit unset (no checks/rejects atomics) |
| `recommend` | `0` ok / `1` raise-quota / `2` shrink-fanout (avg hold > 50µs) |
| `schema-2997` | `2997` when lock-SLO fields are present |

Also correlated with existing `(engine:metrics "query:primitives-hotpath-stats")`
(`pair-alloc-total`, `cdr-depth-max`).

## Hot-path (#2997)

`list` / `append` / `reverse` / `map` / json-array: snapshot + `reserve` + one
quota allow under a timed `ListCtorLockHold`. When limits are unset and the
planned growth is ≤ 8 cells, constructors skip `allow()` entirely (still lock
`pairs_` growth — #2651). Set limits stay fail-closed.

4–8 fiber concurrent `(list …)` is measured in `test_pmr_alloc_fiber_safe`
via `slot_lookup_fast` (bypasses `eval_mutex`). `list-ref` / `member` / math
still never call `allow`.

## Participating constructors (growth only)

- **List:** `list`, `append`, `map`, `reverse`
- **Pair/string:** `cons`, `string-append`, `string-join`, `list->string`, `string->list`
- **JSON:** `json-encode`, `json-parse` (string intern + array lists)
- **Vector:** `vector`, `make-vector`

Read-only / non-growing prims (`list-ref`, `member`, `length`, math) do **not**
consult the quota.

## Implementation

- Header contract: `src/compiler/prim_heap_quota.hh`
- Enforcement: `Evaluator::prim_heap_quota_allow` (high-water always; reject only if limit ≠ 0)
- Metrics: `CompilerMetrics::prim_heap_*` in `observability_metrics.h`

## Related

- Arena / process resource quotas: `resource:quota-set "memory"|"fibers"|"time"` (#753 / #1579)
- Error convention: [primitive-error-convention.md](primitive-error-convention.md)
