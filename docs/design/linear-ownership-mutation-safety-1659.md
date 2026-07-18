# Mandate linear ownership + GC/Arena mutation safety (#1659)

**Issue:** [#1659](https://github.com/cybrid-systems/aura/issues/1659)  
**Builds on:** #1606 · #1596 · #1568 · #1545 · #1478 · #1515 · #1540 · #1288  
**Status:** Production mandate — `linear_ownership_state` end-to-end + tombstone on invalidate + GC/Arena synergy.

## Contract

| Layer | Behavior |
|-------|----------|
| Lowering | Stamp `IRInstruction::linear_ownership_state` (0–4) |
| EnvFrame | `bindings_linear_ownership_state_` per-slot snapshot |
| Interpreter | `linear_heap_` entry `{value, ref_count, live}`; `live=false` = tombstone |
| Apply (TW + IR) | `linear_post_mutate_enforce` before use; safe_fallback on Moved/stale |
| JIT | `aura_jit_linear_post_mutate_enforce` + GuardShape linear probe |
| Invalidate / mutate | `scan_live_closures_for_linear_captures` → `bridge_epoch=0` tombstone |
| GC safepoint | `probe_linear_ownership_at_gc_safepoint` + root audit |
| Force drop | `force_drop_or_mark_invalid` → epoch 0 (cannot Apply) |

Ownership states (mirrors `ir.ixx`):

| Value | Meaning |
|------:|---------|
| 0 | Untracked |
| 1 | Owned |
| 2 | Borrowed |
| 3 | MutBorrowed |
| 4 | Moved (terminal — never a live root) |

## Metrics (`query:linear-boundary-consistency-stats`, schema **1659**)

| Key | Meaning |
|-----|---------|
| `linear_ownership_violation_prevented` | Use-after-move / stale capture prevented |
| `linear-violation-count` | Alias of `linear_violations_caught_total` |
| `linear_live_closure_scans_total` | Live-closure scans |
| `envframe-linear-ownership-snapshot-wired` | 1 |
| `linear-heap-runtime-wired` | 1 |
| `apply-closure-linear-check-wired` | 1 |
| `jit-linear-post-mutate-enforce-wired` | 1 |
| `invalidate-tombstone-wired` | 1 |
| `gc-arena-linear-synergy-wired` | 1 |
| `linear-ownership-mandate-active` | 1 |
| `schema` | **1659** (lineage 1606 / 1596 / 1568) |

## Tests

| File | Role |
|------|------|
| `tests/test_linear_ownership_mutation_safety_1659.cpp` | **#1659** AC |
| `tests/test_linear_ownership_post_mutate.cpp` | #1596 lineage |
| `tests/test_walk_active_closures_1606.cpp` | #1606 lineage |

## Related

- `docs/design/linear-ownership-runtime-1596.md`
- `docs/design/linear-validation.md`
- `docs/design/linear-gc-roots.md`
