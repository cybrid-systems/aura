# Design: Issue #77 — `substitute` boundary / variable capture

## Current state

`src/core/type_impl.cpp:559`:

```cpp
case TypeTag::FORALL: {
    auto* ft = forall_of(ty);
    return register_forall(ft->var, substitute(ft->body, subst));
}
```

The `subst` map is passed through unchanged when recursing into a
Forall's body. This means a free variable inside the body that
happens to share its index with the bound variable will be
substituted.

## The bug

The classic capture case:

```
subst = { x.index: Int }   // user wants to replace x with Int
type  = forall x. (x → x)  // x is bound
```

Walking the tree:
- Forall case: recurse on body (x → x) with **same** subst
- Func case: substitute each x with the subst entry → Int
- Result: `forall x. (Int → Int)` — but `x` is bound, so the binding
  is now meaningless. The user's substitution polluted the scope.

The reverse case (free var in body coincidentally shares index
with bound var):

```
subst = { y.index: Int }   // user wants to replace outer y
type  = forall x. (y → y)  // x is bound, y is free
```

Walking the tree:
- Forall case: recurse on body (y → y) with **same** subst
- Func case: substitute each y → Int
- Result: `forall x. (Int → Int)` — user's substitution on outer
  y accidentally replaced the inner y too. (Whether this is
  *correct* depends on whether the inner y is "the same variable
  as" the outer y. In standard HM, no — the inner y is bound
  and thus a fresh variable. So this is a soundness bug.)

## Why it matters in practice

`substitute` is called from:
- `instantiate` (1 layer, used in #70 subtyping)
- `instantiate_forall` (N layers, used in tests)
- Future code that does generic instantiation, monomorphization,
  or constraint solving

For `instantiate_forall` the current code happens to work because
it peels off one Forall at a time, and each peel's subst contains
only the bound var's index. The bug bites anyone who calls
`substitute` directly with a subst that has a free-var binding
matching a bound var's index.

For `instantiate` (single Forall) the same arg applies — its
subst is built from the bound var, so the bug doesn't manifest
in the current call sites.

So the bug is **latent** today but real. AI-generated code that
calls `substitute` directly with a custom subst is exactly the
kind of path that could trigger it (the issue's impact line:
"silent type corruption in complex polymorphic code (especially
AI-generated)").

## The fix

Standard capture-avoiding substitution: when entering a Forall
scope, shadow the bound variable by removing its index from the
subst before recursing.

```cpp
case TypeTag::FORALL: {
    auto* ft = forall_of(ty);
    // Capture avoidance: bound var shadows any same-indexed
    // entry in subst for the body. (Standard HM.)
    auto inner_subst = subst;
    inner_subst.erase(ft->var.index);
    return register_forall(ft->var, substitute(ft->body, inner_subst));
}
```

### Why this is correct

- The bound variable is a *new* variable that happens to share an
  index with whatever was outer. Inside the body, the user can
  rename it freely without affecting outer code.
- If outer subst has `[idx → T]`, that mapping is for the OUTER
  variable with that index, not the inner bound one. The bound
  var shadows it; references to that index inside the body refer
  to the bound var, not the outer var.
- This is exactly how the Hindley-Milner reference algorithms
  handle it (e.g. Algorithm W, Algorithm J).

### Why not De Bruijn indices

A more robust fix would be De Bruijn indices where the bound var
is shifted to a new index, eliminating the collision entirely.
That's a much larger refactor (every ForallType's var would need
to be re-indexed, every read site would need to know about
shifting). The "erase from subst" fix is a minimal, correct
change that addresses the current soundness bug without breaking
the rest of the code.

If we later want to support substitution in a way that *adds*
new Foralls (rather than peeling existing ones), we'll need De
Bruijn. For now, the simple fix is enough.

### Why not a "free in" set

The alternative is to compute a "free in" set for the body and
filter the subst to only the free vars. This is equivalent to
"erase from subst" in correctness terms, but slower (requires
a full walk to compute the free vars first). Stick with erase.

