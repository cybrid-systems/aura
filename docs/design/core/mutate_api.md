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

The Aura primitive currently routes only:

| Sub-op | Lockless helper |
|--------|-----------------|
| `mutate:rebind` | `eval_flat_apply_mutate_rebind` |
| `mutate:replace-value` | `eval_flat_apply_mutate_replace_value` |
| `mutate:tweak-literal` | `eval_flat_apply_mutate_tweak_literal` |

Other mutate primitives return `batch-unsupported-op` rather than
deadlocking on a nested guard.

## Observability — `(atomic-batch:stats)`

`(atomic-batch:stats)` returns a hash with lifetime counters:

| Key | Meaning |
|-----|---------|
| `batch-count` | Successful commits |
| `ops-total` | Sub-ops executed across all batches |
| `rollback-count` | Rolled-back batches |
| `ops-per-batch` | `ops-total / batch-count` (integer avg) |
| `bumps-saved-total` | Per-op generation bumps suppressed by batching |

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