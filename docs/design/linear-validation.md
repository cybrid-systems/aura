# Linear Validation Pipeline (Post-Mutation)

**Issues:** #1494 / #1486 (closed-loop parents), #1458, #1478, #1538–#1545

## Summary

Post-mutation linear ownership safety is a **dual-half pipeline** plus
**entry-point / invalidate / GC-root** enforcement (#1486):

| Half | API | Layer | What it checks |
|------|-----|-------|----------------|
| Type-checker | `post_mutation_invariant_check` / `PostMutationInvariantVisitor` | AST + `OwnershipEnv` | Dirty-subtree Linear bindings: use-after-move, double-borrow, leaked-linear, occurrence narrowing |
| Runtime | `Evaluator::linear_post_mutate_enforce` / `linear_post_mutate_enforce_all` | EnvFrame | Captured linear values on live env frames (Moved scan via SoA; #1539) |

Issue **#1538** unifies them so every successful `typed_mutate` / `typed_mutate_atomic` runs **both** halves and surfaces combined diagnostics on `MutationResult` and mutation-log JSON.

### #1486 closed-loop map

| AC | Surface | Shipped |
|----|---------|---------|
| 1 Entry validation | `apply_closure` / `materialize_call_env` / JIT Apply | #1478, #1542, #1540 |
| 2 Invalidate + boundary scan | `scan_live_closures_for_linear_captures` on invalidate/compact/JIT + fiber steal (#1557) + outermost Guard exit (Moved-only mark) | #1545, #1486, #1557 |
| 3 GC root consistency | `docs/design/linear-gc-roots.md` + audit ring | #1543 |
| 4 Metrics | `linear_post_mutate_enforcements`, `linear_ownership_violation_prevented` | #1478 |
| 5 Tests | use-after-move + stress | #1539, #1544, `test_issue_1486` |

### #1494 closed-loop map (invalidate / mutate active mark)

| AC | Surface | Shipped |
|----|---------|---------|
| 1 Scan + mark on mutate/invalidate | `scan_live_closures_for_linear_captures` (+ optional `filter_env_id`); Moved mark bumps `linear_ownership_violation_prevented` | #1494 |
| 2 typed_mutate pipeline | `apply_linear_post_mutate_pipeline_` → enforce_all **then** scan only_if_moved | #1494 |
| 3 invalidate_function | scan (all linear) + `linear_post_mutate_enforce_all` | #1494 |
| 4 mark_define_dirty | scan only_if_moved after dirty | #1494 |
| 5 Tests | `tests/test_issue_1494.cpp` | #1494 |

## Call graph

```
typed_mutate / typed_mutate_atomic (success)
  │
  ├─► PostMutationInvariantVisitor  (#1458 / #147)
  │     └─ post_mutation_invariant_check per MutationRecord
  │           └─ OwnershipEnv::validate_ownership on dirty Linear bindings
  │
  └─► apply_linear_post_mutate_pipeline_  (#1538 / #1494)
        ├─ Evaluator::linear_post_mutate_enforce_all
        │     └─ linear_post_mutate_enforce(env_id) for each live frame
        └─ scan_live_closures_for_linear_captures(mark, only_if_moved)
              └─ bridge_epoch=0 on Moved captures → safe_fallback

invalidate_function (#1494 / #1557)
  ├─ scan_live_closures_for_linear_captures(mark_invalid)  // all linear
  └─ linear_post_mutate_enforce_all

compact_env_frames (#1545)
  └─ scan_live_closures_for_linear_captures(mark_invalid)  // pre-remap

fiber steal (#1557)
  └─ probe_linear_ownership_on_fiber_steal
        ├─ scan_live_closures_for_linear_captures(mark_invalid)  // proactive
        └─ epoch/version probe + linear_gc_root audit

mark_define_dirty (#1494)
  └─ scan_live_closures_for_linear_captures(mark, only_if_moved)

JIT ResourceTracker / hot-swap (#1545 / #1536)
  ├─ aura_jit_linear_live_closure_scan → scan_live_closures...
  └─ AuraJIT::walk_active_closures(epoch)
```

## Observability (Agent surface)

| Metric | Use |
|--------|-----|
| `linear_live_closure_scans_total` | How often proactive scans ran |
| `linear_live_closures_marked_invalid_total` | Force-invalid (logical Drop) count |
| `linear_ownership_violation_prevented` | Moved / use-after-move prevented |
| `linear_post_mutate_enforcements` | Per-env apply-time enforce |
| `jit_walk_active_closures_total` | JIT bulk stale-fn walks |

See also `docs/design/linear-gc-roots.md` and `(engine:metrics "query:linear-ownership-gc-compiler-stats")`.

## MutationResult fields (#1538)

| Field | Meaning |
|-------|---------|
| `invariant_status` / `invariant_diagnostics` | Type-checker half (#147) |
| `linear_post_mutate_enforced` | Runtime half ran |
| `linear_post_mutate_safe` | All frames reported safe |
| `linear_post_mutate_frames_checked` | EnvFrames swept |
| `linear_post_mutate_status` | `"NotRun"` \| `"Ok"` \| `"Unsafe"` |

## Mutation log / --serve

`query_mutation_log` → `MutationLogEntry.linear_post_mutate_status`  
Populated from a service-side `mutation_id → status` map written by the combined pipeline. Default `"NotRun"` for pre-#1538 or Disabled-mode records.

## Strict mode

- Type-checker notes under Strict → `success=false` (existing #147 behavior).
- Runtime half `all_safe=false` under Strict → also `success=false` with error  
  `"linear post-mutate enforce reported unsafe env frames"`.

## Metrics

- `linear_post_mutate_enforcements` — per-frame enforce calls (#1478)
- `linear_post_mutate_enforcements_total` — sweep + other total bumps
- `linear_post_mutate_pipeline_total` — combined pipeline runs (#1538)
- `linear_post_mutate_pipeline_unsafe_total` — combined pipeline unsafe (#1538)

## Runtime cell tagging (#1539)

`EnvFrame` (and `Env`) carry a parallel SoA:

```
bindings_symid_[i]                    // SymId + EvalValue
bindings_linear_ownership_state_[i]   // 0=untracked … 4=Moved (mirrors ir.ixx)
```

| API | Role |
|-----|------|
| `bind_symid_with_linear_state` / `bind_with_linear_state` | Stamp state at bind time |
| `set_linear_ownership_state` | Update (e.g. Move → 4) |
| `linear_post_mutate_enforce` | Returns **false** if any binding is Moved |
| Let + `Linear e` | Stamps **Owned** (1) on bind |
| `Move` of Variable | Stamps **Moved** (4) on Env + parent EnvFrame |

`alloc_env_frame_from_env` / `materialize_call_env` copy the SoA so closure capture preserves tags.

`materialize_call_env` also calls `linear_post_mutate_enforce` on entry (#1542) so TCO / non-`apply_closure` materialize sites share the same contract as `closure_needs_safe_fallback`. On Moved → empty-Env fallback (`materialize_fallback_total`).

## GC root audit (#1543)

See **`docs/design/linear-gc-roots.md`**: registration consistency across
typed_mutate / invalidate / compact_env_frames / JIT hot-swap / fiber steal /
GC safepoint, plus `query:linear-gc-root-audit-log` and
`linear_gc_root_audit_checks_total`.

## Future

Borrow/MutBorrow stamping at runtime, and richer per-cell state transitions beyond Moved detection.
