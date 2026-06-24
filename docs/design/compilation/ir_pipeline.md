# IR Pipeline — Mental Model

> Issue #296 living doc. Documents the finalized mental model after
> the three-part Closure Bridge work in #223 / #180.

## Components

```
┌─────────────────────────────────────────────────────────────────┐
│                  Tree-walker (evaluator_impl.cpp)               │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Closure{flat*, pool*, env_id, bridge_epoch, ...}        │   │
│  │                                                          │   │
│  │  INVARIANT 1: bridge_epoch == 0 means "legacy" —         │   │
│  │  trusted, no stale check.                                │   │
│  │  INVARIANT 2: non-zero bridge_epoch must match the       │   │
│  │  current bridge_epoch() at every apply_closure call.     │   │
│  │  INVARIANT 3: bridge_epoch must be captured at           │   │
│  │  construction (MakeClosure / parse-closure), not later.  │   │
│  └──────────────────────────────────────────────────────────┘   │
└────────────────────────┬────────────────────────────────────────┘
                         │ bridge_epoch_fn_ (function pointer,
                         │ avoids circular include with service.ixx)
┌────────────────────────▼────────────────────────────────────────┐
│              CompilerService (service.ixx)                       │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  bridge_epoch() → mutation_epoch_.load(relaxed)          │   │
│  │  bump_bridge_epoch() → mutation_epoch_.fetch_add(1)      │   │
│  │                                                          │   │
│  │  INVARIANT 4: bump_bridge_epoch() must happen as a        │   │
│  │  paired operation with cache invalidation. For Cycle 1,  │   │
│  │  this is automatic — mutation_epoch_ is bumped together  │   │
│  │  with mark_define_dirty / mark_all_defines_dirty.        │   │
│  └──────────────────────────────────────────────────────────┘   │
└────────────────────────┬────────────────────────────────────────┘
                         │ closure_bridge_ (set in
                         │ install_persistent_define_closure_bridge)
┌────────────────────────▼────────────────────────────────────────┐
│              IR Executor (ir_executor_impl.cpp)                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  IRClosure{flat (shared_ptr), pool (shared_ptr),         │   │
│  │           body_id, env, bridge_epoch, ...}               │   │
│  │                                                          │   │
│  │  MakeClosure: copy flat/pool from module.closure_bridge, │   │
│  │    capture bridge_epoch. INVARIANT 3 enforced here.      │   │
│  │  ApplyClosure: use is_bridge_stale(...) to check.        │   │
│  │  If stale → re-parse from body_source (or invalidate).  │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

## State Machine

```
        ┌──────────┐  bump_bridge_epoch()  ┌──────────┐
        │ current  │ ─────────────────────> │  bumped  │
        │  epoch   │                        │  epoch   │
        └──────────┘                        └──────────┘
              │                                   │
              │ closure construction              │ apply_closure
              │ (MakeClosure / parse)             │ (validate)
              ▼                                   ▼
        ┌──────────┐                        ┌──────────────┐
        │ captured │   is_bridge_stale()    │ stale / ok   │
        │   epoch  │ ─────────────────────> │  decision    │
        └──────────┘                        └──────────────┘
                                                  │
                                                  ├── ok → apply
                                                  │
                                                  └── stale → body_source
                                                              re-parse OR
                                                              invalidate
```

## Failure Modes

| Mode | Detection | Recovery |
|---|---|---|
| Stale bridge data | `is_bridge_stale` returns true | Re-parse from `body_source` |
| Legacy (epoch==0) | `is_bridge_stale` returns false | Trust; caller manages |
| Arena reset | `mutation_epoch_` bumped via `reset()` | All closures re-parse |
| Bridge callback missing | `closure_bridge_` not set | Fall through to tree-walker path |

## Files Touched

| File | Role |
|---|---|
| `src/compiler/evaluator.ixx` | `is_bridge_stale` static helper + INVARIANTs |
| `src/compiler/service.ixx` | `bridge_epoch()`, `bump_bridge_epoch()`, `install_persistent_define_closure_bridge()` |
| `src/compiler/evaluator_eval_flat.cpp` | Calls `is_bridge_stale` at apply_closure |
| `src/compiler/ir_executor_impl.cpp` | Captures bridge_epoch at MakeClosure |
| `src/compiler/lowering_impl.cpp` | Sets up `closure_bridge` per IRModule |
| `tests/test_issue_296.cpp` | Isolated bridge unit tests (7 ACs) |

## Cycle History

- **Cycle 1** (#223, #180): shared_ptr-based bridge + epoch tracking
- **Cycle 2** (#224): incremental per-block re-lower consumer
- **Cycle 3** (#225): explicit invalidate_bridge_for() trigger

## Future Work (deferred)

- **Cycle 4**: split `mutation_epoch_` into separate `bridge_epoch_` and
  `cache_epoch_` if bridge and cache invalidation need different policies
  (currently they share `mutation_epoch_` per #223 Cycle 1).
