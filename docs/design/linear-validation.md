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

## Future (#1543+)

Borrow/MutBorrow stamping at runtime, and richer per-cell state transitions beyond Moved detection.
