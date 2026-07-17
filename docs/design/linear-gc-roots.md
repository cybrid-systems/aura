# Linear GC Root Registration Contract

**Issue:** #1543 (parent #1478 AC #3) · **#1568** boundary consistency · **#1596** runtime refine · **#1599** closed-loop readiness  
**Related:** #763 (counters + `query:linear-ownership-gc-compiler-stats`), #1515 (unified sync), #740 (JIT L2 resync), #1510/#1526 (compact dual-epoch), #1545/#1557 (live-closure scan), #1483/#1493 (adaptive safepoint)  
**Refine maps:** `docs/design/linear-ownership-runtime-1596.md`, `docs/design/linear-gc-closedloop-readiness-1599.md`

## Summary

Compiler-managed **IRClosure / tree-walker Closure + EnvFrame** values that capture linear state must remain reachable as GC roots across mutation and compaction. Registration is **metric-tracked** (not a separate root table): live roots are re-collected via `Evaluator::collect_compiler_managed_gc_roots` under the current `bridge_epoch`.

### Proactive live-closure walk (#1545 / #1557 / #1568)

| API | Role |
|-----|------|
| `Evaluator::walk_active_closures(fn)` | Unique-lock walk of tree-walker `closures_` map |
| `Evaluator::scan_live_closures_for_linear_captures(mark, only_if_moved)` | EnvFrame SoA `!= Untracked` → optional **force-invalid** (`bridge_epoch=0` = logical Drop / safe_fallback) |
| `Evaluator::force_drop_or_mark_invalid(id)` | Named Force Drop: `bridge_epoch=0` + violation audit |
| `Evaluator::enforce_linear_boundary_consistency(path, mark_all)` | **#1568 unified closed-loop**: scan + `linear_post_mutate_enforce_all` + epoch fence + `run_linear_gc_root_audit` |
| `AuraJIT::walk_active_closures(epoch)` | JIT-side bulk stale-fn mark (ResourceTracker pre-evict pairs with `aura_jit_linear_live_closure_scan`) |

