# EDSL Mutation Contract — Guard Mandatory

**Issue:** #1444 — Force all `mutate:*` paths to wrap a `MutationBoundaryGuard`
**Status:** Shipped (commit TBD — fill on close)
**Author:** Ani (agent)
**Date:** 2026-07-15

## Contract

Every `mutate:*` primitive in `src/compiler/evaluator_primitives_mutation.cpp`
**must** wrap its body in a `MutationBoundaryGuard` (RAII). The Guard:

- acquires `workspace_mtx_` (shared → exclusive upgrade) on ctor for the
  outermost call;
- bumps `defuse_version_` and `mutation_boundary_held_` flag on enter;
- saves a panic checkpoint (`save_panic_checkpoint()`);
- releases the lock and restores the checkpoint on dtor;
- on failure path, rolls back linear ownership + restores the checkpoint.

## Auto-wrap Helper

For new primitive authors, use:

```cpp
AURA_MUTATION_BOUNDARY_PROTECT(ev, [&]{
    // body — operates on `ev` already Guard-wrapped
});
```

This expands to:

```cpp
bool ok = true;
{
    aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
    // body
}
// ok is false if the body or any nested call failed
```

## Naked-Mutate Detection

The evaluator detects naked mutate calls (`mutation_boundary_depth() == 0`
when a `mutate:*` primitive is dispatched) and increments the
`naked_mutate_attempt` atomic. A naked path is a **bug**: the dispatch site
skipped the Guard. Investigate immediately; never silence.

Coverage surface:

- `(query:mutation-boundary-coverage-stats)` — hash with
  `naked-mutate-attempt`, `boundary-depth`, `boundary-held`,
  `threshold-us`, `strict-mode`, `starvation-prevented`,
  `last-long-mutation-fiber-id`, `last-long-mutation-duration-us`,
  `max-extreme-mutation-us`, `long-mutation-extreme-total`,
  `schema=1444`.
- `(query:mutation-boundary-hold-stats)` — hold histogram + yield/migration
  counters (complements #1373/#1375).
- `(query:naked-mutate-attempt)` — single-field shortcut (#1259).

## Audit

Production-side audit (sampled at code-review time):

| Primitive family | File | Guard? |
|---|---|---|
| `(mutate:set!)` | evaluator_primitives_mutate.cpp:509 | ✅ |
| `(mutate:define)` | evaluator_primitives_mutate.cpp:584 | ✅ |
| `(mutate:rebind)` | evaluator_primitives_mutate.cpp:707 | ✅ |
| `(mutate:set-car!)` / `(mutate:set-cdr!)` | evaluator_primitives_mutate.cpp:1368 | ✅ |
| `(mutate:append!)` | evaluator_primitives_mutate.cpp:1726 | ✅ |
| `(mutate:define-macro)` | evaluator_primitives_mutate.cpp:1947 | ✅ |
| `(mutate:install!)` | evaluator_primitives_mutate.cpp:2022 | ✅ (fine_rollback) |
| `(mutate:patch!)` | evaluator_primitives_mutate.cpp:2108 | ✅ |

Sample test: `tests/test_mutation_boundary_full_coverage.cpp` — verifies
`naked-mutate-attempt` stays at 0 across `set!` + `set-car!` cycles.

## Exception Paths

The following are **intentionally NOT** Guard-wrapped:

- Read-only observation primitives (`query:*` namespace) — never mutate.
- Pure value-dispatch (`engine:metrics`) — no workspace write.
- Diagnostics primitives (`repl:*`, `trace:*`) — read-only by contract.

## Coordination

- `#1447` (atomic-batch primitive) — internal single outermost Guard,
  nested calls are batch-aware (depth counter advances without re-acquiring
  the write lock).
- `#1443` (long-mutation policy) — feeds `starvation-prevented` +
  `strict-mode` telemetry into this coverage primitive's payload.
- `#1445` (steal-boost scheduler hook) — consumes Guard-exit hook to
  promote waiters after a long-held boundary drops.
