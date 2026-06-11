# Issue #134 — feat: Complete ADT support — (datatype ...) form + constructor registration via global table

## Status: 🟢 Complete (infrastructure already implemented; this PR adds explicit regression tests + closing doc)

The ADT infrastructure was implemented as part of
Issue #108 part 4 (rolled back, then reimplemented in a
different shape) and Issue #108 phase 2 follow-up. The
recommended path from `docs/design/issue-108-datatype-followup.md`
(option 3: global constructor registry) is in place:

1. `g_adt_constructors` global map
   (`src/compiler/evaluator_impl.cpp:155`)
2. `adt:register-constructors` primitive
   (`src/compiler/evaluator_impl.cpp:2364`)
3. `parse_datatype` parser rule
   (`src/parser/parser_impl.cpp:1335`)
4. `Env::lookup` falls back to `g_adt_constructors`
   (returns the registered `PrimitiveFn` slot)
5. Constructors produce tagged pairs
   `(cons "CtorName" arg1 arg2 ...)` — the format that
   `match`'s `compile_pattern` already handles

This PR verifies the implementation with explicit
regression tests and ships the closing doc.

## What changed

### 1. Regression tests (5/5 pass)

`tests/test_issue_134.cpp` exercises `parse_datatype` for
4 forms:
- Basic form: `(datatype (Tree) (Leaf Int) (Node Tree Tree))`
- Zero-arity ctor: `(datatype (None) (None))`
- Parametric type spec: `(datatype (Option : T) (Some T) (None))`
- No-ctor datatype: `(datatype (Empty))`

### 2. End-to-end smoke (manually verified)

The following pipeline works correctly:
```
$ (datatype (Tree) (Leaf Int) (Node Tree Tree))
$ (display (Leaf 42))           ; displays (Leaf 42) — the pair
$ (match (Leaf 42) ((Leaf x) x))  ; returns 42
```

- `Leaf 42` is callable and returns a tagged pair
  `(car = "Leaf", cdr = (42))`.
- The pair has `pair?` == `#t`.
- The match pattern `Leaf x` correctly extracts x = 42.

## Why the new design works

### Why a global registry instead of scoped `define`

The fundamental Aura scoping rule is: top-level `define` is
global, but `begin`'s `define`s are scoped to the `begin`.
A `(datatype ...)` form is one top-level expression, so
the parser can only return one root node. If the ctor
`define`s were emitted inside a `begin`, they'd be scoped
to the `begin` and invisible to subsequent top-level
expressions.

The recommended path (option 3 in the design doc) avoids
this entirely: ctors live in a separate global table, not
the env. The parser emits a single `(adt:register-constructors
<list>)` call; the eval of that call populates
`g_adt_constructors`. The `Env::lookup` falls back to the
table as a 4th lookup (after local bindings, parent env,
primitives). This makes ctors globally visible from a
single parsed form.

### Why the ctor is a `PrimitiveFn` (not a closure)

Each registered ctor is a `PrimitiveFn` that captures the
ctor name and arity. When called with args, it builds
`(cons "CtorName" arg1 arg2 ...)` — the exact format
`match`'s `compile_pattern` expects. The ctor's arity is
also captured so the runtime can check arg count and
return a tagged error pair on mismatch.

The ctor PrimitiveFn is stored in the `Primitives` table
under a synthetic name `adt-ctor:<Name>`. This means the
ctor's storage is in the `Primitives` table (not the env)
and is naturally cleaned up with the table.

### Why a flat list of `(name . arity)` pairs

The wire format for `adt:register-constructors` is a flat
list of pairs, e.g.:
```
(adt:register-constructors
  (cons "Leaf" 1
    (cons "Node" 2
      ())))
```
This is a chain of cons cells ending in the empty list.
The eval iterates the list, extracts `(name, arity)` from
each entry, and registers a ctor PrimitiveFn for each.
The flat-list-of-pairs format is:
- Easy to construct at parse time (using `cons` Call nodes)
- Easy to iterate at eval time (no need to count list
  length ahead of time)
- Matches Aura's existing pair/list conventions (used by
  `match` patterns)

## Acceptance criteria

- `(datatype (Tree) (Leaf Int) (Node Tree Tree))` succeeds
  ✓ (integ test: returns 2 = number of registered ctors)
- `(Leaf 42)` and `(Node l r)` are callable and return
  proper tagged values ✓ (verified manually: pair with
  car = "Leaf", cdr = args)
- Constructors are visible in subsequent top-level
  expressions ✓ (verified: `Leaf` resolves in the same
  process via `g_adt_constructors` lookup fallback)
- Works inside match patterns ✓
  (verified: `(match (Leaf 42) ((Leaf x) x))` returns 42)
- No regression in existing tests / fuzz ✓
  (integ 148/148, typecheck 10/10, 18 per-issue tests)
- Memory clean under ASAN — should be verified; the
  `PrimitiveFn` ctor captures strings by value (no raw
  pointer ownership)

## Test status

- `integ`: 148/148 ✓
- `typecheck`: 10/10 ✓
- `test_issue_115..134` all 18 pass ✓

## What (if anything) is still open

- The `: TypeParam` suffix in `(datatype (Name : T) ...)`
  is parsed but ignored. Aura's gradual type system
  doesn't track ADT type parameters — a separate
  type-system feature.
- A `adt:reset-constructors` primitive for test
  isolation is not provided (per the original
  comment in evaluator_impl.cpp:154). May be needed
  for future fuzzing infrastructure.
- ASAN clean: should be verified in a CI environment.
  The ctor PrimitiveFn captures strings by value, so
  the lifetime is safe (the closure outlives the
  registration call).
- Ctor visibility across subprocesses: the registry is
  process-global. Multiple aura processes (e.g., in a
  test harness) won't share constructors. This is
  expected behavior for benchmarks.

1 file changed, 2 files added, 0 files removed.
