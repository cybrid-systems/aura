# Mutate API — Atomic Batch Strong Atomicity (Issue #250 / #394)

`mutate:atomic-batch` composes multiple EDSL mutation primitives into a
single **all-or-nothing** transaction on the active `FlatAST` workspace.
Issue #250 made the batch **strongly atomic** at the generation /
observability layer: intermediate per-op bumps are suppressed and
consolidated into one commit-time bump.

## MutationBoundaryGuard::try_acquire (Issue #1547 / #1628)

**Production path** for mutation boundaries is the typed factory — not
the legacy RAII ctor (deprecated; soft-fails inert on quota reject).

```cpp
auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &ok);
if (!gr) {
  // gr.error().kind == AuraErrorKind::ResourceQuotaExceeded
  // gr.error().message contains "mutation quota exceeded"
  // metrics: resource_quota_rejects_total++, mutation_guard_try_acquire_reject_total++
  return surface_error(gr.error());
}
auto guard = std::move(*gr);
// ... mutate under guard ...
```

| Call site | Status |
|-----------|--------|
| `CompilerService::typed_mutate` | **try_acquire** (#1547/#1628) |
| `CompilerService::eval_on_current` | **try_acquire** (#1547/#1628) |
| `MUTATION_BOUNDARY_PROTECT` macro | **try_acquire** |
| Legacy `MutationBoundaryGuard(ev, &ok)` | `[[deprecated]]` — inert soft-fail |

**Never** throws `runtime_error` / PanicCheckpoint for mutation quota.
Agents should retry/backoff on `ResourceQuotaExceeded`.

Metrics: `query:resource-quota-stats` schema **1628** —
`mutation_guard_try_acquire_total`, `mutation_guard_try_acquire_reject_total`,
`try_acquire_wired`, `panic_checkpoint_quota_replaced`.

## Stdlib safety bridge (Issue #1553)

Engine P0 mechanisms (`MutationBoundaryGuard::try_acquire` + quota
#1547/#1628, safe yield #1504, arena quota #1546) are exposed to Agents via
`lib/std/mutate.aura`:

| API | Role |
|-----|------|
| `(mutate:boundary-depth)` | Guard nesting (0 = yield/steal safe) |
| `(mutate:boundary-safe?)` | `#t` when depth==0 and not held |
| `(mutate:safe-yield)` | Wraps `(ast:yield-at-boundary)` / safe-yield stats |
| `(mutate:quota-ok?)` | Soft `resource:quota-check` probe |
| `(mutate:safety-snapshot)` | Alist of boundary / yield / fiber / quota signals |
| `(mutate:atomic-batch-safe ops [summary])` | Refuse if not boundary-safe; yield then atomic-batch |

**Agent rule:** prefer `mutate:atomic-batch-safe` (or `std/agent`
closed-loop, which already uses it) over raw `mutate:atomic-batch` in
multi-fiber / orchestrator loops. Never call cooperative yield while a
Guard is held — the engine skips (`skipped-held`); stdlib checks
`boundary-safe?` first.

See also `docs/design/agent-decision-metrics.md` (#1553 fold-ins) and
`orch:parallel` / `orch:parallel-with-yield` in `lib/std/orchestrator.aura`.

## Strong atomicity guarantee

When `(mutate:atomic-batch ops summary)` runs successfully:

1. `FlatAST::begin_atomic_batch()` sets `bump_generation_suppressed_`.
2. Each supported sub-op (all 14 below) runs through **lockless helpers**
   that mutate the flat without acquiring a nested `MutationBoundaryGuard`
   (avoids deadlock on the non-recursive `workspace_mtx_`).
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

The outer `MutationBoundaryGuard` (acquired at batch entry, released at
batch exit) holds `workspace_mtx_` as a `std::unique_lock<std::shared_mutex>`
member for the **entire** outermost Guard lifetime — only the outermost
Guard actually acquires the lock (nested guards are detected via a
thread-local depth counter and skip the acquire, per the #236 nesting
rule). This means the batch is also **strongly atomic against concurrent
mutators**: any other fiber / thread that tries to acquire `workspace_mtx_`
unique_lock during the batch waits until the batch commits or rolls back.
`atomic_batch_interleaved_mutation_prevented` (Issue #1900 AC3) counts how
many strong-atomicity sessions ran.

## Supported sub-ops (Issue #1900 dispatch expanded 5 → 14)

All 14 lockless helpers live in `evaluator_eval_flat.cpp` and are
extracted from the wrapper primitives, stripped of `MutationBoundaryGuard`
+ `g_fiber_yield_mutation_boundary` + `workspace_read_only_` check +
lazy COW trigger + post-mutate typecheck + linear ownership validation
+ defuse_version_ bumps + dep-graph propagation (those are outer-batch
responsibilities, performed once per batch).

| Sub-op | Lockless helper | Topology atomicity | Status |
|--------|-----------------|--------------------|--------|
| `mutate:rebind` | `eval_flat_apply_mutate_rebind` | **Full** (MutationRecord inverse + Guard `restore_children` + parent rebuild) | #250 / #1441 / #1502 |
| `mutate:replace-value` | `eval_flat_apply_mutate_replace_value` | **Full** (LiteralInt / LiteralFloat / Variable / LiteralString) | **#1900** (was stub) |
| `mutate:tweak-literal` | `eval_flat_apply_mutate_tweak_literal` | **Full** (LiteralInt delta, clamped ≥ 0) | **#1900** (was stub) |
| `mutate:remove-node` | `eval_flat_apply_mutate_remove_node` | **Full** (structural inverse + Guard topology) | **#396 Phase 2** / #1502 |
| `mutate:insert-child` | `eval_flat_apply_mutate_insert_child` | **Full** (structural inverse + Guard topology) | **#396 Phase 2** / #1502 |
| `mutate:set-body` | `eval_flat_apply_mutate_set_body` | **Full** (Define → Lambda body replacement) | **#1900** |
| `mutate:replace-pattern` | `eval_flat_apply_mutate_replace_pattern` | **Full** (pattern match + replacement, Kleene single-subtree in batch context) | **#1900** |
| `mutate:replace-subtree` | `eval_flat_apply_mutate_replace_subtree` | **Full** (subtree swap + hygiene gate) | **#1900** |
| `mutate:splice` | `eval_flat_apply_mutate_splice` | **Full** (variadic code-string insertion) | **#1900** |
| `mutate:wrap` | `eval_flat_apply_mutate_wrap` | **Full** (sentinel-placeholder substitution) | **#1900** |
| `mutate:rename-symbol` | `eval_flat_apply_mutate_rename_symbol` | **Full** (Variable/Define/DefineType/DefineModule + Lambda params) | **#1900** |
| `mutate:move-node` | `eval_flat_apply_mutate_move_node` | **Full** (cycle-checked subtree move) | **#1900** |
| `mutate:inline-call` | `eval_flat_apply_mutate_inline_call` | **Full** (body clone + param substitution) | **#1900** |

Any other mutate primitive name (e.g., a future `mutate:foo`) hits the
unsupported-op else branch, which bumps `atomic_batch_unsupported_op_total`
(Issue #1900 AC3) and aborts with `batch-unsupported-op` listing the 14
supported names. This counter is a forward-compatibility signal: when it
stays at 0 in production telemetry, the dispatch table is complete; when
it increments, a new sub-op has landed before its lockless helper.

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