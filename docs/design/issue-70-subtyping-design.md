# Design: Issue #70 — Implement `is_subtype` (currently a stub)

## Current state

`TypeRegistry::is_subtype` in `src/core/type_impl.cpp:271`:

```cpp
bool TypeRegistry::is_subtype(TypeId sub, TypeId sup) const {
    if (sub == sup)
        return true;
    if (sup == dynamic_type())
        return true;
    return false;
}
```

This is a complete stub. It only handles:
- Reflexivity: `T <: T`
- Dynamic: `T <: Any`

Every other subtyping question returns `false`, regardless of structural
relationship. This makes every type-checker decision that depends on
subtyping (annotation-vs-context, refinement, parametric polymorphism)
flat-out wrong, even when the result "should" be `true`.

## Call sites

Found via grep:

1. `src/compiler/type_checker_impl.cpp:2145` — annotation vs context
   compatibility for `(: x AnnType expr)` in check mode.

That's the only call site right now. It's used in `consistent_unify`
fallback: if the unification fails, fall back to subtyping to check
"is the annotation type compatible with the expected type?".

There is no Aura-facing primitive yet (`is-subtype?` not exposed).

## Goal of the fix

Replace the stub with **real, sound subtyping rules** for the type
constructors Aura actually uses. Priority-ordered (most important
first):

1. **Identity / dynamic** — already correct; keep
2. **Type variables** — always subtype (defer to substitution)
3. **Function types** — `(A1→A2) <: (B1→B2)` iff `B1 <: A1` (contravariant
   in arg) AND `A2 <: B2` (covariant in return)
4. **Record types** — width subtyping: `Record{a:T, b:U} <: Record{a':T'}`
   iff for every field in sup, sup.field is a structural projection of
   sub.fields, and the matching sub-field type is a subtype of sup's
5. **Variant types** — width subtyping the other way:
   `Variant{A1, A2} <: Variant{B1, B2}` iff every Ai is a subtype of
   some Bj. (Sum types are co-variant in the *opposite* direction from
   product types.)
6. **Linear types** — `Linear T <: Linear U` iff `T <: U`
7. **Module types** — width subtyping on member set, like records
8. **Forall types** — `∀a.T <: ∀b.U` iff there exists a substitution S
   of b→a (or any "more general" type) making `S(T) <: U`
9. **Capability types** — `Cap{effects...} <: Cap{effects'...}` iff the
   cap's effects are a superset of sup's (smaller is a subtype of larger
   in capability systems: a "do less" cap is more restrictive and thus
   a subtype of a "do more" cap)

Out of scope for this fix (noted as future work):
- Mutual recursion through type aliases
- Subtyping with bounded polymorphism (would need `<:` constraints)
- Higher-kinded types (type-of-type)

## Risks

- The current stub returns `false` for "should-be-true" cases. Flipping
  the result to `true` is sound but might surface latent bugs in
  callers. The `consistent_unify` call site already handles "false"
  gracefully (falls through to error reporting), so the immediate
  blast radius is small.
- Width subtyping for records and modules is a known design decision
  (mirrors TypeScript, Flow, Racket) but breaks the invariant
  "two types are equal iff their structure is equal" — when this is
  undesirable, we can add a `type=` for nominal equality.

## Implementation

Replace the stub body in `src/core/type_impl.cpp` with a structural
recursion over `tag_of(sub)` and `tag_of(sup)`. Each case handles
its own subtyping rule, delegating to the recursive call for
sub-components.

Recursion depth is bounded by the type structure depth, which is
finite (entries are leaves, funcs/foralls have bounded arity, etc.).
We add a depth limit (say 64) as a safety net.

