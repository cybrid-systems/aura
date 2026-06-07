# Issue #107 — JIT cache thread-safety (Workspace AST concurrency)

## Status: ✅ CLOSED — 6 parts + 1 ASAN follow-up, all merged to main

Issue #107 covered the thread-safety of the Workspace AST under
multi-agent / fiber concurrency. The work was split into 6 parts
plus an ASAN leak fix that surfaced during the work.

## Commit log

| # | Commit | Description |
|---|--------|-------------|
| 1 | `acec84f` | Workspace AST shared mutex |
| 2 | `f7ef43c` | shared_lock on query:/typecheck-current |
| 3 | `8ad500c` | ast:version primitive |
| 3.5 | `96458d4` | restore auto-typecheck in mutate primitives |
| 4 | `2e5b961` | replace typecheck-current in 4 fuzzer paths |
| 5 | `846c561` | per-sym version in DefUseIndex |
| 6 | `9c7e520` | direct FlatAST snapshot/restore |
| - | `28e94b8` | (follow-up) ASAN leak fix |

## Part-by-part

### Part 1 — Shared mutex
Replaced the implicit "single-threaded" assumption on
`workspace_flat_` with a `std::shared_mutex` (`workspace_mtx_`).
Reads (query:*, typecheck-current, ast:*) take a shared lock;
writes (set-code, mutate:*, snapshot, restore) take a unique
lock. First-class concurrency primitive that everything else
builds on.

### Part 2 — shared_lock on the read path
Made the query and typecheck primitives actually take the
shared lock. Before this, they would race with concurrent
mutates. `set-code` was already unique-locked (it's a write);
the read primitives were the gap.

### Part 3 — `ast:version` primitive
A read-only primitive that returns the current AST versioning
state as a snapshot:
```
(defuse-version . (dirty-defines ...) . (all-defines ...))
```
LLMs and audit tools use this to detect when their cached
view of the AST is stale. Holds the shared lock so the read
is atomic w.r.t. concurrent mutates.

### Part 3.5 — Restore auto-typecheck in mutate
Earlier work had disabled auto-typecheck in mutate primitives
as a workaround for #107 part 2. Part 3.5 turned it back on
because the shared lock now makes it safe.

### Part 4 — Replace typecheck-current in 4 fuzzer paths
The fuzzer had been calling `typecheck-current` (which takes
the unique lock) inside read paths. Replaced with the cheaper
`run_typecheck_no_lock` family that takes the shared lock
instead. This unblocked the concurrent test scenarios.

### Part 5 — Per-symbol version in DefUseIndex
Added a `stale_syms_: set<SymId>` and `global_version_: uint64`
to DefUseIndex itself. The mutation paths (`mutate:rebind`,
`mutate:set-body`) now also call `idx->touch_sym(sym)` to mark
the affected sym as stale. Exposed `stale-syms` and
`defuse-version` in `query:index-stats`.

Deliberate non-decision: did NOT short-circuit `ensure_defuse`
on staleness. That would let a future mutation that forgets
to call `defuse_touch_fn_` silently leak stale data. The
`defuse_affected_syms_` list stays the authoritative source;
the per-sym version is a co-located observation.

### Part 6 — Direct FlatAST snapshot/restore
Replaced the source-based `ast:restore` (which re-parsed the
stored source string) with a direct deep-copy of the
workspace's FlatAST and StringPool. The direct copy:
- Preserves SymId identity (no re-intern churn)
- Preserves `mutation_log_`, `type_id_`, `value_cache_`
- Is O(1) instead of O(source) reparse
- Falls through to source-based restore on failure

The snapshot stores `std::unique_ptr<FlatAST>` and
`std::unique_ptr<StringPool>` alongside the source string.
`ast:restore` prefers the direct copy, falls back to source.

### ASAN leak fix
While testing part 6 under ASAN, found that 8 reset sites
(set-code, set-body, workspace switching, arena reset) plus
the Evaluator destructor all set `defuse_index_ = nullptr`
without freeing the old `DefUseIndex*`. Each call leaked
~3 KB; the destructor leaked the final one. Added a free
helper `defuse_index_destroy(void** slot)` (free function,
not a static member, because the call sites live before
the struct body in the TU). Replaced all 9 nullings with
`defuse_index_destroy(&defuse_index_)`. ASAN exit=0
after the fix.

## Verification (final state)

- `./build/test_ir` exit=0 (all suites pass)
- `tests/fuzz_defuse.py --quick`: 200/200
- `tests/fuzz_workspace.py --quick`: 290/290
- `tests/fuzz_snapshot.py --quick`: 405/405 (48 restores)
- ASAN test_ir: exit=0, 0 leaks
- ASAN snapshot+mutate+restore loop: 50 iterations, 0 leaks
- Manual: snapshot→mutate→restore preserves NodeId identity
  (`g` at 5, `h` at 11 before and after)

## Follow-ups that remained open after #107 close

These are out of #107's scope but were flagged during the work:

1. **Pre-existing rebuilds-doubling in `query:def-use`** —
   between two `query:index-stats` calls, `query:def-use`
   causes `defuse_rebuild_count_` to go up by 2 (1 → 2 → 4).
   Same on clean main, so pre-existing. Likely the auto-typecheck
   after `mutate:rebind` calls `ensure_defuse` somewhere.
   Tracked separately.

2. **Pre-existing `ast:defs` only sees top-level Defines** —
   the helper added in #108 walks the flat and finds Define
   nodes by tag. Top-level Defines inside a `begin` are not
   surfaced. Same scoping rule applies as the Begin-scoped
   X issue. Doesn't affect EDSL benchmarks but worth flagging.

Both are now in the noise of pre-existing Aura quirks. They
neither regress nor block #108 / future issues.

## Architecture summary after #107

The Evaluator's concurrency model is now:

- `workspace_mtx_`: `std::shared_mutex` around the workspace
  AST and dependent state (defuse index, IR cache v2, etc.)
- Read primitives (`query:*`, `typecheck-current`, `ast:*`):
  take `std::shared_lock` (read lock)
- Write primitives (`set-code`, `mutate:*`, `ast:snapshot`,
  `ast:restore`): take `std::unique_lock` (write lock)
- DefUseIndex is an internal cache that can be invalidated
  by either path; per-sym version tracks per-sym staleness
  for observability

This is the baseline that #108 (stdlib gaps), #109 (fiber:join),
and the rest of the P0 list build on.
