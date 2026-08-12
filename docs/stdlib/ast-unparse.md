# AST unparse library (Issue #2922)

Extracted FlatAST → source printer for agent context, snapshots, and
`(current-source)`.

## API

```cpp
import aura.core.ast_unparse;

aura::ast::UnparseOptions opts;
opts.pretty = false;          // default: compact single-line (#2921)
opts.indent_width = 2;        // spaces per level when pretty
opts.max_depth = 256;         // deeper → "..."
opts.define_fn_sugar = false; // (define (f x) …) vs (define f (lambda …))

std::string s = aura::ast::unparse_to_string(flat, pool, flat.root, opts);
```

No `Evaluator` dependency. Safe under `MutationBoundaryGuard` (pure read).

## Aura surface

| Form | Meaning |
|------|---------|
| `(current-source)` | compact unparse of `current_flat_` |
| `(current-source :workspace)` | compact unparse of workspace |
| `(current-source :pretty)` | multi-line indented |
| `(current-source :workspace :pretty)` | workspace + pretty |
| `(current-source :define-fn-sugar)` | define-function sugar |

Keywords may appear in any order. Default options keep #2918–#2921
roundtrip behavior (compact, no sugar).

## Call sites

| Site | Notes |
|------|-------|
| `kCurrentSource` | thin wrapper → `unparse_to_string` |
| `ast:snapshot` / `ast:diff` | library call (no primitive re-entry) |
| `CompilerService` `get_workspace_source_fn` | SSOT with `:workspace` |

## Size note (≈10k nodes)

Unit test `test_ast_unparse` AC7 builds a wide `begin` of ~2500 defines
(≥5k nodes) and times one unparse. Allocation is a single reserved
`std::string` (no recursive `operator+`). See test stdout:
`AC7 bench note: nodes=… bytes=… unparse_ms=…`.

## Extending

When a new production `NodeTag` lands, add an arm in
`src/core/ast_unparse.ixx` and a row in the #2921 roundtrip table
(`tests/compiler/test_current_source_roundtrip.cpp`).
