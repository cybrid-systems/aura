# Complete remaining mutate:* primitive migration to MutationBoundaryGuard + unified defuse_version bump invariants (#1904)

**Issue:** [#1904](https://github.com/cybrid-systems/aura/issues/1904)  
**Builds on:** #177 · #184 · #213 · #233 · #236 · #241 · #266 · #459 · #1014 · #1251 · #1252 · #1253 · #1254 · #1256 · #1364 · #1373 · #1375 · #1443 · #1523 · #1547 · #1556 · #1590 · #1592 · #1608 · #1618 · #1619 · #1628 · #1631  
**Status:** P0 production closed-loop (refine #213 Cycle 1-3 — complete the ~13 legacy mutate:* primitives to MutationBoundaryGuard RAII).

## Problem

#213 + its Cycles 1-3 successfully migrated the rollback mechanism + per-primitive Guard adoption for an initial batch of mutate:* primitives (remove-node, record-patch, etc.). However:

- ~13 mutate:* / workspace:* / agent:* primitives still use legacy `std::unique_lock<std::shared_mutex>(ev.workspace_mtx_)` + explicit `ev.defuse_version_.fetch_add(...)` patterns.
- Unbalanced enter/exit or missing Guard on error paths (early returns without setting `ok=false`) can leak locks or leave the version in inconsistent state.
- Version bump semantics are not uniform: some paths bump on enter only, some on exit (success), some always — breaking the "exactly one net bump per successful boundary" invariant that downstream (AOT mangle, bridge_epoch, stale detection, JIT deopt) rely on.
- Rollback on panic / false `success_flag` is only wired for migrated primitives; legacy ones may leave partial mutations or stale checkpoints.
- Nested boundaries + fiber steal + GC interaction are not yet stress-tested across the full primitive set.

This blocks reliable zero-downtime self-modifying Agent orchestration under concurrent fibers.

## Contract

```
MutationBoundaryGuard (RAII, the single owner of workspace_mtx_ + version + checkpoint + rollback):
  ctor:
    try_acquire(ev, pending_count, success_flag, fine_rollback)  → AuraResult<unique_ptr>
       preferred; typed ResourceQuotaExceeded (#1547/#1556/#1590)
    legacy ctor(ev, success_flag, fine_rollback) [[deprecated]]
       soft-fails on quota (#1590); inert_ flag suppresses lock + dtor work
  dtor:
    exit_mutation_boundary(*success_flag)
    on outermost only: rollback if !success, bump version exactly once on success
    bump mutation_boundary_primitives_wrapped (#1252 coverage counter)
  nesting:
    thread_local depth counter (Evaluator::mutation_boundary_depth_slot)
    only outermost acquires workspace_mtx_ (shared_mutex non-recursive)

Audit gate (scripts/check_legacy_mutate_lock.py):
  ZERO std::unique_lock<std::shared_mutex>(... ev.workspace_mtx_ ...) in
  src/compiler/evaluator_primitives*.cpp (legacy-lock rule)
  ZERO ev.defuse_version_.fetch_add(...) outside Guard context (legacy-bump rule)
  ALLOW marker: // #1904-allow legacy-lock: <reason>
  CI exit 1 on any match; --self-test for regex correctness
```

## Metrics (`query:mutation-guard-coverage`, schema **1904**)

New primitive, returns integer:

| Value | Meaning |
|-------|---------|
| `10000` (basis points) | Vacuously covered — no Guard wraps + no legacy sites yet |
| `-1` | Regression sentinel — `mutation_legacy_manual_lock_total > 0` |
| `wrapped * 10000 / (wrapped + legacy)` | Partial coverage ratio |

| Counter | Bumped when |
|---------|-------------|
| `mutation_boundary_primitives_wrapped` | Every outermost MutationBoundaryGuard ctor (#1252, pre-existing) |
| `mutation_legacy_manual_lock_total` | Every legacy `std::unique_lock(ev.workspace_mtx_)` site (#1904 new) |
| `mutation_guard_try_acquire_total` | Every `try_acquire` call (#1547/#1618/#1628, pre-existing) |
| `mutation_guard_try_acquire_reject_total` | Every quota reject (#1547/#1618/#1628, pre-existing) |

| Key | Meaning |
|-----|---------|
| `guard-coverage-mandated` | 1 |
| `legacy-pattern-linter-wired` | 1 |
| `schema` | **1904** (lineage 1634\|1618\|1556\|1547\|1443\|1375\|1373\|1364\|1256\|1253\|1252\|1014\|459\|266\|241\|236\|233\|213\|184\|177) |

## Migration scope (the ~13 legacy sites)

| File | Lines | Notes |
|------|-------|-------|
| `evaluator_primitives_ast.cpp` | 179, 433, 1091, 1099 | Various AST mutate:* paths |
| `evaluator_primitives_eval.cpp` | 70 | eval-site mutate |
| `evaluator_primitives_mutate.cpp` | 758, 1017, 1477, 1797, 3975, 4096 | Core mutate:* primitives |
| `evaluator_primitives_workspace.cpp` | 638, 667, 698, 812 | workspace:discard / :edit paths |
| `evaluator_primitives_agent.cpp` | 813, 1272 | Agent rebind + workspace setup |

Each migration:
1. Replaces `std::unique_lock<std::shared_mutex> wlock(ev.workspace_mtx_);` with `bool ok = true; MutationBoundaryGuard guard(ev, &ok);` (or `try_acquire` for typed-error paths).
2. Removes the manual `ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel);` (the Guard's dtor owns the bump).
3. Adjusts error paths to set `ok = false` before return (Guard dtor triggers rollback via `exit_mutation_boundary(false)`).
4. Verifies `mutation_legacy_manual_lock_total` does not bump at the migrated site.

## Tests

`tests/test_issue_1904.cpp` (9 ACs, public API + linter integration):

- **AC1**: 4 #1904 accessors reachable + baseline 0
- **AC2**: Fresh evaluator → primitive returns 10000 (vacuously covered)
- **AC3**: Wrapped-only path → primitive still 10000
- **AC4**: Legacy > 0 → primitive returns -1 sentinel
- **AC5**: Basis points math (`-1` precedence over `10000`/`wrapped*10000/total`)
- **AC6**: `scripts/check_legacy_mutate_lock.py --self-test` exits 0
- **AC7**: Linter detects synthetic legacy pattern in a temp file (exits non-zero)
- **AC8**: `mutation_boundary_primitives_wrapped` monotonic under `mutate:rebind`
- **AC9**: Migrated primitive leaves `mutation_legacy_manual_lock_total` at 0

The CI linter (run pre-commit + pre-push) fails the build when any new legacy pattern is reintroduced, providing a hard regression gate.

## Non-duplicative

- Builds on #1252 (mutation_boundary_primitives_wrapped counter) — #1904 adds the complement legacy counter + ratio primitive + linter gate.
- Builds on #1547/#1556/#1590/#1618/#1628 (try_acquire + typed-error quota) — #1904 enforces that all call sites actually use the Guard.
- Builds on #213 Cycles 1-3 (rollback + initial migration) — #1904 is the final close-out for the remaining legacy sites.
- Does NOT introduce a new atomic bump invariant — the existing `enter_mutation_boundary()` + dtor already implements "exactly one net bump per successful outermost boundary" (verified by walk_env_frame_roots stale detection + post-steal refresh). #1904 enforces that primitives use this invariant instead of manual ad-hoc bumps.
