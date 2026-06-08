# Issue #120 — Implement Hygienic Macros (define-hygienic-macro + clone_macro_body with name_map)

## Status: ✅ Resolved (P0 macro hygiene / safety, Task 6)

Aura had `defmacro` for simple template substitution (v1) and
`expand_qq` for quasiquote expansion in the parser, but no
hygienic guarantee. When a macro expansion introduced a binding
(such as `tmp` in a typical `swap!` macro) and the call site
happened to have a same-named binding, the macro would
unintentionally capture the call site's variable — a classic
Scheme hygiene bug, especially dangerous when combined with the
`mutate:*` EDSL for self-modifying code.

## What changed

### 1. New special form: `define-hygienic-macro`
   (`src/parser/parser.ixx`, `src/parser/parser_impl.cpp`)

A new keyword that behaves like `defmacro` but with the
hygienic flag set on the `MacroDef` AST node. Parser:
- `defmacro` → `parse_defmacro(false)`
- `define-hygienic-macro` → `parse_defmacro(true)`

Both forms share the same body parser (dotted rest params,
the existing `parse_defmacro` logic). The only difference is the
`hygienic` bit passed to `add_macrodef`.

### 2. AST: hygienic bit on `MacroDef` node
   (`src/core/ast.ixx`)

The `MacroDef` node's `int_val_` slot already stored the
`dotted` flag (bit 0). Added the `hygienic` flag as bit 1.
New helpers:
- `add_macrodef(name, params, body, dotted, hygienic)`
- `is_hygienic_macrodef(id)` — query bit 1
- `is_dotted_macrodef(id)` — query bit 0

### 3. Runtime: MacroDef carries the hygienic flag
   (`src/compiler/evaluator.ixx`,
    `src/compiler/evaluator_impl.cpp`)

The `MacroDef` struct gained a `bool hygienic` field, set
from the new AST bit. Both call sites that build a
`MacroDef` from an AST node now read the bits correctly:
- The runtime expansion (line ~17669): `is_dotted = bit 0`,
  `is_hygienic = bit 1`.
- The static `macro_expand_all` helper: same.

### 4. Runtime expansion: clone_macro_body for hygienic macros
   (`src/compiler/evaluator_impl.cpp`)

When `md.hygienic` is true, expansion uses
`clone_macro_body(target=*f, target_pool=*p, source=*md.flat,
source_pool=*src_pool, body_id=md.body_id, &subst, &rename_map)`
and then `eval_flat` on the result. The legacy double-eval
path (body → data → re-eval as code) is preserved for
non-hygienic macros.

`clone_macro_body` was already implemented with the hygiene
logic (built-in whitelist, gensym for binding positions,
name_map for reference resolution) but was only called from
the static `macro_expand_all` helper. Two bugs were fixed
during the integration:

a. **Subst NodeIds are in target, not source.** The macro
   param's substitution maps to a NodeId in the *calling*
   flat (target), not the macro's source flat. The original
   code recursed with `body_id=arg_node, source=md.flat`,
   reading from the wrong flat. The fix: return the arg
   NodeId directly (it's already in target, so the calling
   code uses it as-is).

b. **Pre-scan for bindings.** The clone walks in a
   bottom-up order: children first, then the binding position.
   So Variable references in the body would resolve against
   an empty `name_map`. The fix: a pre-scan that walks the
   body and populates `name_map` with gensym'd binding
   names BEFORE the actual clone. The pre-scan only runs when
   `name_map` is non-null (i.e., hygiene is on).

c. **Set node sym_id substitution.** `Set` (and any other
   reference-position node) wasn't looking up its sym_id in
   `subst`. The fix: for `Set`, if the sym_id is a macro
   param, use the arg's sym_id (resolved from target) as
   the Set's name. Otherwise, fall back to `transplant`.

### 5. Regression tests
   (`tests/test_issue_120.cpp`, 7/7 passed)

- `test_capture_outer_tmp` — classic capture-bug shape: a
  hygienic `swap!` introing a `tmp` binding, called inside
  `(let ((tmp "x")) ...)`. The outer `tmp` must NOT be
  captured.
- `test_nested_hygienic_macros` — two hygienic macros,
  one calling the other. Both layers hygiene correctly.
