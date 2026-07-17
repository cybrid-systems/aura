# Mutate API — Atomic Batch Strong Atomicity (Issue #250 / #394)

`mutate:atomic-batch` composes multiple EDSL mutation primitives into a
single **all-or-nothing** transaction on the active `FlatAST` workspace.
Issue #250 made the batch **strongly atomic** at the generation /
observability layer: intermediate per-op bumps are suppressed and
consolidated into one commit-time bump.

## Strong atomicity guarantee

When `(mutate:atomic-batch ops summary)` runs successfully:

1. `FlatAST::begin_atomic_batch()` sets `bump_generation_suppressed_`.
2. Each supported sub-op (`mutate:rebind`, `mutate:replace-value`,
   `mutate:tweak-literal`) runs through **lockless helpers** that mutate
   the flat without acquiring a nested `MutationBoundaryGuard` (avoids
   deadlock on the non-recursive `workspace_mtx_`).
3. Per-op generation bumps are **skipped** while the batch is open.
4. `FlatAST::commit_atomic_batch()` performs **one** generation bump and
   records how many per-op bumps were suppressed.
5. On any failure, `rollback_since` + `rollback_atomic_batch` restore the
   pre-batch flat state and increment `atomic_batch_rollbacks_`.

**Observable consequence:** concurrent readers (another fiber / thread
calling `ast:generation`, `query:*`, etc.) see either the **pre-batch**
or **post-batch** workspace generation — never a sequence of intermediate
generation bumps from individual sub-ops inside the batch.

This is **strong atomicity** for generation / snapshot visibility. It does
**not** mean queries run lock-free inside the batch: readers may block on
`workspace_mtx_` until the batch completes, then observe the committed
state.

## Supported sub-ops

The Aura primitive currently routes:

| Sub-op | Lockless helper | Topology atomicity | Status |
|--------|-----------------|--------------------|--------|
| `mutate:rebind` | `eval_flat_apply_mutate_rebind` | **Full** (MutationRecord inverse + Guard `restore_children` + parent rebuild) | #250 / #1441 / #1502 |
| `mutate:replace-value` | `eval_flat_apply_mutate_replace_value` | n/a (errors out) | #250 stub |
| `mutate:tweak-literal` | `eval_flat_apply_mutate_tweak_literal` | n/a (errors out) | #250 stub |
| `mutate:remove-node` | `eval_flat_apply_mutate_remove_node` | **Full** (structural inverse + Guard topology) | **#396 Phase 2** / #1502 |
| `mutate:insert-child` | `eval_flat_apply_mutate_insert_child` | **Full** (structural inverse + Guard topology) | **#396 Phase 2** / #1502 |

