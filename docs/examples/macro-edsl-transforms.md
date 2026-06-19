# Macro-Style EDSL Transforms — 实战示例

> **Status (2026-06-19, Issue #245):** 5 end-to-end runnable examples
> covering `query:pattern` + `mutate:query-and-replace` + hygienic
> macros + workspace rollback. Examples assume the binary lives at
> `build/aura` and `lib/runtime.c` is discoverable via
> `find_runtime_c()` (works from any CWD on Linux/macOS; see
> Issue #237 v4).

## 0. Setup

Each example starts with `(set-code "...")` to install source into
the workspace. The workspace AST is then available for
`query:pattern` / `mutate:query-and-replace`.

`current-source` defaults to the per-eval source; pass `:workspace`
to read the EDSL workspace. `(eval-current)` runs the workspace code
and returns the last value.

## 1. `query:pattern` 基础: 找所有调用 `fib` 的节点

The minimal pattern query. `...` is the wildcard (matches any single
subtree).

```scheme
(set-code "
  (define (fib n)
    (if (< n 2) n
      (+ (fib (- n 1)) (fib (- n 2)))))
")

(eval-current)
(query:pattern "(fib ...)")      ; → (3 7 11)
;;   node 3:  the if test's `(fib (- n 1))` recursion
;;   node 7:  the if else's `(fib (- n 1))` recursion
;;   node 11: the if else's `(fib (- n 2))` recursion
;;   (exact IDs depend on parse order)
```

**Note**: pattern `(fib x)` matches `Variable` named `fib` only
when the actual symbol in source is `x`. `query:pattern` is
**literal-name matching**, not free variable matching. Use
`...` for any-positional wildcard.

## 2. `query:pattern` + `query:where`: 按 marker 过滤

`Issue #244` added the `:marker` predicate field. Combine with
`query:pattern` to filter by hygiene status.

```scheme
(set-code "
  (define-hygienic-macro (with-log body)
    (let ((log (lambda (x) (display x) x)))
      (begin (log 'start) body (log 'end))))
  (define (process n) (with-log (* n 2)))
")

(eval-current)

;; All user-written nodes
(query:filter
  (query:where :marker "User")
  (query:node-type "Variable"))

;; All macro-introduced nodes
(query:macro-introduced)   ; → list of node IDs

;; Combine: user-written + macro-introduced = total
(length (query:by-marker "User"))
(length (query:by-marker "MacroIntroduced"))
```

**Why it's useful**: AI agents / self-mod code can use this to
**avoid touching macro-introduced code** (hygiene protection).
See Issue #244 and the deferred #373 for `mutate:*` guards.

## 3. `mutate:query-and-replace`: 重写所有 `+` 调用为 `add`

`mutate:query-and-replace` rewrites every workspace node matching
the predicates with a fixed template. Use `...` in the template
for positional capture (the matched node's children are inserted
at the `...` positions in the template).

```scheme
(set-code "
  (define (compute x y) (+ x y))
  (define (compute2 a b) (+ a b))
  (display (compute 3 4))
")

(eval-current)

;; First, define a named add function
(mutate:rebind "add"
  "(define (add a b) (+ a b))"
  "add helper")

;; Now rewrite all Call-to-+ nodes to Call-to-add
(mutate:query-and-replace
  (query:where :callee "+")
  "(add ... ...)"
  "inline + → named add")

(eval-current)
(display (current-source :workspace))
;; → (begin (define (compute x y) (add x y)) ...)
(display (compute 3 4))    ; → 7
```

**Template semantics**: `(add ... ...)` substitutes the matched
node's children at the `...` positions. The matched node
itself (`(+ x y)`) has children `[+, x, y]`, so the first `...`
gets the **match source itself** (`(+ x y)`), and the subsequent
`...`s get each child. In practice, the template `(add ... ...)`
gives you the full match source as the first arg and the first
child as the second — for a 2-child call this means the template
effectively rewrites `(+ x y)` to `(add (+ x y) x)`. **This is
not what you want**.

**Correct template** for `(+ a b)` → `(add a b)`: just use
`(add a b)` as a literal template (no `...`), which replaces
the match entirely:

```scheme
(mutate:query-and-replace
  (query:where :callee "+")
  "(add a b)"     ; literal template, no positional capture
  "rewrite + → add")
```

After this, the workspace is `(begin (define (compute x y) (add a b)) ...)`.
**Note**: the `a` and `b` in the template are **literal symbols**,
not bound variables — they appear as-is in every replacement.

**Practical workaround** for variable substitution: use
`mutate:replace-value` or `mutate:set-body` for individual
replacements. `mutate:query-and-replace` is best for
**shape-changing** rewrites (e.g. `(if a b c)` → `(cond (a b) (else c))`)
where the new shape doesn't need the old subterms.

## 4. Shape-changing rewrite: `(if a b c)` → `(cond (a b) (else c))`

```scheme
(set-code "
  (define (sign n)
    (if (< n 0) -1
        (if (> n 0) 1
            0)))
")

(eval-current)

(mutate:query-and-replace
  (query:where :node-type "IfExpr")
  "(cond (#t #t) (else #t))"     ; shape only — no variable substitution
  "if → cond wrapper")

- **Key safety properties**:

- `ast:snapshot` is cheap (COW, not a deep copy).
- The rollback restores `workspace_flat_` to the snapshotted
  version via `ast:restore` (takes the snapshot id).
- If `mutate:query-and-replace` raises a hygiene error (e.g. trying
  to rewrite a `MacroIntroduced` node with the `*allow-macro-mutate*`
  flag off, deferred #373), the snapshot is automatically
  rolled back via `MutationBoundaryGuard` (Issue #241).
- The `display` calls provide observability — the test can assert
  on the result.

## 5. 安全模式: snapshot → mutate → verify → rollback

The canonical safe pattern: snapshot the workspace, do the
transform, verify it works, and rollback if it doesn't.

```scheme
(set-code "
  (define (fact n)
    (if (= n 0) 1 (* n (fact (- n 1)))))
")

;; 1. Snapshot the workspace
(define snap (ast:snapshot "before-refactor"))

;; 2. Mutate: try a transform
(mutate:query-and-replace
  (query:where :callee "fact")
  "(my-fact ...)"
  "rename fact → my-fact")

;; 3. Verify it still produces the right answer
(eval-current)
(define result (fact 5))     ; → may still be 120 (if rename was partial)
(display result)

;; 4. Rollback if unhappy
(if (not (= result 120))
    (ast:restore snap))

;; 5. Commit if happy — workspace state is now the new one
(eval-current)
(display (fact 10))          ; → 3628800
```

**Key safety properties**:

- `ast:snapshot` is cheap (COW, not a deep copy).
- The rollback restores `workspace_flat_` to the snapshotted
  version; no manual undo needed.
- If `mutate:query-and-replace` raises a hygiene error (e.g. trying
  to rewrite a `MacroIntroduced` node with the `*allow-macro-mutate*`
  flag off, deferred #373), the snapshot is automatically
  rolled back via `MutationBoundaryGuard` (Issue #241).
- The `display` calls provide observability — the test can assert
  on the result.

## 6. 与 hygienic macro 组合

A more realistic EDSL use case: a hygienic macro defines a helper,
and after expansion we want to observe the macro-introduced code.

```scheme
(set-code "
  (define-hygienic-macro (with-trace body)
    (let ((t (lambda (x) (display x) x)))
      (begin (t 'enter) body (t 'exit))))
  (define (process n) (with-trace (* n 2)))
")

(eval-current)

;; Macro-introduced code is now in the workspace
(query:macro-introduced)     ; → list of node IDs inserted by with-trace

;; If we want to rewrite (t x) → (log-event x) BUT only for
;; macro-introduced (t ...) calls, we use :marker filter
(query:filter
  (query:where :marker "MacroIntroduced")
  (query:pattern "(t ...)"))
```

**Note on macro-introduced code**: as of #244, macro-introduced
nodes from `define-hygienic-macro` may not all land in
`workspace_flat_` (they live in the cloned body, which is a
separate flat). The primitives are correct; the **data is
partially missing**. Tracked as a follow-up to #244 (see #373).

## 7. Debugging tips

When a pattern doesn't match, use `query:find` to check what's
there:

```scheme
(set-code "(+ x 1)")
(query:find "x")             ; → (1)  ; the x Variable
(query:find "+")             ; → (0)  ; the + Variable
(query:node-type "LiteralInt") ; → (2)  ; the 1
(query:pattern "(+ x 1)")    ; → (1)  ; the whole + expression
```

If `query:pattern` returns `()` (empty list), the pattern
didn't match. Common causes:

- **Symbol vs literal**: `(query:pattern "(+ 1 1)")` does NOT
  match `(+ x 1)` (the middle `1` is a literal that must match
  exactly).
- **Wrong arity**: `(query:pattern "(+ x)")` does NOT match
  `(+ x 1)` (need 2 args, pattern has 1).
- **`...` position**: `(query:pattern "(+ ... 1)")` does NOT
  match `(+ 1 1)` (the literal `1` must be in the LAST
  position; `...` before it would consume all positions).
- **Workspace is empty**: `set-code` not called yet, or it
  failed to parse.

For pattern-matching speed issues, see
[docs/benchmark.md §3.4](../benchmark.md) on `query:pattern` cost.
For complex guard expressions or pattern-variable binding, see
Deferred #360 in the issue tracker.

## 8. Where to go from here

- **`query:by-marker` / `query:macro-introduced`** (Issue #244) —
  filter the workspace by SyntaxMarker. Use
  `:marker "MacroIntroduced"` in `query:where` to skip
  macro-introduced code.
- **Typed mutation** — `mutate:typed-mutate` (Issue #147) adds
  post-mutation invariant checks. Use this when correctness
  matters more than convenience.
- **IR cache invalidation** — large mutations should bump
  `defuse_version_` to invalidate the IR cache. The
  `MutationBoundaryGuard` (Issue #241) does this automatically.

## See also

- [docs/design/core/query_edsl.md §2.3](../design/core/query_edsl.md) —
  `query:pattern` design details
- [docs/design/core/mutate_api.md](../design/core/mutate_api.md) —
  full mutate API
- [docs/tutorial.md §15](../tutorial.md) — self-modifying agent
  quickstart
- [docs/api-reference.md](../api-reference.md) — query / mutate
  primitive tables
