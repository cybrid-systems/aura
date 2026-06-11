# Design: Issue #79 — Strict type-check + rich diagnostics

## Evidence (what's actually broken)

I read `src/compiler/type_checker_impl.cpp`, `src/compiler/diag.ixx`, `src/compiler/query_impl.cpp`,
`src/compiler/service.ixx`, and `src/main.cpp`. The following concrete bugs make
type-checking warnings-only with weak error reporting:

### Bug A — `is_coercible` silently accepts real type errors

`src/compiler/type_checker_impl.cpp:520-560`. The function permits a wide range of
cross-type coercions (Int ↔ String, Int ↔ Bool, Float ↔ String, ...). At every
call site (line 1711, 2263, 2320), a `true` return from `is_coercible` causes
the diagnostic to be reported as a `Note` (informational) instead of a
`TypeError`. The `cs.typecheck()` output filters Notes out via
`has_errors() == false`, so:

```aura
(begin (define x "hello") (+ 1 x))
```

`--typecheck` reports **"no errors"** and exits 0. The runtime catches the
mismatch (correctly) and prints `error: type mismatch — expected Int, got String 'hello'`,
but the static path is silent.

The user-facing contract — `--typecheck` means "static type errors, if any" —
is broken.

### Bug B — Type-error sites lack `with_blame` and `with_suggestion`

Counted in `type_checker_impl.cpp`: 14 `diag_.report(Diagnostic(ErrorKind::TypeError,...))`
sites. 5 of them have neither `with_blame(...)` nor `with_suggestion(...)`:
- L1213: `cannot move x — moved`
- L1237: `cannot borrow x — mut-borrowed`
- L1261: `cannot mutably borrow x — borrowed`
- L1284: `cannot drop x — moved`
- L1564: `no member 'foo' in module M`
- L2355: `expected a function type but got <T>`

Without `BlameParty`, AI agents and `AuraQuery` cannot distinguish "you wrote
the wrong argument" (Caller) from "the annotation is wrong" (Annotation)
from "the borrow violated linearity" (System).

Without `with_suggestion`, the AI agent's "auto-fix" loop has nothing to
ground its edits in. The LLM has to invent a fix from the message text
alone.

### Bug C — Runtime diagnostics lack source location

`src/compiler/evaluator_impl.cpp:13758, 15115` create `Diagnostic d(ErrorKind::UnboundVariable, ...)`
with no `SourceLocation`. The user's console output shows `1:1: error:` (or
just `error:`) because `location.valid()` is false and the formatter falls
back to "?" or `node[N]`. This is silent data loss for any tooling that
relies on line numbers.

### Bug D — `Kind::HasError` query is a stub

`src/compiler/query_impl.cpp:280`:
```cpp
case QueryExpr::Kind::HasError:
    return false; // Error tracking not yet implemented in FlatAST
```

`FlatAST` has no per-node error tracking, so the AuraQuery `(has-error? N)`
clause always returns false. AI agents can't ask "which nodes are broken?"
without crawling the entire diagnostic list and matching by `node_id`.

### Bug E — `--typecheck` exit code is always 0

`src/main.cpp:1077`:
```cpp
return result.find("diagnostics:") == std::string::npos ? 0 : 1;
```

But `cs.typecheck()` (service.ixx:1424) never produces a string containing
the literal `"diagnostics:"` — its error path prints each diagnostic on a
new line via `d.format() + "\n"`. So this check always finds no
"diagnostics:" prefix and returns 0. **Even when the typechecker reports
errors via the eval path, the exit code is 0.**

### Bug F — No "fix-it" / coercion hint on type-mismatch

When `(+ 1 "hello")` becomes a CoercionNode at the call site (L1719), the
diagnostic is just a Note. The AI agent would need to know:
- That the call was type-mismatched
- What the expected type was
- What the actual type was
- A suggested fix (e.g. "wrap in (to-int x)" or "use a string+string concat")

None of this is in the Note message.

## The fix

### Phase 1: Add `strict` flag to TypeChecker + TypeChecker is the truth

`src/compiler/type_checker.ixx`:
- Add `bool strict_ = false;` field to `TypeChecker` (default off for backward compat).
- Add `set_strict(bool)` and `is_strict()` accessors.
- In the eval path, **always** set `strict = true` so the runtime also
  reports hard errors. Gradual coercion is a *type-checker* mode, not a
  *runtime* mode. The runtime must enforce what the type-checker said.

`src/compiler/type_checker_impl.cpp`:
- `is_coercible`: gate the cross-type rules on `strict_`:
  - Dynamic ↔ X: always coercible (gradual core, always allowed)
  - Int ↔ String, Int ↔ Bool, Float ↔ String, Float ↔ Bool: only
    coercible when `!strict_`. In strict mode, these are **TypeErrors**
    (not even coercions).
  - Float ↔ Int: numeric coercion, always allowed (a real number narrows).