```cpp
bool TypeRegistry::is_subtype(TypeId sub, TypeId sup) const {
    return is_subtype_impl(sub, sup, 0);
}

bool TypeRegistry::is_subtype_impl(TypeId sub, TypeId sup, int depth) const {
    if (depth > 64) return false;          // safety net
    if (sub == sup)  return true;          // identity
    if (sup == dynamic_type()) return true; // T <: Any
    if (sub == dynamic_type()) return false; // Any is the top, not a subtype of anything else

    // Type variables: defer (always subtype so the caller's
    // substitution / unification can resolve).
    if (is_var(sub) || is_var(sup)) return true;

    auto sub_tag = tag_of(sub);
    auto sup_tag = tag_of(sup);

    // Different leaf tags (Int vs String) → not subtype.
    if (sub_tag != sup_tag) {
        // Allow a few "trivially equal" cross-tag cases.
        if (sub_tag == TypeTag::INT && sup_tag == TypeTag::FLOAT) return false;
        if (sub_tag == TypeTag::FLOAT && sup_tag == TypeTag::INT) return false;
        return false;
    }

    switch (sub_tag) {
        case TypeTag::FUNC: {
            // (A1→A2) <: (B1→B2) iff B1 <: A1 (contravariant) AND A2 <: B2
            auto* sf = func_of(sub);
            auto* st = func_of(sup);
            if (sf->args.size() != st->args.size()) return false;
            for (size_t i = 0; i < sf->args.size(); ++i) {
                if (!is_subtype_impl(st->args[i], sf->args[i], depth + 1)) return false;
            }
            return is_subtype_impl(sf->ret, st->ret, depth + 1);
        }
        case TypeTag::RECORD: {
            // Width subtyping: every sup field must exist in sub with
            // a subtype type.
            auto* sr = record_of(sub);
            auto* tr = record_of(sup);
            for (auto& [name, sup_type] : tr->fields) {
                TypeId sub_type{};
                for (auto& [n2, t2] : sr->fields) {
                    if (n2 == name) { sub_type = t2; break; }
                }
                if (!sub_type.valid()) return false;
                if (!is_subtype_impl(sub_type, sup_type, depth + 1)) return false;
            }
            return true;
        }
        case TypeTag::VARIANT: {
            // Width subtyping the other way: every sub variant must
            // match some sup variant by name (Aura variants are nominal
            // by constructor name).
            auto* sv = variant_of(sub);
            auto* tv = variant_of(sup);
            for (auto& [name, sub_args] : sv->variants) {
                bool found = false;
                for (auto& [n2, sup_args] : tv->variants) {
                    if (n2 == name) {
                        if (sub_args.size() != sup_args.size()) return false;
                        bool args_ok = true;
                        for (size_t i = 0; i < sub_args.size(); ++i) {
                            if (!is_subtype_impl(sub_args[i], sup_args[i], depth + 1)) {
                                args_ok = false;
                                break;
                            }
                        }
                        if (args_ok) { found = true; break; }
                    }
                }
                if (!found) return false;
            }
            return true;
        }
        case TypeTag::LINEAR: {
            auto* sl = linear_of(sub);
            auto* tl = linear_of(sup);
            return is_subtype_impl(sl->inner, tl->inner, depth + 1);
        }
        case TypeTag::MODULE: {
            auto* sm = module_of(sub);
            auto* tm = module_of(sup);
            for (auto& [name, sup_type] : tm->members) {
                TypeId sub_type{};
                for (auto& [n2, t2] : sm->members) {
                    if (n2 == name) { sub_type = t2; break; }
                }
                if (!sub_type.valid()) return false;
                if (!is_subtype_impl(sub_type, sup_type, depth + 1)) return false;
            }
            return true;
        }
        case TypeTag::FORALL: {
            // ∀a.T <: ∀b.U iff substitute b with a fresh var in U,
            // and check T <: U'. (Skipping for now — see "Out of scope".)
            return false;
        }
        case TypeTag::CAPABILITY: {
            // Cap{e1,e2,...} <: Cap{e1',e2',...} iff sup.effects is a
            // subset of sub.effects (a less-permissive cap is a subtype
            // of a more-permissive one).
            auto* sc = capability_of(sub);
            auto* tc = capability_of(sup);
            for (auto& e : tc->effects) {
                bool found = false;
                for (auto& e2 : sc->effects) {
                    if (e == e2) { found = true; break; }
                }
                if (!found) return false;
            }
            return true;
        }
        case TypeTag::EFFECT: {
            // Effect types are nominal: same name = same effect.
            return name_of(sub) == name_of(sup);
        }
        // Leaf types (INT, BOOL, STRING, VOID, TYPE, VECTOR, FLOAT,
        // PAIR, HASH, DYNAMIC): already handled by identity above.
        default:
            return false;
    }
}
```

## Tests (`tests/suite/subtyping.aura`)

1. Reflexivity: every leaf type `T <: T` returns true
2. Dynamic: every type `T <: Any` returns true
3. Function subtyping:
   - `(Int → Int) <: (Int → Int)` (reflexive)
   - `(Int → Int) <: (Any → Int)` (contravariant — accept wider arg)
   - `(Int → Int) <: (Int → Any)` (covariant return)
   - `(Int → Int) <: (String → Int)` returns false (Int not subtype of String)
   - different arities → not subtype
4. Record subtyping (width):
   - `Record{a:Int, b:Int} <: Record{a:Int}` (sub has extra `b`)
   - `Record{a:Int} <: Record{a:Int, b:Int}` returns false (missing field)
   - `Record{a:Any} <: Record{a:Int}` (Any is a supertype)
5. Variant subtyping (width the other way):
   - `Variant{A,B} <: Variant{A,B,C}` (sub has fewer variants)
   - `Variant{A,B} <: Variant{A}` returns false (missing variant B)
6. Function not subtype of record (different tag)
7. Linear: `Linear Int <: Linear Int` (reflexive)
8. Module: `Module{a:Int, b:Int} <: Module{a:Int}`
9. Capability: `Cap{file,net} <: Cap{file}` (sup is smaller)

## Acceptance criteria

- All above tests pass in CI
- No existing test (run-tests.sh, leak regression, suite) regresses
- The single internal call site (`type_checker_impl.cpp:2145`) behaves
  correctly — `(: x Int 42)` still works

## Out of scope (filed as follow-up issues if relevant)

- Forall subtyping (#70 case 8): the binder-substitution trick
  requires careful alpha-renaming; defer.
- Nominal type equality `type=` for opt-out of structural subtyping.
- Bounded polymorphism `<: T` constraints.
- Higher-kinded types (TYPE constructor).
