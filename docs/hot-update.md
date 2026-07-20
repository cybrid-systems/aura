# Hot-Update MVP Scope — Issue #1943

> Status: **MVP locked**. Out-of-scope paths are explicitly deferred — see
> "Explicitly out of scope" below. AI agents and tools that target Aura
> hot-update should only rely on the **in-scope** capabilities.

## Goal

Narrow the supported hot-update capability to a controllable MVP so the
mutation + hot-update loop can be stabilized before adding more ambitious
mechanisms. This is Phase 1 of the [#1942 Simplification Roadmap](../README.md)
and follows the same philosophy as the rest of the project: **stabilize the
core first, then expand**.

## In scope (MVP)

The MVP supports the following hot-update operations on a single workspace
where no concurrent fiber steal is in progress:

1. **Replace function body of a defined function** — `(mutate:set-body ...)`
   / `(engine:redefine ...)` rebuilds the function's IR and re-emits the
   single affected define. Bumps the workspace's `generation_` /
   `subtree_generation_` for the affected subtree only.

2. **Basic closure refresh / invalidation** — closures directly referencing
   the changed function are invalidated via the existing
   `compact_env_frames` / `truncate_env_frames_to_checkpoint` path
   (see `evaluator.ixx` + `evaluator_compact.cpp`). Closures captured in
   other workspaces are NOT migrated.

3. **Simple dirty propagation + re-lower + re-emit for the changed region**
   — `mark_block_dirty!` / `clear_block_dirty!` (per-block) are the
   supported granularity. `compile:relower-strategy` reports the strategy
   chosen (`none` / `incremental` / `full`).

## Explicitly out of scope (deferred)

The following are intentionally NOT supported in the MVP. They remain
documented as future work and have related open issues:

- **Complex cross-closure migration during hot-update** —
  closures captured in multiple workspaces or across COW boundaries
  are NOT migrated atomically. See [#1929], [#1947], [#1954] for the
  closure lifetime hardening work, which is prerequisite to any
  cross-closure migration.

- **Hot-update while a fiber is stolen or in complex agent orchestration** —
  `MutationBoundaryGuard` enforces depth==0 for the outermost Guard, but
  nested Guards do NOT block re-lowering. See [#1931], [#1950], [#1953]
  for the systemic MutationBoundaryGuard enforcement work. Until those
  land, hot-update during agent orchestration is unsafe.

- **Cross-workspace stable `DefineId` persistence / COW migration** —
  single-workspace process-stable **name→func_id** map for
  `aura_reemit_aot_for_dirty` landed in [#1930] / [#1952]. Persisting
  DefineId across workspace migration / COW boundaries remains deferred.

- **Hot-update of closures captured across COW workspace boundaries**
  (advanced scenarios) — see [#1916] for `EnvFrame` SoA + `bridge_epoch`
  prerequisites.

- **Per-instruction dirty tracking** — `compile:mark-instruction-dirty!` /
  `compile:clear-instruction-dirty!` / `compile:is-instruction-dirty?`
  are present in the primitive surface but NOT covered by the MVP
  correctness contract. They remain useful as observability primitives.

## Related primitives

### MVP-supported (correctness contract)

- `compile:mark-block-dirty!` / `compile:clear-block-dirty!`
- `compile:block-dirty?` / `compile:block-dirty-count`
- `compile:func-block-dirty-count`
- `compile:relower-strategy`
- `compile:snapshot`
- `compile:per-symbol-dirty-stats`
- `compile:dirty-reason-counts`
- `compile:per-defuse-index-add` / `compile:per-defuse-index-callers`
- `compile:per-symbol-reinfer-stats`
- `compile:per-defuse-index-stats`
- `compile:subtree-bump` / `compile:subtree-generation` /
  `compile:subtree-bump-count` (scoped per-subtree generation,
  supported within a single workspace)

### MVP-deferred (observability only, no correctness contract)

- `compile:mark-instruction-dirty!` / `compile:clear-instruction-dirty!` /
  `compile:is-instruction-dirty?` (per-instruction granularity)
- `compile:mark-narrowing-dirty!` / `compile:narrowing-dirty?`
  (type-system dirty bit, not a hot-update primitive per se)
- `compile:clear-macro-dirty!` / `compile:macro-dirty?` /
  `compile:macro-dirty-stats` / `compile:macro-dirty-count`
  (macro-expansion dirty tracking)
- `compile:epoch` (global mutation epoch counter)
- `compile:cache-size` / `compile:dirty-count` / `compile:dep-edges`
  (low-level IR cache stats — observable but not part of MVP contract)

## How to use this doc

- If you are an AI agent driving Aura hot-update: stay within "In scope".
- If you are reviewing a hot-update PR: check that the change does not
  silently expand the supported scope.
- If you are filing a new issue that touches a "deferred" path: link
  the related open issues listed above.

## Related open P0 issues (under the narrowed scope)

These remain open and are pre-requisites or extensions of the MVP:

- [#1929] Fix `make_closure_view` raw pointer lifetime violation *(closed: dual-epoch + schema-1929)*
- [#1930] Complete `aura_reemit_aot_for_dirty` LLVM re-emit pipeline *(closed: single-workspace stable name→func_id + emit callback)*
- [#1931] Systemic `MutationBoundaryGuard` enforcement *(closed: dtor ≤6 atomics + 100% compile/mutate Guard + schema-1931)*
- [#1947] `make_closure_view` copies Closure raw pointer *(closed via #1888/#1926 lifetime stamps)*
- [#1950] `MutationBoundaryGuard` dtor 15+ atomic ops per call *(closed via #1747/#1931 batch)*
- [#1952] Complete `aura_reemit_aot_for_dirty` real LLVM incremental re-emit
- [#1953] Systemic `MutationBoundaryGuard` refine of #1931 *(closed: schema-1953 + gate coverage)*
- [#1954] Strengthen `make_closure_view` lifetime + walk_active_closures *(closed: schema-1954 refine of #1929)*
- [#1955] `compact_env_frames` / `truncate_env_frames_to_checkpoint`
  consistency: Guard + …
- [#1956] Establish `hot_update_registry` unified coordination center
- (and many closed historical hot-update issues, e.g. #1905, #1747,
  #1710, #1707, #1658, #1636, #1627, #1625, #1623, #1616, #1614,
  #1609, #1607)

## Revision history

- 2026-07-20 (#1943): Initial MVP scope document. Confirmed by the
  `test_issue_1943` regression test.