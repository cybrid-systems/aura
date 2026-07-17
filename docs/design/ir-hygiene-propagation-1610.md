# IR / JIT / AOT MacroIntroduced hygiene propagation (#1610)

**Issue:** [#1610](https://github.com/cybrid-systems/aura/issues/1610)  
**Builds on:** #1047 · #455 · #733 · #501 · #1273 · #1609  
**Status:** Production closed-loop (stamp + JIT policy + authoritative stats).

## Contract

| Layer | Behavior |
|-------|----------|
| Lowering `emit()` | Stamp `source_ast_node_id`, `source_marker`, `provenance` from AST |
| `IRInstruction` | `source_marker` + `provenance` (+ existing `source_ast_node_id`) |
| InlinePass | Skip inlining MacroIntroduced call sites (pre-existing #388) |
| FlatInstruction | Carries `source_marker` IR→JIT |
| JIT `lower()` | Consult MacroIntroduced; no L1/L2 shape specialization; dirty+macro → deopt |
| AOT bridge | Same FlatFnBuilder path as JIT (marker propagated) |

## Metrics (`query:ir-hygiene-stats`, schema **1610**)

| Key | Meaning |
|-----|---------|
| `ir-hygiene-stamped-count` | MacroIntroduced IR instr stamps (`aura_hygiene_ir_macro_marker_total`) |
| `provenance-stamped-count` | Non-zero AST provenance stamps |
| `jit-macro-introduced-deopt` | Dirty+MacroIntroduced forced deopts |
| `jit-macro-hygiene-consults` | JIT lower saw source_marker==MacroIntroduced |
| `inline-hygiene-skipped` | InlinePass skips (#501 lineage) |
| `lowering-stamp-wired` / `jit-marker-check-wired` | Wire flags (=1) |
| `schema` | **1610** |

## Tests

| File | Role |
|------|------|
| `tests/test_ir_hygiene_propagation_1610.cpp` | **#1610** AC |
| `tests/test_issue_733.cpp` | Marker hygiene stats (schema 733, unchanged) |
| `tests/test_task6_production_readiness_closed_loop_514.cpp` | ir-hygiene-stats regression |

## Related

- `docs/design/query-pattern-hygiene-1609.md` (query-layer hygiene)
- `docs/production-review-macro-reflect-self-evo-top5-issues.md`
