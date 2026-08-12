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

## Discovery

- `query:primitives-meta` / `primitive:describe` / `engine:metrics` remain the Agent surfaces for names and counters.
- See also `docs/generated/primitives.md` inventory.
