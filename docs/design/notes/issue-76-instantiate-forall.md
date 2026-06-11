# Design: Issue #76 — `instantiate_forall` early termination

## Current state

`src/core/type_impl.cpp:375`:

```cpp
TypeId TypeRegistry::instantiate_forall(TypeId forall_id,
                                         const std::vector<TypeId>& args) {
    TypeId result = forall_id;
    std::size_t arg_idx = 0;
    while (auto* ft = forall_of(result)) {
        if (arg_idx >= args.size())
            break; // 剩余 forall 保留
        std::unordered_map<std::uint32_t, TypeId> subst;
        subst[ft->var.index] = args[arg_idx++];
        result = substitute(ft->body, subst);
    }
    return result;
}
```

The `break` at `arg_idx >= args.size()` is the bug. When callers supply
fewer arguments than the depth of the nested Forall chain, the loop
returns the residual `∀a. T` with `a` still free.

## Why this is unsound

Consider a value typed as `forall a. Int` that the type-checker
decides to instantiate for a monomorphic context (e.g. a function
argument). If `args = []`, the current code returns `forall a. Int`
unchanged. A downstream monomorphization step would see a Forall type
in a monomorphic position and either:
1. Crash (the IR pipeline doesn't expect free vars there)
2. Pick an arbitrary `a` and substitute it, leading to non-determinism
3. Silently leak the var into compiled code, leading to broken
   monomorphization assumptions

Any of these is a soundness hole: a value claimed to be a
monomorphic `Int` is actually still polymorphic.

## The fix

Replace the early `break` with **continuing instantiation using fresh
type variables**. When args is exhausted, the remaining Foralls get
fresh vars from `make_var`. The result is fully instantiated (no
free vars), so any monomorphic context that consumes it stays sound.

```cpp
TypeId TypeRegistry::instantiate_forall(TypeId forall_id,
                                         const std::vector<TypeId>& args) {
    TypeId result = forall_id;
    std::size_t arg_idx = 0;
    while (auto* ft = forall_of(result)) {
        std::unordered_map<std::uint32_t, TypeId> subst;
        if (arg_idx < args.size()) {
            subst[ft->var.index] = args[arg_idx++];
        } else {
            // Args exhausted: use a fresh type variable for the
            // residual bound var. This is sound: the caller asked
            // for a monomorphized value; we hand them a Forall-body
            // where the residual var is fresh and unconstrained.
            subst[ft->var.index] = make_var("");
        }
        result = substitute(ft->body, subst);
    }
    return result;
}
```

### Why fresh vars (not error/reject)?

The alternative designs were:

- **Error/reject under-instantiation**: throw an exception, return
  invalid TypeId, or log a diagnostic. The downside: every caller
  has to know exactly the forall depth, which is fragile when the
  type comes from a generic library (e.g. user calls `List<T>` but
  only knows about `T`).

- **Skip residual foralls**: same as the current bug — just a
  different shape of unsoundness (returns the body, with a free var
  still inside).

Fresh vars are the "auto-monomorphize" choice: the caller gets a
fully-instantiated type, the residual var is genuinely free, and
the monomorphization step can do whatever it wants with it (likely
unify it with the expected type, or generate a fresh instantiation
for each use site). This is the path that gives callers the most
ergonomic API while preserving soundness.

### Risk: caller of `instantiate_forall` expects a specific shape

Today, `instantiate_forall` is declared but has **no call sites** in
the codebase. So the behavior change is safe. If future callers
rely on the residual Forall being preserved, they'll need to
either:
- supply enough args to fully instantiate
- use a new `instantiate_forall_partial` that returns the residual
  if any

We'll add the partial variant later if needed. For now, the
simple fully-instantiate behavior is correct.

## Test design

`tests/suite/instantiate_forall.aura`:

1. **No under-instantiation**: `(forall a. Int)` with `args=[]`
   returns the body type, not a Forall. (Hard to test without
   exposing `instantiate_forall` to Aura — we expose it as a
   primitive.)
2. **Partial under-instantiation**: `(forall a. forall b. (a, b))`
   with `args=[Int]` returns `forall b. (Int, b)` → with `args=[]`
   it returns `(fresh_var, fresh_var_2)`.
3. **Full instantiation**: `args=[Int, String]` returns `(Int, String)`.
4. **Original bug regression**: pre-fix, `args=[]` on
   `forall a. Int` returned a Forall; post-fix, it returns Int.

### Exposing the primitive

Add a new primitive:
```cpp
primitives_.add("instantiate-forall", [this](const auto& a) -> EvalValue {
    if (a.size() < 1 || !is_forall_id(a[0])) {
        // not a forall id — return it unchanged
        return a[0];
    }
    auto fid = as_forall_id(a[0]);
    std::vector<TypeId> args;
    for (std::size_t i = 1; i < a.size(); ++i) {
        args.push_back(a[i]);
    }
    return instantiate_forall(fid, args);
});
```

Actually `instantiate-forall` returns a TypeId, not an Aura value.
Aura doesn't have a direct TypeId type. For testing, we can:
- Expose `(type-equal? T1 T2)` and check structural equality
- Or expose `(is-forall? T)` to check the residual
- Or expose `(type-instantiate-forall T . args)` that returns a
  hash with the resulting type and whether it was a Forall

For simplicity, I'll go with: a primitive `(instantiate-forall T . args)`
that returns a hash `{type: <TypeId-as-value>, is-forall: <bool>}`.
The internal TypeId is wrapped as a value via `make_int(idx)` (the
`entries_` index). The test can compare indices.

Wait — the test value identity is fragile (indices change). A better
exposition: return a `format_type` string for the result, and a bool
for `is-forall`. Then the test compares strings.

Actually, simplest: just expose `format_type` and `is_subtype` (already
exposed indirectly via the type-checker). The test can do:
```aura
(define r1 (instantiate-forall (type-of (lambda (x) x))))
(check= "a" r1)   ; fresh var named "a"
```

For the test, I'll add a `(type-of)` overload that returns a real
TypeId-as-int. Or, simpler, just format the type and check the
string.

Actually let me think simpler. I'll expose the function and have it
return a string (formatted type) plus a bool for "is forall
remaining". The test verifies the residual is not a Forall.

## Implementation plan

1. Fix `instantiate_forall` in `type_impl.cpp` (3-line change).
2. Expose `(instantiate-forall T . args)` primitive in
   `evaluator_impl.cpp` (returns hash with formatted type and
   `is-forall` flag).
3. Write `tests/suite/instantiate_forall.aura`.
4. Run all existing tests for regressions.

## Acceptance criteria

- `instantiate_forall(forall a. Int, [])` returns a non-Forall type
  (fresh var instead of the residual a)
- The fresh var is genuinely unconstrained (not equal to any of
  the input args)
- Existing 30/30 suite, 5/5 leak, 285/285 run-tests all pass

## Out of scope

- Higher-kinded types (follow-up to #70)
- Bounded polymorphism `<: T` constraints
- Hash-based TypeId interning (only if profiling shows it hot)
- A "partial" variant of instantiate_forall (caller can supply
  fewer args, get a residual Forall back); not needed until
  someone wants it.
