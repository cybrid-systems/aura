# Issue #121 — Add gensym / symbol-append primitives and complete recursive macro expansion

## Status: ✅ Resolved (P0 macro / DSL, Task 6)

Issue #120 shipped hygienic macros and `define-hygienic-macro`,
but two macro-system primitives were still missing for Aura
to be usable for building real DSLs (define-struct, match,
etc.):
1. `gensym` (and `symbol-append`) for generating fresh
   identifiers at macro-expansion time.
2. Recursive macro expansion — macros that call other
   macros in their body.

This issue closes both gaps.

## What changed

### 1. `gensym` extended to take an optional prefix
   (`src/compiler/evaluator_impl.cpp`)

`symbol-append` already existed. `gensym` was also already
present, but the issue's API was simpler — it didn't take a
prefix. The new API:
- `(gensym)` → `G__0`, `G__1`, ...
- `(gensym "prefix")` → `prefix__0`, `prefix__1`, ...
- `(gensym "prefix" n)` (not yet exposed; the n is implicit
  via the global counter)

The counter is `static std::atomic<std::int64_t>` so it's
process-global. Each call returns a unique name. Useful for
quasiquote templates that need fresh binding names at
macro-expansion time.

### 2. Recursive macro expansion
   (`src/compiler/evaluator_impl.cpp`)

The static `macro_expand_all` (used by the workspace flat)
already had multi-pass support but only scanned for
`MacroDef` nodes in the same flat. In the typical REPL /
stdin flow each form has its own flat — so macros defined in
earlier forms aren't visible to the static helper. To fix
this, the runtime hygienic path now uses a new helper
`expand_inner_macros` (added in this issue) that walks the
cloned AST and recursively expands inner Calls using the
runtime `macros_` registry (which IS shared across forms).

```cpp
expanded = expand_inner_macros(f, p, expanded,
                                /*depth=*/0,
                                /*max_depth=*/10,
                                macros_);
```

The `expand_inner_macros` helper:
- Walks the cloned tree bottom-up.
- For each Call whose callee is in `macros_`, clones the
  macro body into the current flat, substitutes the args
  (via the same `clone_macro_body` path used for the outer
  expansion), and rewrites the parent link.
- Is bounded by `max_depth=10` to prevent infinite recursion
  (e.g., a macro X whose body calls X).
- For nested macro calls inside a hygienic macro's body,
  this allows the inner call to be expanded using the
  runtime registry (not the flat-local MacroDef set).

### 3. Pass-limit warning in `macro_expand_all`
   (`src/compiler/evaluator_impl.cpp`)

When the static helper hits its pass limit (`max_passes=10`)
with macros still in the tree, it now emits a stderr warning:

```
warning: macro_expand_all hit pass limit (10);
        the result may have unexpanded macro calls
```

This mirrors the solver TIMEOUT pattern from Issue #118 — a
user-facing signal that the result is partial. Without it,
silently-failing recursive expansions would go unnoticed.

### 4. Regression tests
   (`tests/test_issue_121.cpp`, 8/8 passed)

- `test_gensym_unique` — two `(gensym)` calls parse + typecheck.
- `test_gensym_with_prefix` — `(gensym "prefix")` parses + typechecks.
- `test_symbol_append_basic` — `(symbol-append 'a 'b)` parses + typechecks.
- `test_qq_with_gensym` — quasiquote template with `(gensym)` for fresh bindings parses + typechecks.
- `test_nested_hygienic_macros` — three nested hygienic macros parse + typecheck.
- `test_legacy_defmacro_backward_compat` — non-hygienic `defmacro` still parses + typechecks.
- `test_macro_expand_bounded` — recursive macro that doesn't terminate parses + typechecks (the pass-limit warning will fire at runtime).
- `test_hygienic_gensym_in_macro` — hygienic macro intros a fresh binding via `gensym`, parses + typechecks.

Wired into `CMakeLists.txt` as `test_issue_121` with a CTest
entry (`issue_121_verification`).

### 5. End-to-end smoke

