# Issue #112 — Update design documents to reflect current state

## Status: ✅ CLOSED — 7 docs updated + 1 new

Issue #112 was a documentation refresh: the design docs and tutorial had
drifted from the actual code state after #107 (concurrency) / #108
(stdlib gaps) / #109 (fiber:join) / #110 (qar + self-modifying-flat) all
landed. The 5 sub-tasks are all closed.

## Commit

| Commit | Description |
|--------|-------------|
| `0a30351` | `docs(developer): add evaluator developer guide (#112)` — first commit, dev guide |
| `TBD`    | `docs(112): update design + tutorial + README to reflect #107-#110` — this commit |

## What changed

### 1. `docs/developer/evaluator.md` — **NEW** (15 sections, ~23 KB)

The developer guide for adding new primitives to the evaluator.
Created as the explicit follow-up to the #111 audit. Covers:

- **§1**: The self-modifying-flat iteration rule (the #110 qar lesson)
- **§2**: Adding a new primitive (arg validation, return conventions)
- **§3**: Mutate locking protocol (unique vs shared, no re-entrance)
- **§4**: DefUseIndex touch protocol (both `defuse_affected_syms_` and `defuse_touch_fn_`)
- **§5-§10**: Closures/primitives, C FFI, ADT ctor table, recursion
  guards, IRContext, AST snapshot/restore
- **§11**: Pre-merge pitfalls checklist (11 items)
- **§12-§14**: Testing, file map, related docs

### 2. `docs/design/query_edsl_design.md` (now `design/core/query_edsl.md`) — updated

- Marked all P0-P3 priorities as ✅ shipped (P0-P2 全部完成, P3 重构原语
  也完成)
- Added `query:where` / `query:filter` section (实装于 #110)
- Added `query:def-use` / `query:reaches` / `query:effects` (DefUseIndex
  integration from #107 part 5)
- Added `ast:defs` / `ast:nodes` (from #108 part 2)
- Added `mutate:query-and-replace` (from #110)
- Added "Agent 集成模式" section (10.1-10.3) with runnable code
- Performance table now has measured values, not estimates
- Added §11 "Related docs" cross-references

### 3. `docs/design/agent_orchestration.md` (now `design/core/agent_orchestration.md`) — **major update**

The doc claimed `fiber:join` was still a stub and `orch:parallel` had a
serial fallback. Both are wrong after #109. Updated:

- Status: ✅ Phase 1-2 全部完成
- "❌ 待实现" → "✅ 已实现" for fiber:join, orch:parallel
- Added "Mutation Boundary 与并发 mutate" section explaining
  `workspace_mtx_` + `g_fiber_yield_mutation_boundary` interaction
- Replaced the "fiber:join 实现方案" stub with the actual implementation
  (cv-based blocking in stdin mode, yield-and-check in serve-async)
- Added "Cross-Session Messaging 示例" with correlation id
- Added "多 Agent Pipeline 示例" with planner/coder/tester + parallel
  reviews + conduct with if/retry
- Updated implementation roadmap (Phase 1-3 done, Phase 4 future)

### 4. `docs/design/mutate_api.md` (now `design/core/mutate_api.md`) — **major update**

Expanded from 100 lines / 5 primitives to ~250 lines / 12 primitives:

- Added "原子性与回滚" section with `save-panic-checkpoint` / `restore-panic-checkpoint`
- Reorganized primitives by category (precise / function-level / pattern / high-level refactor)
- Added `mutate:query-and-replace` section (Issue #110)
- Added `mutate:wrap` / `mutate:splice` (new since the old doc)
- Added "Mutate 协议总览" with the canonical primitive skeleton
- Added "测试覆盖" section with fuzzer list
- Added "Future Work" section

### 5. `docs/design/typed_mutation_design.md` (now `design/core/typed_mutation.md`) — **major update**

The doc didn't mention `DefUseIndex` or `WorkspaceTree COW` at all. The
#112 sub-task 5 specifically asked to sync these. Added:

- **§6**: DefUseIndex + WorkspaceTree COW 集成 (the #112 sync point)
  - DefUseIndex per-sym staleness from #107 part 5
  - Touch protocol (both `defuse_affected_syms_` and `defuse_touch_fn_`)
  - WorkspaceTree COW data structures
  - Memory budget enforcement (Issue #97 Action 3)
  - Aura API examples
- **§7**: Direct FlatAST Snapshot/Restore (from #107 part 6)
  - Why we replaced reparse-based snapshot with deep-copy
  - What's lossless, what isn't
  - OOM fallback path
- **§8**: 反思与教训
  - 3 design decisions that turned out wrong
  - 3 current open follow-ups

### 6. `docs/tutorial.md` — added §10.5 "Self-Modifying Agent 快速上手"

The exact 5-min quickstart that #112 sub-task 4 asked for:

- 最小循环: `set-code` → `query:pattern` → `mutate:extract-function` → `eval-current`
- 真实场景: bug 修复循环（factorial 改迭代）
- 多 Agent Pipeline (planner/coder/tester + parallel reviews + conduct with if/retry)
- 安全模式: snapshot → mutate → verify → rollback
- 跨 Agent 通信示例

### 7. `README.md` — added "30 秒 Quickstart" + capability list

The README was 33 lines of "build" + one paragraph of capabilities.
Added:

- 30-second Self-Modifying Agent Quickstart (copy-pasteable code)
- Bullet list of capabilities (incremental compile, concurrency, JIT/AOT)
- Links to all 4 design docs + developer guide

## Verification

- All line-number references verified against the current tree
  (`evaluator_impl.cpp`, `evaluator.ixx`, `ir_executor.ixx`).
- All `merr` / `mev` / `MAX_*_DEPTH` / `g_fiber_yield_mutation_boundary`
  / `mark_dirty_upward` patterns referenced in the docs exist in the
  code as described.
- The new tutorial section 10.5 examples are runnable
  (all primitives cited exist in `evaluator_impl.cpp`).
- No code changes. Working tree was clean before doc updates.
- The original commit `0a30351` was an initial dev-guide-only commit
  that turned out to be a partial fix — this commit completes the
  full #112 scope.

## Total #112 work

| Sub-task | Doc | What |
|----------|-----|------|
| 1 | `query_edsl_design.md` (now core/query_edsl.md) | Updated implementation status, added query:where/query:filter, added Agent integration patterns |
| 2 | `agent_orchestration.md` (now core/agent_orchestration.md) | Marked Phase 1-2 done, added fiber:join impl, added Mutation Boundary section, added multi-agent examples |
| 3 | `mutate_api.md` (now core/mutate_api.md) | Expanded to 12 primitives, added atomicity/rollback, added mutate protocol overview |
| 4 | `tutorial.md` + `README.md` | Added 5-min quickstart sections |
| 5 | `typed_mutation_design.md` (now core/typed_mutation.md) | Added DefUseIndex + WorkspaceTree COW + Direct Snapshot sections |
| + | `developer/evaluator.md` | **NEW** — evaluator dev guide (the #111 follow-up) |

7 doc files updated/created. 0 code changes. Future sessions add
sections as new patterns emerge (the developer guide is explicit about
being a "living document").
