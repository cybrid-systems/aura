# Hygienic Macros — Design Notes

This document captures the design contract for `define-hygienic-macro` and the EDSL/mutation interactions. It complements the implementation in `src/compiler/macro_expansion.{cpp,ixx}` and the test coverage in `tests/test_issue_120.cpp`, `tests/test_issue_121.cpp`, `tests/test_issue_326.cpp`, and `tests/test_issue_364.cpp`.

## Core invariants

A hygienic macro is defined via:
```
(define-hygienic-macro (name . params) body)
```

At expansion time:
1. `clone_macro_body` is called with a fresh `name_map` so that:
   - Parameter references inside `body` are resolved to fresh `SymId`s (gensym-style).
   - Local `define`s inside `body` (e.g. `let` bindings) don't leak into the caller's scope.
   - Free references inside `body` resolve to the macro's **definition** environment, not the call site.
2. Each expanded node is stamped with `SyntaxMarker::MacroIntroduced` (Issue #190) so the `(syntax-marker-counts)` primitive can distinguish macro-introduced code from user-written code.

## Nested macros

Nested `define-hygienic-macro` works because each expansion runs `clone_macro_body` with its own fresh `name_map`. The outer macro's expansion creates a synthetic form that contains the inner macro's definition; that inner definition is then expanded when the synthetic form is evaluated. Marker propagation is recursive: if the outer expansion introduces a node containing an inner macro definition, the entire subtree carries `MacroIntroduced`.

Example:
```scheme
(define-hygienic-macro (inner x) (list 'define (list 'v x) x))
(define-hygienic-macro (outer n) (inner (inner n)))
(outer 42)
;; Expands to: (define v (inner 42)) then (define v (define v 42))
;; Each step introduces macro-introduced markers.
```

## swap! — the canonical hygiene test

```scheme
(define-hygienic-macro (swap! a b)
  (let ((tmp a)) (set! a b) (set! b tmp)))

(define tmp "outer")
(let ((x 1) (y 2))
  (swap! x y)
  ;; x ↔ 1, y ↔ 2 here (NOT (tmp "outer"))
  (list x y tmp))
;; Without hygiene: tmp captures "outer"
;; With hygiene: tmp is gensym'd → unbound; reference error
```

The macro's `let ((tmp a))` introduces a fresh `tmp` in the macro's environment. The caller's outer `tmp` is shadowed/unaffected.

## Mutation interactions

### `mutate:query-and-replace` on macro-introduced nodes

`mutate:query-and-replace` operates on the FlatAST node level. Macro-introduced nodes have `SyntaxMarker::MacroIntroduced` but mutation treats them like any other node — the marker is preserved on the replacement node (verified in `tests/test_issue_326.cpp` scenario 4).

### `mutate:rebind` on the macro itself

`mutate:rebind` on a macro symbol rebinds the macro definition (the body that will be expanded). Subsequent invocations use the new body. This is tested in `tests/test_issue_364.cpp` scenario 3.

### Snapshot + rollback

`ast:snapshot` + `ast:rollback` preserves macro definitions across the snapshot. If a macro was defined before the snapshot, it remains available after rollback. If a macro was defined after, it's removed by rollback. Tested in `tests/test_issue_364.cpp` scenario 4.

## Stress test coverage

`tests/test_issue_364.cpp` runs 100 cycles of:
1. Define a uniquely-named hygienic macro
2. Invoke it once
3. Re-set-code with a new macro

This exercises the macro registry's incremental-add path and the workspace's macro-recall across re-set-codes (Issue #165 / #166 workspace-aware macro registry).

## Test coverage map

| Scenario | Test file | Coverage |
|---|---|---|
| Basic `swap!` | test_issue_120.cpp test 7 | RAII hygiene |
| Nested gensym | test_issue_121.cpp test 8 | `gensym` in macro body |
| Marker propagation | test_issue_326.cpp scenario 1 | `(syntax-marker-counts)` |
| `query:pattern` on macro-introduced bindings | test_issue_326.cpp scenario 3 | query primitive visibility |
| `mutate:query-and-replace` preserves marker | test_issue_326.cpp scenario 4 | mutation + marker |
| Macro registry persists across evals | test_issue_326.cpp scenario 8 | Issue #165/#166 |
| Nested macro expansion | test_issue_364.cpp scenario 1 | clone_macro_body recursion |
| Nested `swap!` | test_issue_364.cpp scenario 2 | hygiene under nesting |
| `mutate:rebind` on macro | test_issue_364.cpp scenario 3 | macro redefinition |
| `ast:snapshot` + `ast:rollback` with macro | test_issue_364.cpp scenario 4 | snapshot consistency |
| 100-cycle stress | test_issue_364.cpp scenario 5 | macro registry scalability |
| Macro defining macro | test_issue_364.cpp scenario 6 | macro-defining-macro edge case |

## Future work

1. Macro hygiene violations detection: a primitive that reports when a macro body accidentally references a free variable from the caller's environment. Currently only test-time detectable.
2. Incremental macro re-expansion: when a macro definition changes, mark all existing call sites as stale (similar to `mark_subtree_dirty` for AST nodes). Currently re-evaluation re-expands from scratch.
3. `(syntax-marker-counts)` per-workspace: the macro registry is workspace-aware but the marker counter is global. Multi-workspace scenarios might need per-workspace counts.
