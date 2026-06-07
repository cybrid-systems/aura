# Issue #108 part 4 follow-up: datatype ADT macro

## Status: deferred (out of scope for this PR)

Parts 1-3 of #108 shipped (`08a3046`, `ed11de2`, `d65e8a7`).
Part 4 (the `datatype` ADT macro) was attempted but rolled back.
This document captures why and what the real fix would need.

## What the benchmark wants

`adt-tree.aura` and `adt-either.aura` ask the LLM to write:
```
(datatype (Tree : T)
  (Leaf T)
  (Node Tree Tree))
(match (Leaf 42)
  ((Leaf x) (display 1))
  (else (display 0)))
```

`(Leaf 42)` and `(Node l r)` are constructor calls.
`match` then needs to recognize `(Leaf x)` as a constructor pattern
in its clauses.

## What the existing Aura code already has

The parser's `match` (parser_impl.cpp `compile_pattern`) already
supports constructor patterns — the `Call` branch handles callee
names that aren't `list`/`cons`/`pair` as constructor tags.
Constructors are expected to return:

```
(cons "CtorName" (list arg1 arg2 ...))
```

The test: `(pair? tmp) && (string=? (car tmp) "CtorName") && walk-sub-patterns`.

So **only the constructor-creating side is missing**.

## What I tried (rolled back)

### Attempt 1: Aura macro in `std/adt.aura`

A `defmacro` that expands `(datatype ...)` into:
```
(begin
  (define Ctor1 (lambda args (cons "Ctor1" args)))
  (define Ctor2 (lambda args (cons "Ctor2" args))))
```

`defmacro` in Aura has pre-existing limitations that I hit:
- Rest parameters (`(datatype spec . ctors)`) inside a macro body
  crash or don't substitute properly when nested in `let`/`map`/
  `lambda` forms inside the body.
- Multi-form macro body returns `()` instead of evaluating to the
  return form.

These are macro-expander bugs, not `datatype`-specific. Fixing them
is a separate piece of work.

### Attempt 2: Parser special form `parse_datatype`

Added `parse_datatype` in parser_impl.cpp that parses
`(datatype (Name : T) (Ctor1 ...) ...)` and emits a Begin with
Define nodes for each constructor.

This compiled and parsed, but the runtime behavior was wrong:

```
$ (datatype (Tree : T) (Leaf T) (Node Tree Tree))
$ (display Leaf)
error: unbound variable: Leaf
```

The Begin's Defines are scoped to the Begin, not the top level.
Subsequent top-level expressions can't see the constructors.

Verification: same scoping happens with regular `begin`:
```
$ (begin (define X 1))
$ (display X)
error: unbound variable: X
```

But:
```
$ (define X 1)
$ (display X)
1
```

works. So top-level Defines are global, but Begin's Defines are
scoped. This is a fundamental Aura scoping rule.

## Why this is hard

The parser returns a single root node for each top-level form.
For `datatype` to make constructors globally available, we'd need
either:

1. **Multiple top-level roots from one form** — the parser only
   returns one node. To expose N Defines globally, we'd need to
   flatten the Begin somehow.

2. **A "top-level" Begin mode** — the eval would need to recognize
   "this Begin was a top-level form, not nested in another form,
   so its Defines leak out". But there's no syntactic distinction
   between top-level Begin and nested Begin.

3. **A new evaluator primitive that registers constructors in a
   global table** — separate from define. Subsequent `(Leaf 42)`
   would look up Leaf in the global table, not the env. But this
   requires changing the eval/lookup path, which is invasive.

4. **Pre-registering all known constructors at compile time** —
   only works for a fixed set of ADTs. Doesn't scale.

## Recommended path forward

Option 3 is the most general. Sketch:

1. Add `adt_register(name, ctor-name)` primitive in evaluator_impl.cpp.
2. Parser emits a single `AdtRegister` node containing the
   constructor list. Eval of this node populates a global
   `g_adt_constructors` map.
3. Variable lookup checks the env first, then `g_adt_constructors`.
4. `g_adt_constructors` entries are closures over the constructor
   name string (so `(Leaf 42)` returns `(cons "Leaf" (42))`).

This bypasses the Begin-scoped-define issue entirely: the
constructors live in a separate table, not the env.

Estimated effort: 200-300 lines of C++, plus tests.

## Why I rolled back

The parser-side change (Attempt 2) was 60 lines but didn't actually
work because of Aura's scoping rules. Keeping it in the tree as
"compiles but doesn't function" would mislead future readers. Better
to document the gap and ship parts 1-3 cleanly.

The fact that `match` already supports constructor patterns means
the "match side" of ADT is essentially done — only the "construct
side" is missing, and the recommended path above is well-scoped.

## Related follow-ups from this exercise

While testing, I noticed that `ast:defs` (added in part 2) only
finds top-level Defines, not Defines inside a top-level Begin. The
EDSL benchmark doesn't currently hit this, but it's a pre-existing
Aura oddity worth fixing. Tracked separately.