This preserves backward compat (`--typecheck` and existing tests still
work) while adding a new lever. Default in `CompilerService::typecheck()`
is `strict = true` (because that's what users mean by "typecheck").

### Phase 2: Fill in missing `with_blame` / `with_suggestion`

For each of the 5 sites in Bug B:

- L1213, 1237, 1261, 1284 (move/borrow/mut/drop): BlameParty = `System`
  (linearity is a system-level invariant), suggestion = e.g.
  `"replace 'move x' with a clone, or re-bind x to a fresh value"`.
- L1564 (no member in module): BlameParty = `Caller` (caller passed a
  wrong module member name), suggestion = the closest-match `did you
  mean 'foo'?` (we already compute it elsewhere for unbound vars).
- L2355 (expected function type): BlameParty = `Caller`, suggestion =
  `"check that the callee is bound to a function, not a value"`.

### Phase 3: Source location on runtime diagnostics

`src/compiler/evaluator_impl.cpp:13758, 15115`: thread `cur_loc` through
`eval_flat` (it currently has none for these two paths) and pass to the
`Diagnostic` ctor. Set `v.line, v.col` from the offending node's
`flat.get(...)`.

### Phase 4: Per-node error tracking + AuraQuery `Kind::HasError`

`src/core/ast.ixx`:
- Add `std::pmr::vector<std::uint32_t> error_kind_` SoA column to `FlatAST`
  (parallel to `tag_`). 0 = no error, non-zero = `ErrorKind` enum value.
- `set_node_error(NodeId, ErrorKind)` and `node_error(NodeId) -> ErrorKind`.
- `clear_node_error(NodeId)`.
- `clear_node_error_subtree(NodeId root)` — walks the tree, resets all
  descendants' error markers.

`src/compiler/type_checker_impl.cpp`: every `diag_.report(Diagnostic(...TypeError...))`
also calls `flat.set_node_error(node_id, kind)`. The first parameter
(`node_id`) is already in scope at every error site; we just need to
plumb it through.

`src/compiler/evaluator_impl.cpp`: same for the runtime error sites.

`src/compiler/query_impl.cpp:280`:
```cpp
case QueryExpr::Kind::HasError: {
    auto err = index_.ast.node_error(id);
    return err != 0;
}
```

`src/compiler/query_impl.cpp:160-170` already parses the literal
`(has-error? N)` clause. Now it actually works.

### Phase 5: Correct exit code from `--typecheck`

`src/main.cpp:1077`: stop checking the output string. Instead, ask the
`CompilerService` to return a structured result that includes a `has_errors`
flag, and use that.

`src/compiler/service.ixx`: add a `TypecheckResult` struct:
```cpp
struct TypecheckResult {
    std::string output;
    bool has_errors = false;
    std::vector<aura::diag::Diagnostic> diagnostics;
};
```

`typecheck()` returns `TypecheckResult`. `main.cpp` returns
`result.has_errors ? 1 : 0`.

### Phase 6: Fix-it hints on cross-type coercions

`src/compiler/type_checker_impl.cpp:1711-1735` (the cross-type coercion
Note path): when `strict_ == false` and we insert a CoercionNode, attach
a `with_suggestion("consider using a typed annotation (: x Int) to make
this static")`. This gives AI agents a concrete handle to convert gradual
code into strict code.

## Backward compat

- `is_strict()` defaults to `false` in `TypeChecker` ctor. Existing
  callers that construct `TypeChecker` and call `infer_flat` see no
  behavior change unless they explicitly opt in.
- `CompilerService::typecheck()` sets `strict = true` because that
  matches user intent.
- `eval_flat` and the runtime path set `strict = true` so CastOp
  insertions are still produced (for gradual safety) but a real type
  mismatch is reported, not silenced.

## Test plan

- `test_regression.py`:
  - `tc-strict-catches-int-plus-string` — `(+ 1 "x")` reports as type error
    in strict mode, no error in non-strict (with cast inserted).
  - `tc-runtime-diagnostic-has-location` — runtime unbound var has line:col
  - `tc-runtime-suggestion` — `did you mean` works for runtime errors
- `test_ir.cpp`: add tests for `FlatAST::set_node_error` / `node_error`
  / `Kind::HasError` query.
- `tests/suite/typecheck_strict.aura`: hand-written tests that exercise
  the strict-mode blast radius.

## Affected files

- `src/compiler/type_checker.ixx` — add strict_ flag.
- `src/compiler/type_checker_impl.cpp` — is_coercible gate, blame + suggestion fills.
- `src/compiler/evaluator_impl.cpp` — location, blame, suggestion, set_node_error.
- `src/compiler/service.ixx` — TypecheckResult struct, set strict on typecheck.
- `src/compiler/diag.ixx` — minor: ensure Diagnostic formats blame even when
  suggestion is set (already does, no change).
- `src/core/ast.ixx` — new `error_kind_` SoA column + accessors.
- `src/compiler/query_impl.cpp` — implement `Kind::HasError`.
- `src/main.cpp` — return 1 on type errors.
- `tests/test_ir.cpp` — new tests.
- `tests/test_regression.py` — new cases.
- `tests/suite/typecheck_strict.aura` — new suite.
