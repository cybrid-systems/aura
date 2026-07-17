# IRClosure / ClosureBridge MacroIntroduced provenance (#1616)

**Issue:** [#1616](https://github.com/cybrid-systems/aura/issues/1616)  
**Builds on:** #1610 · #1047 · #455 · #1513  
**Status:** Production closed-loop (refine #1047 end-to-end).

## Contract

| Layer | Behavior |
|-------|----------|
| `IRInstruction` | `source_marker` + `provenance` (from #1610 emit) |
| `FlatInstruction` | Carries both fields IR→JIT |
| `ClosureBridgeData` | `source_marker` + `provenance` stamped from body AST |
| `IRClosure` | Same fields at MakeClosure; consulted on apply |
| `ir_closure_needs_safe_fallback` | MacroIntroduced consults + ignored-if-stale metrics |

## Metrics (`query:ir-hygiene-stats`, schema **1616**)

IR-marker AC surface is **keys** on this hash (`macro-introduced-count`, …) —
no separate `query:ir-marker-stats` name (#1448 freeze).

| Key | Meaning |
|-----|---------|
| `ir_provenance_stamped_total` | MakeClosure MacroIntroduced stamps |
| `macro_introduced_ignored_in_ir` | Stale MacroIntroduced forced fallback |
| `ir-closure-macro-stamped` / `consults` | Closure hygiene path |
| `macro-introduced-count` | AST markers + IR stamps |
| `closure-bridge-marker-wired` | 1 |
| `schema` | **1616** |

## Tests

| File | Role |
|------|------|
| `tests/test_ir_closure_provenance_1616.cpp` | **#1616** AC |
| `tests/test_ir_hygiene_propagation_1610.cpp` | Lineage accepts 1616 |

## Related

- `docs/design/ir-hygiene-propagation-1610.md`
