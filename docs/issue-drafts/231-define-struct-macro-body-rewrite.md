# Issue #231 — define-struct macro body rewrite + env-binding path

## Origin

This is the follow-up to [Issue #230](../issue-closings/) bugs #1 and
#2. The Quote-wrap fix in commit 6b90641 ("fix(230 #1,#2
partial)") and the defmacro-survives-set-code fix in commit b092353
both shipped, but the deeper issue — `define-struct` (and similar
symbol-generating macros) producing broken constructors — is not
fully fixed. This issue tracks that remaining work.

## The bug (recap)

`(define-struct foo (a b))` produces a `make-foo` that, when called
with `(make-foo 1 2)`, returns a vector containing the **symbols**
`a` and `b` rather than the values `1` and `2`. The macro body's
`a` and `b` in the make function get captured from the macro's
`let`/`fields` lexical scope, not from the lambda's parameter
scope. Trace:

```scheme
(define-hygienic-macro (define-struct name fields)
  (let ((mk (symbol-append "make-" name))
        (pred (symbol-append name "?")))
    (let build ((i 1) (fs fields) (acc
      (list
        ...
        (list (quote define) mk
          (list (quote lambda) fields       ; <-- (a b) are the params
            (cons (quote vector)
              (cons (list (quote quote) name)
                (map (lambda (f) f) fields))))))))
        ...)))
```

After `define-struct foo (a b)`, the body produces a make function
whose body is the cons expression. When the make function is called
with `(1 2)`, the body's `fields` resolves via the Quote-wrap to
the list `(a b)` (the macro's value), and the `map` produces
`(a b)` (symbols). The vector ends up `[foo a b]`.

## What's already done

1. `clone_macro_body` now wraps macro-param Variables in a `Quote`
   node (commit 6b90641). This makes `name` evaluate to the literal
   symbol `foo` rather than a Variable lookup — fixes the *name*
   half of the issue, but not the *fields* half.
2. `define-hygienic-macro*` parser keyword and `&`-sigil preserved-
   param infrastructure (commit 6b90641). The env-binding expansion
   path that would make this work for symbol-generating macros was
   attempted and reverted in the same commit's WIP — see the
   detailed write-up in `memory/2026-06-18.md`.
3. `set-code` no longer clears the runtime `macros_` registry and
   allocates a fresh pool/flat each call (commit b092353). This
   fixes the orthogonal `defmacro survives set-code` bug (issue
   #230 bug #3).

## The remaining work (two parts)

### Part A — env-binding path for `define-hygienic-macro*`

The current `define-hygienic-macro` macro expansion path
(`evaluator_impl.cpp:18927-18998`) walks the body, converts macro-
param Variables to Quote-wrapped AST nodes, and lets
`eval_data_as_code` re-evaluate the result. For bodies that
reference the macro's params as data (e.g., a symbol-generating
macro that builds code with the user's struct name), this works.

For bodies that reference a lambda-local symbol (a symbol bound
inside a generated `lambda` body but not as a macro param), the
Quote-wrap turns the symbol into a literal — but the lambda
body still needs the symbol as a Variable reference to the
lambda's bound param. The two needs conflict.

The `define-hygienic-macro*` (with `*` suffix) variant was
designed for this case: all params are bound in a child env, the
body is evaluated in that env, and the body's Variables (for
macro params) look up the literal values directly. The parser
keyword and `&` sigil are in place; the call-site env-binding
path was started (lines around 18995-19010 in evaluator_impl.cpp
on the branch) and reverted because:

1. **Pool mismatch.** The cloned closure body lives in a fresh
   `cl_pool` (`evaluator_impl.cpp:18160-18170`); the env's
   bindings are interned in the macro's `pool`. The body's
   Variable symids and the env's binding symids are in different
   pools, so `Env::lookup_by_symid` (the fast path) returns
   miss. The string-based fallback may or may not hit depending
   on interning order.
2. **Nested-binding shadowing.** Even with the pool fix, the
   body has Variables for nested let/lambda bindings (e.g.,
   `mk`, `pred`, `i`, `fs`, `acc`, `f` inside a `let build`).
   The clone_macro_body's hygiene rename gensyms these, so
   they're renamed in the body — but the env-binding path
   bypasses the subst that was keeping them aligned with the
   macro params. The fix is to run the existing hygiene rename
   (with `name_map`, no `subst`) and then look up the body's
   Variables in the env, but getting the symid alignment
   right is fiddly.

The cleanest fix is to skip `eval_data_as_code` entirely and
use `data_to_flat` (already exists at
`evaluator_impl.cpp:17705`) to convert the macro's output
value to AST nodes directly, then `eval_flat` the AST. The
`eval_data_as_code` lambda case already uses `data_to_flat`
for the body — the issue is only the pool alignment for the
closure's body and the env's bindings.

### Part B — `lib/std/struct.aura` body rewrite

Once Part A is done (or once a primitive workaround is in
place — see `memory/2026-06-18.md` for the
"add list->vector primitive" investigation), the
`lib/std/struct.aura` body should be rewritten to use a
variadic `(lambda args ...)` shape so the make function takes
the call args as a single list and builds the vector via
`(list->vector (cons ',name args))` or
`(apply vector (cons ',name args))`. The current body uses
`(lambda fields ...)` which is what the issue title calls the
"hygiene bug" — `fields` is used both as the lambda's
parameter list AND as the data to map over, and the gensym pass
conflates the two.

## Acceptance criteria

- `(define-struct foo (a b))` followed by
  `(define f (make-foo 1 2))` produces `f = #(foo 1 2)`.
- `(foo? f)` returns `#t`.
- `(foo-a f)` returns `1`, `(foo-b f)` returns `2`.
- No regressions in `test_issue_165` (6/6), `test_issue_166`
  (10/10), bash suite (315/315), `tests/suite/edsl*.aura`,
  `tests/suite/typesystem.aura`.
- The env-binding path for `define-hygienic-macro*` is wired
  up and documented (or, if workaround primitive is chosen,
  the workaround is documented and the env-binding path
  remains as a follow-up).

## Notes

- The Arena growth trade-off from commit b092353 (one pool/flat
  per set-code call, instead of the old "reuse and clear" OOM
  fix) is independent of this work and is NOT a regression
  vector for this fix. The OOM fix can be re-applied with a
  generational pool pool if 750 consecutive set-codes become
  a benchmark target again.
- Issue #230's #4 (max_passes 10→32) and #3 (defmacro
  survives set-code) are already shipped. This issue is purely
  about #1 (define-struct) and #2 (dhm* preserve semantics).

## Status (2026-06-18)

✅ **SHIPPED** in commit 5171113 (also closes #230). The
env-binding path is implemented, lib/std/struct.aura
rewritten, data_to_flat default Call handler updated to
add_variable, all tests green. See the close comments on
issue #230 and #334 for the full shipped details.
