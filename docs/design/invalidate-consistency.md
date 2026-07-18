# Invalidate consistency audit — soft/hard unified protocol (#1627)

**Issue:** [#1627](https://github.com/cybrid-systems/aura/issues/1627)  
**Builds on:** #1496 · #1476 · #1607 · #1536 · #1543 · #1545 · #1626  
**Status:** Production closed-loop audit + soft-path pre-cascade parity.

## Single write-side protocol

```
mark_define_dirty(name)          // soft: dirty bits + BFS cascade
invalidate_function(name)        // hard: + JIT erase / dep_graph teardown
        │
        ├─► prepare_unified_invalidation_pre_cascade_(name)   (#1627)
        │     scan_live_closures_for_linear_captures(mark_invalid)
        │     linear_post_mutate_enforce_all()
        │     on_compiler_invalidate_gc_coordination(name)
        │     run_linear_gc_root_audit(Invalidate)
        │
        └─► atomic_bump_epochs_and_stamp_bridge(name)
              release fence
              bump_bridge_epoch() / defuse / aot table
              invalidate_bridge_for + JIT batch_deopt
              notify_walk_active_closures_
```

Readers (`apply_closure`, `aura_is_jit_closure_fresh`) acquire-load either
domain and take safe fallback / deopt — never half-updated state.

## Mutation path coverage (6+)

| # | Path | Pre-cascade | Epoch protocol | GC/JIT |
|---|------|-------------|----------------|--------|
| 1 | `mark_define_dirty` (set-body / rebind soft) | yes (#1627) | atomic_bump | walk + audit |
| 2 | `invalidate_function` (hard) | yes | atomic_bump | hard erase + audit |
| 3 | `typed_mutate` catch-all | via atomic_bump("") | atomic_bump | batch_deopt all |
| 4 | `hot_swap_function` | bridge invalidate | mutation_epoch | GC coordination |
| 5 | `set-code` / workspace reset | mark_all_defines_dirty | full dirty | roots clear |
| 6 | fiber steal / compact_env_frames | dual-epoch remaps | stamp after compact | audit Compact |
| 7 | JIT ResourceTracker / fn_trackers_ | batch_deopt in atomic_bump | aot table | walk_active |

## Metrics (`query:epoch-apply-hotpath-stats`, schema **1627**)

| Key | Meaning |
|-----|---------|
| `invalidate_cascade_depth` / `_max` | BFS cascade depth |
| `bridge_epoch_bumps` | Epoch publishes |
| `live_closure_stale_prevented` | Live IR/JIT stale |
| `linear_gc_root_audit_checks_total` | GC root audits |
| `invalidate_pre_cascade_prepare_total` | Soft+hard prepare |
| `soft-pre-cascade-wired` / `invalidate-consistency-wired` | 1 |
| `soft-hard-same-protocol` | 1 |
| `schema` | **1627** (lineage 1626\|1607\|1604) |

## Tests

| File | Role |
|------|------|
| `tests/test_invalidate_consistency_1627.cpp` | **#1627** AC |
| `tests/test_unified_invalidation_1607.cpp` | Lineage |
| `tests/test_issue_1496.cpp` | Original unify |

## Related

- `docs/design/unified-invalidation-1607.md`
- `docs/design/unified-invalidation-epoch-protocol.md`
