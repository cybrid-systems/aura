# Linear GC Root Registration Contract

**Issue:** #1543 (parent #1478 AC #3)  
**Related:** #763 (counters + `query:linear-ownership-gc-compiler-stats`), #1515 (unified sync), #740 (JIT L2 resync), #1510/#1526 (compact dual-epoch)

## Summary

Compiler-managed **IRClosure / tree-walker Closure + EnvFrame** values that capture linear state must remain reachable as GC roots across mutation and compaction. Registration is **metric-tracked** (not a separate root table): live roots are re-collected via `Evaluator::collect_compiler_managed_gc_roots` under the current `bridge_epoch`.

| Counter | Meaning |
|---------|---------|
| `linear_ownership_gc_root_registrations_total` | Root re-registration events (invalidate resync, compact restamp, manual bump) |
| `linear_ownership_gc_root_stale_hits_total` | Collect skipped a root whose `bridge_epoch` ≠ current |
| `linear_ownership_gc_violations_prevented_total` | Probe caught linear/epoch violation |
| `linear_ownership_gc_env_version_resync_total` | `resync_linear_jit_gc_roots_after_invalidate` ran |
| `linear_gc_root_audit_checks_total` | `#1543` consistency audit invocations |

## Invariant

1. **Register** after any path that remaps `env_id` or bumps dual-epoch so the next GC walk sees fresh stamps.
2. **Logical unregister** = skip at collect time (stale `bridge_epoch`) or clear `Closure::env_id` on compact reclaim — **not** a separate free-list. Stale hits are the unregister signal.
3. **Balance:** over the process lifetime, `env_version_resync ≤ registrations` (every resync bumps reg ≥ 1).
4. **Monotonicity:** the four counters never decrease (relaxed atomics, audit snapshots).

## Mutation paths (6)

| # | Path | Register | Unregister / skip | Audit path id |
|---|------|----------|-------------------|---------------|
| 1 | **typed_mutate** / `apply_linear_post_mutate_pipeline_` | Indirect: post-mutate enforce only; roots already live | None | `typed_mutate` (0) |
| 2 | **invalidate_function** → `run_linear_ownership_revalidate_after_invalidate` | `sync_linear_roots_and_bridge_epoch` → `resync_linear_jit_gc_roots_after_invalidate` (+reg, +resync) | Collect skips epoch-mismatched | `invalidate_function` (1) |
| 3 | **compact_env_frames** | Remap `env_id` + restamp `bridge_epoch`; +reg by restamped count | Dead frames reclaim; dangling `env_id` → `NULL_ENV_ID` | `compact_env_frames` (2) |
| 4 | **JIT hot-swap** / `notify_jit_fn_trackers_batch_deopt_` | Deopt marks; next apply re-validates; audit snapshots | Stale fn trackers not roots | `jit_hot_swap` (3) |
| 5 | **fiber steal** / `probe_linear_ownership_on_fiber_steal` | Probe only; violations → metrics | — | `fiber_steal` (4) |
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
  ├─ remap Closure/EnvFrame ids
  ├─ dual-epoch bump + restamp bridge_epoch
  ├─ +reg (restamped count)
  └─ run_linear_gc_root_audit(Compact)
```

## Audit API

- **C++:** `Evaluator::run_linear_gc_root_audit(path)` → `bool ok`  
  Snapshots counters, checks monotonicity + `resync ≤ reg`, appends ring entry, bumps `linear_gc_root_audit_checks_total`.
- **Query:** `(engine:metrics "query:linear-gc-root-audit-log")`  
  Optional arg: max recent log lines under `log`. Schema `1543`.
- **Ring:** `kLinearGcRootAuditRingSize` (32); accessors `linear_gc_root_audit_entry_at`, `linear_gc_root_audit_total`.

## Existing observability

- `query:linear-ownership-gc-compiler-stats` (schema 763) — the four lifecycle counters.
- `query:linear-gc-root-audit-log` (schema 1543) — audit checks + last entry + optional log.

## Tests

`tests/test_issue_1543.cpp` — registration monotonicity, resync balance, multi-path audit, query surface.
