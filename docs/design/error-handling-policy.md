# Error Handling Policy (Issues #807 / #809)

> **Authoritative policy:** Hot paths (eval, mutation, fiber switch, JIT
> execution) must use `AuraResult` / `EvalResult` / `std::expected`.
> Exceptions are allowed only for truly exceptional situations (OOM,
> initialization failures, hard invariant violations).

This document is the single source of truth for error-handling decisions.
Also see [`core/exception_policy.md`](core/exception_policy.md) for the
hot-path audit table and migration phases.

## Rules

| Context | Preferred | Exceptions allowed? |
|---------|-----------|---------------------|
| Evaluator hot loops / mutate / Guard | `EvalResult` → `AuraResult` | **No** |
| Fiber / scheduler **init** (mmap, eventfd, epoll) | Prefer `VoidResult`; Phase 1 still may throw | Yes at process startup |
| GapBuffer bounds | `out_of_range` | Yes — API contract |
| JIT guest `Raise` | controlled EH bridge | Yes — language feature |
| Internal JIT / runtime errors | `AuraResult` | **No** |
| Contract / assert failure | map to `InternalContractFailure` | Prefer Result; hard abort only if unrecoverable |

## Interop layer (#809)

| Helper | Location | Role |
|--------|----------|------|
| `map_kind_name` | `aura.core.error` | diag kind string → `AuraErrorKind` |
| `make_unexpected_from_kind_name` | `aura.core.error` | build `std::unexpected<AuraError>` |
| `make_aura_error` | `aura.core.error` | build `AuraError` value |
| `CompilerService::eval_as_aura_result` | `service.ixx` | public eval → `AuraResult` |
| `CompilerService::eval_result_to_aura` | `service.ixx` | convert without re-eval |
| `aura_error_bridge.h` | compiler | layering note (core must not import diag) |

Full `Diagnostic` struct conversion stays out of `error.ixx` (no circular deps).

## Observability

- `(query:error-handling-policy-stats)` schema **809**
  - `interop-conversions`, `contract-as-aura-error`, `policy-doc-active`, `hot-path-uses-result`

## Related issues

| Issue | Topic |
|-------|-------|
| #807 | Initial audit + interop helpers |
| #808 | Evaluator / CompilerService AuraResult Phase 1 |
| #809 | Formal policy + docs + interop enforcement (this doc) |
| #810 | Fiber/Scheduler init |
| #811 | JIT exception bridge classification |
| #813 | MutationBoundaryGuard AuraResult path |