## Test plan

`tests/test_ir.cpp` section 19:

Add new test cases that directly exercise `substitute` with the
problematic patterns:

1. **Bound var in body, subst targets that var**:
   - Type: `forall x. (x → x)` (x.index = 5)
   - subst: `{5 → Int}`  (target the bound var)
   - Expected: result is `forall x. (x → x)` unchanged (bound var
     shadows subst entry; no replacement)
   - Pre-fix bug: result is `forall x. (Int → Int)` (x binding
     polluted)

2. **Free var in body shares index with bound var**:
   - Make outer y (index 7), make inner x (index 7 — same!)
   - Build type: `forall x. (y → y)` (with y at index 7)
   - subst: `{7 → Int}`
   - Expected: result is `forall x. (y → y)` unchanged (bound x
     shadows the outer y's index; the inner y is x, not the
     outer y, so the subst doesn't apply)

   Wait — in the existing code, each `make_var` makes a fresh
   var with a new index. So I can't easily make a var with an
   index collision via the public API. Let me think of a
   different test that exposes the bug.

3. **Direct substitute with a free-var subst** (the real
   AI-generated path):
   - Build type: `forall a. (a → b)` (a bound, b free)
   - Build subst: `{b.index → Int}` (target the free b)
   - Expected: result is `forall a. (a → Int)` (only b is
     substituted, a is preserved as the bound var)
   - This works correctly with the current code because b is
     not in the bound var's position. The bug needs a more
     contrived case to manifest.

   Actually let me think about the ONLY case where the bug
   manifests: a Forall's bound var index is the same as a free
   var's index in the body. Since each make_var gives a fresh
   index, this requires the body to be constructed with a
   bound var that happened to match a previous index.

   The natural way to trigger this: nested Foralls where the
   inner Forall's body references a free var with the same
   index as the outer Forall's bound var.

4. **Nested Foralls with index collision** (the real test):
   - Make a (index 10), make b (index 11), make c (index 11 — same as b)
   - Build type: `forall a. (forall b. (a → c))`
   - Build subst: `{a.index → Int, c.index → String}` (target outer a and free c)
   - Walk:
     - Outer Forall a (10), body = forall b. (a → c)
     - With my fix: inner_subst = subst.erase(10) = {11: String}
     - Recurse on forall b. (a → c) with inner_subst
       - Inner Forall b (11), body = (a → c)
       - With my fix: inner_inner_subst = inner_subst.erase(11) = {}
       - Recurse on (a → c) with empty subst
         - a (10): not in subst, returns a
         - c (11): not in subst, returns c
       - Return register_forall(b, a → c) → reuse the same forall b. (a → c)
     - Return register_forall(a, forall b. (a → c)) → reuse the same forall a. (forall b. (a → c))
   - Result: unchanged
   - Pre-fix bug: inner_inner_subst would have been {11: String}, so c gets replaced:
     - c (11) is in subst, returns String
     - Return forall b. (a → String)
     - Then register_forall(a, ...) returns forall a. (forall b. (a → String))
   - The pre-fix result has the bug: c (the free var) was incorrectly
     substituted. The post-fix result preserves c.

5. **Reuse a var that matches an outer bound var index** (forced collision):
   - Make a (index 10)
   - Make inner_b (the same TypeId as a? No, that's not how make_var works)
   - Skip this — can't easily construct

Let me settle on test 4 as the main regression test. The setup
is:
- a = make_var("a")
- b = make_var("b")
- c = make_var("c")   // free var, will share index? No, each make_var gives a new index
- forall a. (forall b. (a → c)) — this has a bound at 10, b bound at 11, c free at 12
- subst = {10: Int, 12: String}
- Expected: forall a. (forall b. (Int → String)) — a replaced, c replaced, b preserved

OK this test is interesting. Let me trace it through:

With my fix:
- Outer Forall a (10), body = forall b. (a → c)
  - inner_subst = subst.erase(10) = {12: String}
  - Recurse on forall b. (a → c)
    - Inner Forall b (11), body = (a → c)
      - inner_inner_subst = inner_subst.erase(11) = {12: String}
      - Recurse on (a → c)
        - a (10): not in inner_inner_subst, returns a (10)
        - c (12): IS in inner_inner_subst, returns String
        - return register_func({a}, String) → new func Func{a, String}
      - return register_forall(b, Func{a, String}) → new forall forall b. (a → String)
    - returns forall b. (a → String)
  - return register_forall(a, forall b. (a → String)) → new forall forall a. (forall b. (a → String))

Final: forall a. (forall b. (a → String)) — a NOT replaced because a is bound by the outer Forall. c IS replaced with String.

Hmm, that's a "capturing" result — the user's outer subst of a→Int didn't take effect because a is bound. That's actually correct semantics: subst's `[a.index → Int]` is for the *outer* a, not the inner bound a. Inside the body, the index 10 is the bound a, not the outer a.

But it's confusing for users. Let me make the test more clear:

Simpler test: just two vars, no collision.
- a = make_var("a")
- b = make_var("b")
- type: forall a. (a → b)  — a bound, b free
- subst: {a.index → Int}  — target the bound a
- Expected: forall a. (a → b)  (unchanged — the bound a shadows)

Trace with my fix:
- Outer Forall a, body = (a → b)
  - inner_subst = subst.erase(a.index) = {}
  - Recurse on (a → b)
    - a: not in subst, returns a
    - b: not in subst, returns b
  - return forall a. (a → b) — unchanged

Without my fix:
- Outer Forall a, body = (a → b)
  - Recurse on (a → b) with same subst
    - a: IS in subst, returns Int
    - b: not in subst, returns b
  - return forall a. (Int → b) — WRONG! The bound a was replaced.

Yes — this is the bug. Test passes if we get `forall a. (a → b)`, fails (regression) if we get `forall a. (Int → b)`.

OK so the simple test is:
- a = make_var("a")
- b = make_var("b")
- type: forall a. (a → b)
- subst: {a.index → Int}
- Expected: forall a. (a → b) (preserved, not substituted)

And the multi-layer test:
- a = make_var("a")
- b = make_var("b")
- c = make_var("c")
- type: forall a. (forall b. (a → c))
- subst: {a.index → Int, c.index → String}
- Expected: forall a. (forall b. (a → String))  (c free → substituted; a bound → NOT)

Actually with my fix trace above, the result was `forall a. (forall b. (a → String))`. The a inside the body is still a (the bound var), not Int. The c (free) becomes String.

Pre-fix:
- Outer Forall a, body = forall b. (a → c), subst = {10: Int, 12: String}
- Recurse on forall b. (a → c) with same subst
  - Inner Forall b, body = (a → c)
  - Recurse on (a → c) with same subst
    - a: IS in subst, returns Int
    - c: IS in subst, returns String
  - return forall b. (Int → String)
- return forall a. (forall b. (Int → String))

Pre-fix result: forall a. (forall b. (Int → String))
Post-fix result: forall a. (forall b. (a → String))

The pre-fix result has BOTH a and c replaced. Post-fix has only c replaced. The post-fix is correct.

OK so test 4 is the regression test. Let me write the design and code.

## Implementation plan

1. Fix `substitute` in `type_impl.cpp` line 559 — 2-line change.
2. Add 2 new test cases in `test_ir.cpp` section 19.
3. Run all tests for regressions.

## Acceptance criteria

- `substitute(forall a. (a → b), {a → Int})` returns `forall a. (a → b)` (unchanged)
- `substitute(forall a. (forall b. (a → c)), {a → Int, c → String})` returns
  `forall a. (forall b. (a → String))` (only c is substituted; a is bound)
- All existing tests still pass

## Out of scope

- De Bruijn indices (would require a full refactor)
- Free-in precomputation (slower than erase-from-subst)
- Higher-kinded types
- Bounded polymorphism
