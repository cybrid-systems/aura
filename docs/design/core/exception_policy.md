# Exception & Error Policy (Issues #807 / #808)

> **Hot paths use `AuraResult` / `EvalResult` — not exceptions.**
> Exceptions are reserved for truly exceptional situations (OOM, init failure,
> hard invariant violations). This is required for fiber safety, StableNodeRef
> generation validity, and predictable AI Agent control loops.

## Policy summary

| Context | Preferred | Allowed exceptions |
|---------|-----------|--------------------|
| Evaluator `eval_flat` / apply / mutate hot loops | `EvalResult` → migrate to `AuraResult` (#808) | none |
| Fiber / scheduler **init** (mmap, eventfd, epoll) | `std::system_error` OK | yes — construction-time |
| GapBuffer bounds checks | `std::out_of_range` OK | yes — programmer error API |
| JIT IR `Raise` / personality | runtime exception model | yes — language feature |
| OOM / allocator failure | `AuraErrorKind::ArenaOutOfMemory` or throw | either; prefer Result |
| Contract assertion failure | map to `InternalContractFailure` | via handler |

## Why not exceptions in eval loops

Quoted from `src/core/error.ixx` (Issue #474):

> AI Agent control loop needs predictable state. Exceptions break fiber / COW
> reference counts / generation invalidation chains on unwind.
> `std::expected` + monadic ops makes the failure path explicit.

## Types

| Type | Module | Role |
|------|--------|------|
| `AuraError` / `AuraResult<T>` / `VoidResult` | `aura.core.error` | Unified core result |
| `Diagnostic` / `Result<T>` | `aura.diag` | Compiler diagnostics |
| `EvalResult` | `aura.compiler.evaluator` | Today’s eval surface (`Result<EvalValue>`) |

### Interop (#807)

- `aura::core::map_kind_name(std::string_view)` — diag kind name → `AuraErrorKind`
- `aura::core::make_unexpected_from_kind_name(...)` — build `std::unexpected<AuraError>`
- `CompilerService::eval_as_aura_result(input)` — **#808 Phase 1** public bridge
- `CompilerService::eval_result_to_aura(EvalResult)` — convert without re-eval

Full `Diagnostic` struct conversion stays out of `error.ixx` (layering: core must not import `aura.diag`).

## Hot-path audit snapshot (#807)

| File | `throw` sites | Classification |
|------|---------------|----------------|
| `evaluator_eval_flat.cpp` | 0 | OK — uses `EvalResult` |
| `evaluator_fiber_mutation.cpp` | 0 | OK |
| `evaluator_primitives_mutate.cpp` | 0 | OK — `mev` / stale-ref errors |
| `service.ixx` | 0 in eval path | OK — use `eval_as_aura_result` for new code |
| `fiber.cpp` | 3× `system_error` (mmap/eventfd/getcontext) | **Keep** — fiber construction |
| `scheduler.cpp` | 1× `system_error` (epoll_create) | **Keep** — scheduler construction |
| `gap_buffer.hh` | 2× `out_of_range` | **Keep** — API contract |
| `aura_jit.cpp` | IR `aura_throw_exception` symbols | **Keep** — language Raise |
| `ir_executor_impl.cpp` | 0 raw throw (UncaughtException diagnostic) | OK |

## Migration plan (#808)

**Phase 1 (this issue):** public `eval_as_aura_result` + converters; no silent throws on failure.

**Phase 2:** `eval_flat` / mutate helpers return `AuraResult` end-to-end; replace nested `if (ok)`.

**Phase 3:** query / typecheck paths; retire dual `EvalResult` where safe.

## References

- `src/core/error.ixx` (Issue #474)
- `docs/design/core/stable_ref_best_practices.md`
- C++ Core Guidelines E.2 / E.5