| Path | Scan / enforce wiring (#1568) |
|------|-------------------------------|
| `invalidate_function` | pre-cascade scan + revalidate + GC root audit |
| `compact_env_frames` | pre-remap scan + `linear_post_mutate_enforce_all`; post-restamp GC root audit |
| JIT ResourceTracker / hot-swap | `aura_jit_linear_live_closure_scan` + hot-swap audit |
| **fiber steal** | `probe_linear_ownership_on_fiber_steal` → **`enforce_linear_boundary_consistency(FiberSteal, mark_all=true)`** |
| Guard exit / typed_mutate | **`enforce_linear_boundary_consistency(TypedMutate, mark_all=false)`** (Moved only) |
| GC safepoint | `sync_linear_roots_and_bridge_epoch` + audit |

**Force Drop semantics:** there is no separate free of captured heap cells; marking `bridge_epoch=0` forces `is_bridge_stale` / `closure_needs_safe_fallback` so the capture cannot be applied (logical Drop). Moved captures also bump `linear_ownership_violation_prevented`.

### Epoch fence dual-check (#1568)

Inside `enforce_linear_boundary_consistency`, after the linear scan:

1. For each Closure with `bridge_epoch != 0` and `bridge_epoch != current_bridge_epoch` → Force Drop + `linear_epoch_fence_enforce_total`.
2. For linear-capturing frames with `EnvFrame.version_ < defuse_version_snapshot` → Force Drop (stale under concurrent mutate/compact).

## Metrics (issue-facing names)

| Counter | Meaning |
|---------|---------|
| `linear_post_mutate_enforcements` | Per-frame post-mutate checks (`linear_post_mutate_enforce`) |
| `linear_ownership_violation_prevented` | Use-after-move / Moved / epoch-stale intercepts |
| `linear_gc_root_audit_checks_total` | `#1543` consistency audit invocations |
| `linear_boundary_consistency_total` | `#1568` unified enforce entry count |
| `linear_epoch_fence_enforce_total` | Epoch-stale Force Drops |
| `linear_force_drop_total` | Explicit / fence Force Drops |
| `linear_violation_audit_total` | Provenance audit ring entries |
| `linear_ownership_gc_root_registrations_total` | Root re-registration events |
| `linear_ownership_gc_root_stale_hits_total` | Collect skipped a root whose `bridge_epoch` ≠ current |
| `linear_ownership_gc_violations_prevented_total` | Probe caught linear/epoch violation |
| `linear_ownership_gc_env_version_resync_total` | `resync_linear_jit_gc_roots_after_invalidate` ran |
| `linear_live_closure_scans_total` | Proactive scans |
| `linear_live_closures_marked_invalid_total` | Closures force-invalidated by scan |

## Invariant

1. **Register** after any path that remaps `env_id` or bumps dual-epoch so the next GC walk sees fresh stamps.
2. **Logical unregister** = skip at collect time (stale `bridge_epoch`) or clear `Closure::env_id` on compact reclaim — **not** a separate free-list. Stale hits are the unregister signal.
3. **Balance:** over the process lifetime, `env_version_resync ≤ registrations` (every resync bumps reg ≥ 1).
4. **Monotonicity:** the four GC-lifecycle counters never decrease (relaxed atomics, audit snapshots).
5. **#1568 boundary contract:** every outermost mutation boundary, fiber steal, and compact pre-pass must either call `enforce_linear_boundary_consistency` or the equivalent scan+enforce+audit sequence so GC roots and linear ownership stay dual-consistent with the live epoch pair.

## Mutation paths (6)

| # | Path | Register | Unregister / skip | Audit path id |
|---|------|----------|-------------------|---------------|
| 1 | **typed_mutate** / Guard exit / `apply_linear_post_mutate_pipeline_` | Indirect: post-mutate enforce; roots already live | Force Drop on Moved / epoch-stale | `typed_mutate` (0) |
| 2 | **invalidate_function** → `run_linear_ownership_revalidate_after_invalidate` | `sync_linear_roots_and_bridge_epoch` → resync (+reg, +resync) | Collect skips epoch-mismatched | `invalidate_function` (1) |
| 3 | **compact_env_frames** | Remap `env_id` + restamp `bridge_epoch`; +reg by restamped count | Dead frames reclaim; `bridge_epoch=0` left alone | `compact_env_frames` (2) |
| 4 | **JIT hot-swap** / `notify_jit_fn_trackers_batch_deopt_` | Deopt marks; next apply re-validates; audit snapshots | Stale fn trackers not roots | `jit_hot_swap` (3) |
| 5 | **fiber steal** / `probe_linear_ownership_on_fiber_steal` | Full `#1568` enforce (mark_all) | Force Drop linear + epoch-stale | `fiber_steal` (4) |
| 6 | **GC safepoint** / `request_gc_safepoint` immediate | Full `sync_linear_roots_and_bridge_epoch` | Stale skip at collect | `gc_safepoint` (5) |

### Call graph (register-heavy)

```
invalidate_function
  └─ run_linear_ownership_revalidate_after_invalidate
       └─ sync_linear_roots_and_bridge_epoch
            ├─ resync_linear_jit_gc_roots_after_invalidate  (+reg, +resync)
            └─ probe_linear_ownership_at_gc_safepoint
       └─ run_linear_gc_root_audit(Invalidate)

request_gc_safepoint (immediate)
  └─ sync_linear_roots_and_bridge_epoch
  └─ run_linear_gc_root_audit(GcSafepoint)

compact_env_frames
  ├─ scan_live_closures_for_linear_captures(mark_all)
  ├─ linear_post_mutate_enforce_all
  ├─ remap Closure/EnvFrame ids
  ├─ dual-epoch bump + restamp bridge_epoch (skip bridge_epoch==0)
  ├─ +reg (restamped count)
  └─ run_linear_gc_root_audit(Compact)

MutationBoundaryGuard dtor / apply_linear_post_mutate_pipeline_
  └─ enforce_linear_boundary_consistency(TypedMutate, mark_all=false)
       ├─ scan (only_if_moved)
       ├─ linear_post_mutate_enforce_all
       ├─ epoch fence Force Drop
       └─ run_linear_gc_root_audit(TypedMutate)

probe_linear_ownership_on_fiber_steal
  └─ enforce_linear_boundary_consistency(FiberSteal, mark_all=true)
```

## Audit APIs

### GC root registration audit (#1543)

- **C++:** `Evaluator::run_linear_gc_root_audit(path)` → `bool ok`  
  Snapshots counters, checks monotonicity + `resync ≤ reg`, appends ring entry, bumps `linear_gc_root_audit_checks_total`.
- **Query:** `(engine:metrics "query:linear-gc-root-audit-log")`  
  Optional arg: max recent log lines under `log`. Schema **1599** (lineage 1543).
- **Ring:** `kLinearGcRootAuditRingSize` (32).
- **Readiness linkage (#1599):** `query:ai-closedloop-readiness-stats` exports
  `linear-gc-root-audit-checks`, `linear-live-closure-scans`, and
  `mutation_stack_depth_histogram`. Adaptive surface:
  `query:gc-safepoint-adaptive-stats` (schema 1599).

### Linear violation provenance audit (#1568)

- **C++:** `record_linear_violation_audit(path, reason, env_id, closure_id)`  
  Reasons: `Moved=1`, `EpochStale=2`, `ForceDrop=3`.
- **Query:** `(engine:metrics "query:linear-boundary-consistency-stats")`  
  Optional arg: max violation-log lines. Schema `1568`.
- **Ring:** `kLinearViolationAuditRingSize` (64).

## Existing observability

- `query:linear-ownership-gc-compiler-stats` (schema 763) — the four lifecycle counters.
- `query:linear-gc-root-audit-log` (schema 1543) — audit checks + last entry + optional log.
- `query:linear-boundary-consistency-stats` (schema **1568**) — enforce totals + violation provenance log.

## Tests

- `tests/test_issue_1543.cpp` — registration monotonicity, resync balance, multi-path audit, query surface.
- `tests/test_linear_boundary_consistency_1568.cpp` — use-after-move intercept, force drop, stress, query schema 1568.

## Non-goals

- Do not change the linear ownership state machine (`Untracked/Owned/Borrowed/MutBorrowed/Moved`).
- Do not replace the GC mark/sweep algorithm — only root registration consistency + Force Drop.
