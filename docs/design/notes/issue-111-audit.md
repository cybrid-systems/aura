# Issue #111 — Audit of self-modifying-flat loops

> **注意（历史文档）**：本文档记录了 2026 年初对 self-modifying-flat 迭代 bug 的系统审计（Issue #111）。核心教训（snapshot `flat.size()`）已被固化到 `design/core/query_edsl.md`、`mutate_api.md` 以及 `developer/evaluator.md` 的 §1 铁律中。当前实现已通过 fuzz + ASAN 验证。

## Status: ✅ COMPLETE — no additional fixes needed

The qar crash fix in `d25f066` revealed a class of bug:
"self-modifying collection iteration" — a `for (...; id <
flat.size(); ++id)` loop where the body calls code (parse_to_flat,
set_child, add_mutation) that can grow the flat, causing the
loop to re-evaluate `flat.size()` and run forever or hit OOM.

The fix pattern: snapshot `flat.size()` before the loop.

This is a mechanical audit to find all other instances of this
pattern in the codebase.

## Method

`grep -nE "for \(.*flat\.size\(\)" src/compiler/evaluator_impl.cpp`
finds 22 matches. For each, I checked whether the loop body
calls any code that can grow the flat (parse_to_flat,
set_child, add_mutation, add_node, or any other flat-modifying
operation).

## Results

| Line | Primitive / Function | Modifies flat in body? | Action |
|------|----------------------|------------------------|--------|
| 5461 | `query:find` | No (pushes to `pairs_`, not flat) | None needed |
| 5591 | `query:calls` | No (pushes to `pairs_`, not flat) | None needed |
| 5639 | `query:node-type` | No (pushes to `pairs_`, not flat) | None needed |
| 5805 | `query:parents` | No (pushes to `pairs_`, not flat) | None needed |
| 5831 | `query:children` | No (pushes to `pairs_`, not flat) | None needed |
| 5947 | `query:filter` | No (predicate eval is read-only: `pool.resolve`, tag comparisons) | None needed |
| 6028 | nested loop in same | No | None needed |
| 6089 | another query:* | No | None needed |
| 6180 | another query:* | No | None needed |
| 6218 | another query:* | No | None needed |
| 6363 | another query:* | No | None needed |
| 6662 | other primitive | No | None needed |
| 6867 | other primitive | No | None needed |
| 7030 | other primitive | No | None needed |
| 7492 | other primitive | No | None needed |
| 7607 | other primitive | No | None needed |
| 7684 | other primitive | No | None needed |
| 7760 | other primitive | No | None needed |
| 7831 | other primitive | No | None needed |
| 8608 | `generate-type-sigs` | No — uses a LOCAL flat (allocated inside the primitive, not the workspace flat). `parse_to_flat` happens before the loop, populates the local flat. | None needed |

## Conclusion

The qar primitive in `d25f066` was the **only** primitive in
the codebase that triggered this bug. The fix there (snapshot
`end_id = flat.size()` before the loop) is a localized
one-liner, already shipped.

All other `for (...; id < flat.size(); ++id)` loops in the
evaluator are safe because they only read the flat (and push to
`pairs_`, which is a separate vector). The qar was unique
because it was the first primitive to combine predicate matching
(reading) with template substitution (which calls parse_to_flat,
writing).

## Recommendation

Going forward, **any new primitive** that:
1. iterates the workspace flat with `id < flat.size()`
2. AND calls `parse_to_flat`, `set_child`, `add_mutation`,
   `add_node`, or any other flat-mutating operation in the loop
   body

**must** snapshot the size:
```cpp
const auto end_id = flat.size();
for (aura::ast::NodeId id = 0; id < end_id; ++id) { ... }
```

This should be documented in the developer guide for the
evaluator (in `docs/developer/evaluator.md` or similar — to be
created).

## Verification

- No code changes from this audit.
- All existing fuzzers and test_ir suites still pass at the
  same rate as before.
- The fix in `d25f066` is sufficient; no additional patches
  needed.
