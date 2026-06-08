# Issue #129 — Docs: sync aura_typesystem.md with current implementation

## Status: 🟢 Complete (docs-only sync — 3 items moved from "未实装" to "实装/部分实装")

This is a documentation-only issue. The implementation
already supports incremental type checking, Let-Poly, and
ADT exhaustiveness — but the design doc was lagging
behind, marking them as "未实装". The doc has been
updated to match the code.

## What changed

### 1. `docs/design/aura_typesystem.md` (single file, +~50 lines)

- **Top status line**: "T2a-T2e" → "T2a-T2e + T4".
  T4 is the cluster of incremental / let-poly / ADT
  exhaustiveness features.
- **Section 2.4 Let-Poly row**: ⚡ "is_poly 字段存在但
  未完整使用" → ✅ T4 with description of the bind +
  lookup mechanism.
- **Section 2.5 穷尽性检查 row**: 🟡 "尚未实现" → ✅ T4
  with description of the `__match_tmp` +
  `get_adt_constructors` mechanism.
- **Section 5.1 增量类型检查**:
  - Title: "（未实装）" → "（实装于 synthesize_flat）"
  - Body rewritten to describe the actual implementation:
    `if (!flat.is_dirty(id))` skip path,
    `cache_hits` / `cache_misses` / `stale_cache` stats,
    EngineStats aggregation.
- **Section 5.3 Let-Poly**:
  - Title: "Let-Poly 未使用" → "Let-Poly（已实装于
    TypeEnv::bind + lookup）"
  - Body rewritten to describe `is_poly` set by
    `forall_of`, `instantiate_forall` in lookup, and
    `infer_flat` propagation in let/letrec.
- **Section 5.5 ADT 穷尽性**:
  - Title: "ADT 穷尽性检查" → "ADT 穷尽性检查（部分实装）"
  - Body rewritten to describe the `__match_tmp` +
    `get_match_info` + `get_adt_constructors` mechanism.
  - "剩余限制" subsection added: nested match doesn't
    re-check after the outer match.
- **Section 6 未来改进路径**:
  - "Level 2: 增量类型检查" → "Level 2: 增量类型检查
    （已实装，见 5.1）" with cross-reference.
  - "Level 4: Let-Poly 启用" → "Level 4: Let-Poly
    （已实装，见 5.3）" with cross-reference.
  - New "Level 5: ADT 穷尽性检查" with cross-reference
    to 5.5.
- **New Section 8: Implementation vs Documentation 同步说明**:
  - Table summarizing the 3 sync items (before /
    actual / after).
  - Checklist for future PRs to keep doc in sync.

## Why the new design works

### Why a docs sync is a real issue

Documentation drift is a real engineering problem. When
the doc says "incremental type checking is not
implemented" and the code has been using it for 3
months, new contributors either (a) waste time
implementing something that already exists, or (b)
ignore the doc and trust the code, which means the
doc is useless. A docs sync issue is a way to
explicitly call out the drift and resolve it.

### Why the structure (table in 5.x + cross-references in 6)

The original 5.x was a list of "未实现" items. The
refactored 5.x mixes "实装" and "未实装" items, with
each section having a clear status. Cross-references
between 5.x and 6 make it easy to navigate: see 5.1
for the implementation status, see 6.Level 2 for the
historical "future work" path that has been completed.

### Why a "Implementation vs Documentation" section (8)

Issue #129 explicitly suggested this section. It's
useful because:
1. The PR author can point at section 8 to show
   the sync work
2. Future contributors can use the checklist (8.2) to
   update the doc when adding new type-system features
3. The table (8.1) is a snapshot of "what was drifted
   at this point in time" — useful for archaeology

## Known limitations (out of scope for #129)

- **Nested match exhaustiveness** — the current
  implementation only checks the top-level match
  (`__match_tmp` at the immediate let). Nested matches
  are not independently re-checked. A future issue.
- **Multi-mutation dirty granularity** — the current
  cache check works for single mutations; multiple
  mutations in sequence may invalidate more than
  necessary. A future issue.
- **The other design docs** (`aura_typesystem_formal.md`,
  `typed_mutation_design.md`, `ir_pipeline_design.md`)
  may have similar drift. They are not in scope for
  this issue.

## Verification

- The doc changes are documentation-only — no code
  changes, no tests to add.
- The implementation references in the doc (line
  numbers like `type_checker_impl.cpp:1336`) are
  accurate as of the current main branch
  (`2380eb8`).
- All existing tests pass (integ 148/148, typecheck
  10/10, 14 per-issue tests). The doc changes don't
  affect tests.

## Test status

- `integ`: 148/148 ✓
- `typecheck`: 10/10 ✓
- `test_issue_115..128` all 14 pass ✓
- No new test_issue_129 (this is a docs-only issue;
  a regression test would be "verify the doc claims
  match the code", which is a meta-test, not a unit
  test).

## What (if anything) is still open

- Sync the other design docs (aura_typesystem_formal.md,
  typed_mutation_design.md, ir_pipeline_design.md) to
  match the current implementation. These may have
  similar drift.
- Add a CI step that runs a "doc claims vs code"
  cross-checker (would be a future infrastructure
  improvement).

1 file changed, 0 files added, 0 files removed.
