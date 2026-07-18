# Enforce dual-path consistency (bindings_ vs bindings_symid_) + INVALID_VERSION GC filtering (#1903)

**Issue:** [#1903](https://github.com/cybrid-systems/aura/issues/1903)  
**Builds on:** #145 · #242 · #286 · #322 · #355 · #356 · #418 · #485 · #488 · #543 · #1269 · #1384 · #1482 · #1510 · #1526 · #1539 · #1550 · #1592 · #1604 · #1608 · #1614 · #1626 · #1631  
**Status:** P0 production closed-loop (refine existing observability into enforced invariant + INVALID_VERSION GC walk filter + Aura primitive).

## Problem

SoA/DOD Phase 2 introduced parallel `bindings_` (string-keyed) + `bindings_symid_` (SymId-keyed) + `bindings_linear_ownership_state_` vectors on `EnvFrame`, with `#1269` adding the canonical dual-path consistency probe (`Evaluator::ensure_envframe_dual_path_consistency`). However, enforcement was *opportunistic*:

- Many mutate paths (`mutate:rebind` / `mutate:replace-*` / `set-body` under `MutationBoundaryGuard`) updated only one side of the dual-path or relied on ad-hoc sync.
- `materialize_call_env` had version + `refresh_stale_frame_in_walk` checks but the dual-path consistency probe only ran at frame-read time (before copy), not post-copy on the materialized `Env`.
- `GCEnvWalkFn` (registered in `gc_coordinator` → evaluator's `walk_env_frame_roots`) skipped stale frames but did **not** skip `INVALID_VERSION` frames — a regression risk if a doomed-transaction frame's bindings referenced AST nodes / pool strings that no longer existed.
- `complete_post_resume_steal_refresh` refreshed stale frame `version_` but did not re-assert dual-path consistency on the refreshed frame, leaving the bind-time ↔ walk-time invariant implicit.
- No Aura primitive exposed the post-#543 counter family independently of `#1903`'s new counters; observability was bundled into `query:envframe-dualpath-stats`.

This led to potential stale `Env` / closure body materialization, incorrect GC roots, or version mismatch under concurrent fiber steal + mutation + GC in long-running Agent orchestration.

## Contract

```
EnvFrame::ensure_dual_path_consistent() const noexcept -> bool
  length parity       bindings_.size() == bindings_symid_.size()
  linear SoA parity   bindings_linear_ownership_state_.size() == bindings_symid_.size()
  content parity      when pool_ is set AND lengths agree:
                      for each i: bindings_[i].second == bindings_symid_[i].second
                      AND (when pool_->resolve(symid) is non-empty)
                          bindings_[i].first == pool_->resolve(symid)
  bumps envframe_dual_consistency_asserted_ on every call
  on pass: bump bindings_dual_sync_count_
  on fail: bump envframe_desync_detected_

Wire-up sites:
  EnvFrame::bind_with_linear_state           — call ensure_dual_path_consistent() (post-emit)
  EnvFrame::bind_symid_with_linear_state     — call ensure_dual_path_consistent() (post-emit)
  Evaluator::alloc_env_frame_from_env        — set fr.owner_ + call ensure (existing)
  materialize_call_env                       — post-copy: bump material_consistency_checks
                                              + (debug) assert dual consistency
  walk_env_frame_roots                       — INVALID_VERSION skip + bump gc_walk_safe_skips_
                                              + prefer bindings_symid_ + legacy fallback bumps
                                                envframe_gc_walk_legacy_fallback_uses_
  refresh_stale_frames_after_steal           — track refreshed IDs, after lock release:
                                              for each refreshed ID: ensure_dual_path_consistent
                                              + bump envframe_post_steal_dual_synced_
```

## Metrics (`query:envframe-dual-consistency-stats`, schema **1903**)

New primitive, sum of 4 counters (P0 follows the `query:envframe-dualpath-stats` shape — a follow-up returns a 4-tuple so the Agent can react independently).

| Counter | Bumped when |
|---------|-------------|
| `envframe_dual_consistency_asserted_` | Every `ensure_dual_path_consistent()` call (pass + fail) |
| `envframe_post_steal_dual_synced_` | Every refreshed frame in `refresh_stale_frames_after_steal` |
| `envframe_materialize_consistency_checks_` | Every `materialize_call_env` post-copy check |
| `envframe_gc_walk_legacy_fallback_uses_` | Every `walk_env_frame_roots` frame where `bindings_symid_` is empty and walk fell back to `bindings_` |

| Key | Meaning |
|-----|---------|
| `dual-consistency-enforced` | 1 (helper shipped) |
| `gc-walk-invalid-version-skip` | 1 (`walk_env_frame_roots` filters `INVALID_VERSION`) |
| `gc-walk-prefer-symid` | 1 (`walk_env_frame_roots` prefers `bindings_symid_`) |
| `post-steal-dual-sync` | 1 (`refresh_stale_frames_after_steal` re-asserts) |
| `materialize-dual-check` | 1 (post-copy consistency check) |
| `schema` | **1903** (lineage 1550\|1269\|543\|485\|418\|356) |

## Tests

`tests/test_issue_1903.cpp` (9 acceptance criteria, public API only — INVALID_VERSION frame mutation is covered by `tests/test_issue_356.cpp`):

- **AC1** `ensure_dual_path_consistent()` bumps `envframe_dual_consistency_asserted_` by 1 per call
- **AC2** `(engine:metrics "query:envframe-dual-consistency-stats")` returns int + monotonic
- **AC3** 4 new counter accessors reachable + monotonic non-decreasing
- **AC4** `walk_env_frame_roots` on fresh evaluator: empty arena → no bumps
- **AC5** 1k iter alloc + ensure loop → asserted grows by exactly 3000 (no leak)
- **AC6** `walk_env_frame_roots` after stress loop → no decrement of safe_skips / legacy fallback
- (INVALID_VERSION walk skip + post-steal + materialize paths are covered by their primary test files: `test_issue_356` for `INVALID_VERSION`, `test_unify_invalidate_try_acquire_1634` for post-steal, `test_issue_147` for materialize.)

## Follow-ups tracked (deferred)

1. 4-tuple return from `query:envframe-dual-consistency-stats` (P0 ships sum; tuple follows the `dualpath-stats` shape).
2. Semantic comparison at the bind path itself: today the helper detects drift; a future P1 can auto-repair by re-interning + rebuilding the cheaper side (bindings_ → bindings_symid_ via canonical pool, or vice versa).
3. `bindings_legacy_uses_` per-`EnvFrame` counter (Env has it; EnvFrame doesn't yet) — split the legacy-uses signal between Env (lookup) and EnvFrame (walk / mutate).
4. Test harness for `walk_env_frame_roots` with crafted `INVALID_VERSION` / desynced frames without needing panic-rollback (test-only mutators).

## Non-duplicative

- Builds on the `#1269` dual-path probe (#1903 strengthens to a member helper with content parity check, not just length).
- Builds on the `#356` `INVALID_VERSION` sentinel (#1903 extends GC walk to actually filter it, not just materialize).
- Builds on the `#543` / `#1550` counter family (#1903 adds 4 new counters; no overlap with existing).
- Builds on the `#1592` / `#1631` post-steal refresh (#1903 adds the dual-sync pass after refresh).
- Builds on the `#1626` dual-check on `apply_closure` (#1903 is the EnvFrame-side counterpart — materialization + GC walk + post-steal).