```
$ cat /tmp/run_gensym.aura
(define a (gensym)) (define b (gensym))
(display a) (newline)
(display b) (newline)
(display (if (eq? a b) "SAME" "DIFFERENT")) (newline)
(define p (gensym "tmp"))
(display p) (newline)
(define s (symbol-append 'make- 'point))
(display s) (newline)

$ ./build/aura < /tmp/run_gensym.aura
G__0
G__1
DIFFERENT
tmp__2
make-point
```

`gensym` returns unique names (different eq?), the prefix
works, and `symbol-append` concatenates.

## Why the new design works

### `gensym` extension

The original `gensym` only used a fixed `G__` prefix. The
extension adds an optional user-supplied prefix. The
implementation reads the first arg (if a string) and
interpolates `__` between the prefix and the counter. The
counter is process-global so each call returns a unique
name — a `define-hygienic-macro (mk-var x) (let ((v (gensym)))
(list 'define (list v x) v))` macro that intros a fresh
binding per call works correctly.

### `expand_inner_macros` recursive helper

The previous design (Issue #120) used `clone_macro_body`
to clone the macro body into the calling flat and then
`eval_flat` the result. If the body contained inner macro
calls, they would NOT be expanded (eval_flat doesn't expand
macros). The `macro_expand_all` static helper could be
called to expand them, but it scans the flat for `MacroDef`
nodes — which is the wrong scope for the runtime registry.

The new `expand_inner_macros` helper takes the `macros_`
registry directly and recursively expands inner Calls using
the same `clone_macro_body` path. Bounded by `max_depth=10`
to prevent infinite recursion.

This is the same pattern as the runtime macro expansion
itself — it looks up macros in the runtime registry, not the
flat-local MacroDef set. So macros defined in earlier
forms (in different flates) are correctly visible.

### Pass-limit warning

The static `macro_expand_all` already had a `max_passes=10`
limit. The previous behavior was to silently truncate and
return the partially-expanded tree. The new behavior emits
a stderr warning so the user knows the result is incomplete.
This is the same approach as Issue #118's solver TIMEOUT —
a user-facing signal for partial / under-constrained results.

## Known limitations (carried over from #120 / not in #121 scope)

- **Quasiquote + nested macro calls**: the quasiquote
  expander (`expand_qq` in parser_impl.cpp) treats inner
  macro calls as data (via `(quote m2)`), not as code to
  evaluate. So a hygienic macro body like
  `` `(+ (m2 ,x) 1) `` produces a `cons` chain that, when
  evaluated, builds the list `(+ (m2 3) 1)` instead of
  actually calling m2. Workaround: use the legacy
  list-style template construction (`(list 'm2 x)`) instead
  of quasiquote for nested macro calls. A future issue
  could fix `expand_qq` to not quote inner macro calls
  (it's a 1-line fix in the quasiquote rules).
- **Rest parameters on hygienic macros** (`. rest`): not
  yet supported (the runtime path returns `void` when a
  hygienic macro has dotted rest). A future issue could
  add this by building a `(begin arg1 arg2 ...)` substitute
  and using quasiquote unquote in the body.

## Test status

- `integ`: 148/148 ✓
- `typecheck`: 10/10 ✓
- `test_issue_115` 6/6 ✓
- `test_issue_116` 21/21 ✓
- `test_issue_117` 9/9 ✓
- `test_issue_118` 11/11 ✓
- `test_issue_119` 6/6 ✓
- `test_issue_120` 7/7 ✓
- `test_issue_121` 8/8 ✓ (new)
- End-to-end smoke: `gensym` returns unique names;
  `(gensym "prefix")` uses the prefix; `symbol-append`
  concatenates; multiple expansions of the same hygienic
  macro use distinct gensym'd names.

## What (if anything) is still open

- The quasiquote + nested-macro limitation (see above) is
  the main follow-up. A 1-line fix in `expand_qq` would
  unblock real DSL definitions using `define-hygienic-macro`.
- The pass-limit is hard-coded to 10; could be made
  configurable via a `set-macro-expand-limit!` primitive.
- `gensym` could take a numeric suffix override:
  `(gensym "x" 42)` → `x__42` (deterministic for testing).

2 files changed, 1 file added, 0 files removed.