- `test_macro_params_vs_builtins` — macro param named `if`
  (a builtin). The rename_binding code skips builtins, so
  `if` is NOT gensym'd.
- `test_multiple_expansions_no_collision` — two expansions
  of the same macro. Each gets its own gensym'd `tmp`.
- `test_legacy_defmacro_still_parses` — non-hygienic
  `defmacro` still works (backward compat).
- `test_hygienic_macro_without_binding` — a hygienic macro
  that doesn't intros a binding (name_map can be empty).
- `test_runtime_end_to_end` — the capture-bug shape
  compiled for the runtime.

Wired into `CMakeLists.txt` as `test_issue_120` with a CTest
entry (`issue_120_verification`).

### 6. End-to-end smoke

```
$ cat /tmp/test.aura
(define-hygienic-macro (swap! a b)
  (let ((tmp a)) (set! a b) (set! b tmp)))
(define x 1)
(define y 2)
(swap! x y)
(display "x=") (display x) (newline)
(display "y=") (display y) (newline)

$ ./build/aura < /tmp/test.aura
x=2
y=1
```

The swap works AND the outer `tmp` is not captured when
present:

```
$ cat /tmp/test_capture.aura
(define-hygienic-macro (swap! a b)
  (let ((tmp a)) (set! a b) (set! b tmp)))
(define x 1) (define y 2) (define tmp "outer-tmp")
(swap! x y)
(display "x=") (display x) (newline)
(display "y=") (display y) (newline)
(display "tmp=") (display tmp) (newline)

$ ./build/aura < /tmp/test_capture.aura
x=2
y=1
tmp=outer-tmp
```

## Why the new design works

The previous `defmacro` used a double-eval pipeline:
- AST args → data (via `ast_to_data`)
- Bind params in `tail_env`
- Evaluate body to data (with quasiquote)
- Re-evaluate data as code (via `eval_data_as_code`)

This works for simple substitution but has no hygiene: a
macro that intros a binding (via `let`/`set!` in its body)
modifies the macro's local `tail_env`, not the call site. The
`set!` in `swap!` modifies a local `a` (bound to a copy of
x), not the caller's `x`.

The new `define-hygienic-macro` uses AST-level substitution
with `clone_macro_body`:
- Args are passed by NodeId (in the calling flat).
- The body is cloned with subst (param→arg) and name_map
  (gensym'd binding names).
- The cloned tree is evaluated directly in the calling
  env. The `set!` modifies the caller's actual binding, not
  a local copy.

The hygiene invariants:
- Macro-introduced bindings (gensym'd names like `__tmp_0`)
  cannot be captured by the call site because the call site
  doesn't have these names in its env.
- Call-site bindings (e.g., the user's `tmp`) cannot be
  captured by the macro because the macro's references to
  `tmp` have been redirected to the gensym'd name.

The pre-scan is needed because the body walk is bottom-up:
binding positions are processed AFTER their bodies. The
pre-scan populates `name_map` before the actual clone so
Variable references in the body can resolve to the gensym'd
name.

## Test status

- `integ`: 148/148 ✓
- `typecheck`: 10/10 ✓
- `test_issue_115` 6/6 ✓
- `test_issue_116` 21/21 ✓
- `test_issue_117` 9/9 ✓
- `test_issue_118` 11/11 ✓
- `test_issue_119` 6/6 ✓
- `test_issue_120` 7/7 ✓ (new)
- End-to-end smoke: `x=2, y=1` after swap, outer `tmp`
  preserved as `"outer-tmp"`.

## What (if anything) is still open

- Rest parameters (`. rest`) on hygienic macros are not
  yet supported. The code falls through to a "no expansion"
  return (make_void) when a hygienic macro has dotted rest.
  A future issue could add this by building a
  `(begin arg1 arg2 ...)` substitute and using quasiquote
  unquote in the body.
- `name_map` and subst are per-expansion maps; for nested
  hygienic macros, each layer gets its own map. Cross-layer
  hygiene (where the outer macro's gensym'd names are visible
  to the inner macro) isn't yet tested.
- Performance: the pre-scan walks the body twice
  (pre-scan + actual clone). For typical macro bodies
  (small), the cost is negligible. For large bodies, a
  single-pass walk that defers reference resolution could
  be faster.

5 files changed, 2 files added, 0 files removed.
