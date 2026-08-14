# Primitive error-return convention (Issue #2914)

All registered Aura primitives (`evaluator_primitives_*.cpp`) share one
error-return convention so Agents, `query:primitives-meta`, and diagnostics
observe failures consistently.

## Preferred paths

| Situation | Return | Counter |
|-----------|--------|---------|
| Recoverable failure Agents should observe | `make_primitive_error(...)` or structured `make_merr(kind, msg)` | bump `primitive_error_counter` (via `make_primitive_error` / PRIM_ERROR) |
| Pure predicate false | `make_bool(false)` | none |
| Empty list / missing optional lookup with documented empty semantics | `make_void()` or empty pair list | none (document at call site) |
| Hard programmer error / invariant | `make_primitive_error` with clear message | yes |

## Disallowed

- Silent `make_void()` / `make_bool(false)` for **true error cases** (bad arg types that should be diagnosed, I/O failures that abort the operation, type mismatches on mutating ops).
- Returning success shapes on failure without an error value.

## S0 / EDSL notes

- Predicate primitives (`*?`, `equal?`, …) keep boolean returns.
- Empty-collection returns (`car` on empty via error, `list-ref` OOB → error) prefer `make_primitive_error`.
- Stats facades (`ObservabilityPrims::register_stats_impl`) may return `make_int(0)` / empty hashes when metrics are unwired — that is “no sample”, not an error.

## Residual core-surface rules (Issue #2998)

Audit of list / math / json / pair / vector after #2914. True errors
always go through `make_primitive_error` (counter bumped). Predicates
stay boolean. Documented empty / not-found stay silent only as below.

| Primitive | Documented empty / not-found | True error (`make_primitive_error`) |
|-----------|------------------------------|-------------------------------------|
| `member` | not-found → int `0` (falsy) | too few args, not a list, improper / corrupted |
| `list-ref` | — | too few args, non-int / negative index, OOB, corrupted (including empty list at index) |
| `take` / `drop` | `n=0`, empty list, drop past end → void | too few args, non-int / negative count, not a list, improper / corrupted |
| `filter` / `map` | empty list → void | too few args, not a list |
| `reverse` | empty list → void | too few args, not a list, improper / corrupted |
| `car` / `cdr` | — | not a pair, corrupted pair index (no silent `0`) |
| `cadr` family | missing cdr / non-pair → void (stdlib guards with `pair?`) | — |
| `vector-length` / `vector->list` | — | not a vector, corrupted index |
| `modulo` / `mod` / `quotient` / `remainder` / `abs` | — | too few args, non-numeric, div-by-zero, INT64_MIN/-1 |
| `inexact->exact` | out-of-range float saturates (documented #1153) | too few args, non-number |
| `json-parse` | JSON `null` → void | non-string arg, unexpected EOF, invalid token |
| `json-get-string` | missing field → void | non-string args |

## Discovery

- `query:primitives-meta` / `primitive:describe` / `engine:metrics` remain the Agent surfaces for names and counters.
- See also `docs/generated/primitives.md` inventory.