Other mutate primitives still return `batch-unsupported-op` rather than
deadlocking on a nested guard. `mutate:replace-value` and
`mutate:tweak-literal` are intentional stubs (would need lockless
extraction from the wrapper primitives — the existing
`eval_flat_apply_mutate_*` stubs are TODO and not part of #396 scope).

## Structural topology rollback (Issue #1502)

On batch failure (sub-op error or unsupported op), atomicity is enforced
at **three layers**:

1. **MutationRecord inverse** — `rollback_since` / `rollback_to_size`
   walks the log in reverse and applies
   `try_rollback_structural_child_op` / `try_rollback_rebind_op`, which
   restore both `children_` slots and `parent_` back-links for logged ops.
2. **Explicit parent rebuild** — before Guard exit, the batch fail path
   calls `FlatAST::rebuild_parent_links_from_children()` so
   `parent_of()` cannot diverge from `children()` even if an inverse
   partially fails.
3. **Guard `restore_children`** — `MutationBoundaryGuard` dtor with
   `success=false` reinstalls the pre-batch PCV snapshot and **again**
   rebuilds `parent_` from the restored child lists
   (`children_topology_restore_count_` + `parent_topology_restore_count_`).

Also on fail: `linear_post_mutate_enforce_all()` so EnvFrame linear
ownership is not left half-applied after AI multi-step aborts.

**Fully atomic (topology):** `rebind`, `remove-node`, `insert-child`
(under atomic-batch + Guard).  
**Best-effort / not batchable yet:** `replace-value`, `tweak-literal`,
and any unextracted mutate op → fail-fast `batch-unsupported-op` with
full topology restore of prior successful sub-ops in the same batch.

Observability: `(stats:get "atomic-batch:stats")` exposes
`children-topology-restore`, `parent-topology-restore`, and `schema` 1502;
`(stats:get "ast:generation-stats")` also has `parent-topology-restore`.

## Observability — `(atomic-batch:stats)`

`(atomic-batch:stats)` returns a hash with lifetime counters:

| Key | Meaning |
|-----|---------|
| `batch-count` | Successful commits |
| `ops-total` | Sub-ops executed across all batches |
| `rollback-count` | Rolled-back batches |
| `ops-per-batch` | `ops-total / batch-count` (integer avg) |
| `bumps-saved-total` | Per-op generation bumps suppressed by batching |
| `executed-under-concurrent-fiber` | **#396 Phase 3** — commits that ran while the bridge fiber setter was active (heuristic for "ran under concurrent fiber pressure"). 0 in non-serve / test paths. |

### Reading `bumps-saved-total` for dashboards

After a multi-op batch, `bumps-saved-total` should increase by roughly
`(N - 1)` for `N` successful sub-ops (one bump at commit instead of
`N` per-op bumps):

```scheme
(begin
  (set-code "(define x 0)")
  (eval-current)
  (mutate:atomic-batch
    (list
      (list "mutate:rebind" "x" "1" "a")
      (list "mutate:rebind" "x" "2" "b")
      (list "mutate:rebind" "x" "3" "c"))
    "demo")
  (hash-ref (atomic-batch:stats) "bumps-saved-total"))
```

Issue #394 fixed a pre-existing `hash-ref` bug (capacity-8 hash table
with 5 keys) so all keys above return integers, not `()`.

Related integer shortcut (compiler snapshot): `(query:atomic-batch-stats)`.

## Trade-offs vs weak atomicity

| Aspect | Weak (pre-#250) | Strong (#250+) |
|--------|-----------------|----------------|
| Generation bumps | One per sub-op | One per batch commit |
| Concurrent query | May observe partial sequence of bumps | Pre- or post-batch only |
| Rollback | Per-op log rollback | Full batch rollback |
| Fiber safety | Intermediate states visible | No torn generation reads |
| Cost | Finer-grained invalidation | Coarser single invalidation |

**When to prefer atomic batch:** AI multi-step self-edit loops where
several mutations must appear together (rebind + replace + tweak) and
downstream agents must not act on half-applied edits.

## Fiber safety — Issue #396 Phase 1

When a batch runs inside a fiber context, the bridge setter
`g_fiber_set_yield_reason_mutation_boundary` is invoked on
`MutationBoundaryGuard` entry. This sets the current fiber's
`last_yield_reason_` to `MutationBoundary` (lightweight — does NOT
actually yield), so work-stealing decisions (`Fiber::is_stealable()`)
see the fiber as being at a mutation boundary.

**Effect:** other fibers doing work-stealing during a batch will treat
the batching fiber as stealable (the work-stealing scheduler
distinguishes stealable `MutationBoundary` from non-stealable
`BlockingIO` / `SchedulerSteal` reasons), which is the correct
semantic — the batch's per-op mutations look like one atomic mutation
boundary to the scheduler.

In test-binary / non-serve paths, the bridge setter is null and the
batch is a no-op for fiber state (no fiber scheduler running).

**When weak per-op semantics still matter:** single mutations outside a
batch still bump generation per op — use those when you want incremental
invalidation between small, independent edits.

## Concurrent fiber safety

Multi-fiber / multi-thread orchestration should:

1. Treat `mutate:atomic-batch` as a **single critical section** on the
   workspace write lock.
2. Run queries from other fibers expecting **blocking** until the batch
   commits (then observe committed state).
3. Use `Fiber::yield(YieldReason::MutationBoundary)` between separate
   eval calls — **not** inside an open batch on the mutating fiber.
4. Re-validate `StableNodeRef` after any batch; see
   [stable_ref_best_practices.md](stable_ref_best_practices.md).

Regression coverage: `tests/test_issue_394.cpp`.

## See also

- Issue #192 — `mutate:atomic-batch` introduction
- Issue #250 — strong atomicity + `bumps-saved-total`
- Issue #394 — `hash-ref` fix + concurrent fiber tests
- [api-reference.md](../../api-reference.md) — primitive index