# Linear Validation Pipeline (Post-Mutation)

**Issues:** #1458, #1478, #1538

## Summary

Post-mutation linear ownership safety is a **dual-half pipeline**:

| Half | API | Layer | What it checks |
|------|-----|-------|----------------|
| Type-checker | `post_mutation_invariant_check` / `PostMutationInvariantVisitor` | AST + `OwnershipEnv` | Dirty-subtree Linear bindings: use-after-move, double-borrow, leaked-linear, occurrence narrowing |
| Runtime | `Evaluator::linear_post_mutate_enforce` / `linear_post_mutate_enforce_all` | EnvFrame | Captured linear values on live env frames (MVP counter; full cell scan → #1543) |

Issue **#1538** unifies them so every successful `typed_mutate` / `typed_mutate_atomic` runs **both** halves and surfaces combined diagnostics on `MutationResult` and mutation-log JSON.

## Call graph

```
typed_mutate / typed_mutate_atomic (success)
  │
  ├─► PostMutationInvariantVisitor  (#1458 / #147)
  │     └─ post_mutation_invariant_check per MutationRecord
  │           └─ OwnershipEnv::validate_ownership on dirty Linear bindings
  │
  └─► apply_linear_post_mutate_pipeline_  (#1538)
        └─ Evaluator::linear_post_mutate_enforce_all
              └─ linear_post_mutate_enforce(env_id) for each live frame
```

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

## Future (#1543)

`linear_post_mutate_enforce` MVP always returns true. Real per-cell linear ownership scan on `EnvFrame.bindings_` will make `all_safe=false` actionable for use-after-move on captured cells at apply time.
