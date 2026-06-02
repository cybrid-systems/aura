# Design: TypeId interning (Issue #70 follow-up)

## Why

`TypeRegistry::register_*` family of methods (register_func, register_variant,
register_record, register_module, register_linear, register_forall, etc.)
unconditionally appends a new `Entry` to `entries_` and returns a fresh
`TypeId`. Two structurally-identical types — e.g. two calls to
`register_func({Int}, Int)` — get **two different TypeIds** even though they
describe the same type.

This breaks the contract that "two TypeIds are equal iff they refer to the
same type": `is_subtype` (just implemented in #70) returns true for
reflexivity only when `sub == sup` as `TypeId` values, so two structurally
equal types fail the equality test. `consistent_unify` has the same problem.

We saw this directly in the #70 test: the `: f (Int -> Int) (double 5)`
annotation returned 5 instead of 10 — the type-checker couldn't recognize
that `(Int -> Int)` and `Int -> Int` (the type of `double 5`) are the same
structural type, so the type-check fell back to the raw value.

The fix: when a `register_*` method is called with a structural input
that already has an entry, return the existing `TypeId` instead of
creating a new one. This is **TypeId interning** by structural
equivalence.

## Goal

After the fix:

1. Two calls to `register_func({Int}, Int)` return the same `TypeId`.
2. `consistent_unify` and `is_subtype` work correctly on structurally-equal
   types regardless of which call site they came from.
3. The existing test `(: f (Int -> Int) (double 5))` returns 10
   (the actual double of 5), not 5.
4. No regression in existing tests (run-tests.sh, leak, suite).

## Design

### 1. Equality function

Add a private member function that compares two `TypeId`s by structural
content, not by raw id:

```cpp
bool type_equals(TypeId a, TypeId b) const;
```

Implementation: switch on `tag_of(a)` and `tag_of(b)`, recurse for
compound types. Same-tag rules:

- Leaves (INT, BOOL, STRING, VOID, TYPE, VECTOR, FLOAT, PAIR, HASH):
  always equal (only one entry per tag in the predefined 9).
- TYPE_VAR: equal iff same index (vars are placeholders; identity matters).
- FUNC: same arity, same args (recursive `type_equals`), same ret.
- LINEAR: inner equal.
- VARIANT: same number of variants, same names in order, same args per
  variant.
- RECORD: same number of fields, same names in order, same types per
  field.
- MODULE: same number of members, same names in order, same types.
- FORALL: same var index, same body.
- CAPABILITY: same effects (as a set, order-independent), same
  unrestricted flag.
- EFFECT: same name, same arg.

Different-tag: never equal (except dynamic = itself).

### 2. Hash function

A companion `type_hash(TypeId)` for use as the unordered_map's hash:

```cpp
std::uint64_t type_hash(TypeId a) const;
```

Hashes combine FNV-1a over (tag, components). For sets (capability
effects), sort first to make hash order-independent.

### 3. Interning table

```cpp
struct InternedType {
    std::unordered_map<TypeId, std::pair<TypeId, std::uint64_t>, TypeHash, TypeEq> table_;
};
// Or a simpler approach: keyed by uint64_t (hash) with linear scan
// for collisions.
```

A single `unordered_map<TypeId, TypeId>` would work as the value
type, but we need `type_hash` and `type_equals` as the hash/equal
template parameters. Since `TypeId` is just a pair (index, generation),
we'd need a wrapper. Simpler: store keyed by `std::uint64_t` (the hash)
with linear scan for collisions, since we expect very few collisions
given a good hash and small N.

Actually the cleanest: make the `intern_` map keyed by something like
`std::pair<TypeTag, std::vector<TypeId>>` with custom hash/equal. But
that requires per-tag vector-of-component-ids handling. Let me keep
it simple: **a single `unordered_map<uint64_t, std::vector<TypeId>>`
where the key is the type_hash and the value is a list of candidate
TypeIds (very short, usually length 1) that match the hash. Lookup
scans the vector with `type_equals` for verification. This handles
hash collisions correctly and is O(1) amortized.

Actually, even simpler given the small N: **linear scan through all
entries_ on each register_* call**. The typical N is hundreds
(see tests/suite/type_registry_compact.aura: 367 entries after
loading stdlib + test). Linear scan is O(N) per register. N is
small; register_* isn't called in a hot loop. This is the simplest
correct implementation and avoids the complexity of hash-based
interning for an observability layer. We can swap to a hash later
if profiling shows it's a bottleneck.

**Decision: start with linear scan.** If profiling later shows it's
hot, upgrade to `unordered_map<uint64_t, vector<TypeId>>` with
hash-based bucketing.

### 4. Per-method dedup

Each `register_*` method gets a "check existing first" prelude.
For example:

```cpp
TypeId TypeRegistry::register_func(std::vector<TypeId> args, TypeId ret) {
    // Dedup: if a func type with these args/ret already exists, return it.
    for (std::uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tag != TypeTag::FUNC) continue;
        if (!entries_[i].func) continue;
        const auto& f = *entries_[i].func;
        if (f.args.size() != args.size()) continue;
        bool match = (f.ret == ret) || type_equals(f.ret, ret);
        if (!match) continue;
        bool args_match = true;
        for (std::size_t j = 0; j < args.size(); ++j) {
            if (f.args[j] != args[j] && !type_equals(f.args[j], args[j])) {
                args_match = false;
                break;
            }
        }
        if (args_match) {
            return TypeId{static_cast<std::uint32_t>(i), next_generation_};
        }
    }
    // No match: register as before.
    const auto id = TypeId{
        .index = static_cast<std::uint32_t>(entries_.size()),
        .generation = next_generation_,
    };
    // ... rest of existing logic ...
    return id;
}
```

Wait — there's a subtlety. The returned TypeId carries the CURRENT
generation, not the generation when the entry was originally registered.
But that's the same logic as before. Actually, hmm — if we intern and
return an existing entry, we want callers to see the same TypeId
they'd see if they had called register earlier. The id should
reflect the entry's stable identity.

Let me re-think. Currently:
- Entry at index `i` was registered at some past generation G_old.
- Current generation is G_new.
- We return `TypeId{i, G_new}` — that "type id" represents the
  current-generation view of an entry that has lived across
  generations.

But the caller may store this TypeId and use it later. As long as
G_new is current, the TypeId is "valid". And if a compact() bumps
the generation, the TypeId becomes stale (which is the intended
semantics).

Interning changes the surface: instead of "each call returns a
new TypeId with current generation", we return "any call with the
same structural input returns the same TypeId (with current
generation)".

This is a behavior change. Is it safe?

- `is_subtype(a, b)`: now returns true if structurally equal. **More
  correct.**
- `consistent_unify`: same. **More correct.**
- Anything holding a TypeId from before the fix: still works because
  the entries are still at the same index.

I think the behavior change is strictly more correct. Let me
document it as a "fix" not a "change".

### 5. Special cases

**`register_type(tag, name)`**: dedup by (tag, name). The 9
predefined types each have a unique name, so no dedup needed. For
user-registered types, dedup is straightforward.

**`register_func_named(args, ret, name)`**: dedup by (args, ret).
If a match exists, OVERWRITE the existing entry's name with the
new name. (Otherwise the user's call to `register_func_named` would
silently ignore their chosen name.)

**`register_forall(var, body)`**: dedup by (var, body). Var is by
index, so two forall types with the same var-index and same body
collapse to the same TypeId. **Caveat**: if the user binds the same
var to a different body later, the second call would dedup to the
first. This is actually the *correct* behavior — types should be
identified structurally.

**`make_var(name)`**: NO dedup. Each `make_var` returns a fresh
type variable. Vars are placeholders that are unified by `consistent_unify`.

### 6. `compact()` interaction

`compact()` re-registers the 9 predefined types. After compact, the
newly-registered types have generation = G_new. If a user re-registers
the same struct after compact, the dedup logic must look in the
new-generation entries only (entries_ is now mostly empty + 9
predefined). The dedup loop iterates `entries_` which is the
post-compact list. This works correctly because the post-compact
generation matches the current `next_generation_`, and the
re-registered types in the post-compact state are the canonical
interned versions.

A subtle issue: what if a user holds a TypeId from generation G_old
that referred to a user-registered type? After compact, that
TypeId is stale (entry was reaped, generation bumped). The user
gets a "stale TypeId" error. This is the existing behavior — dedup
doesn't change it.

### 7. Performance budget

Linear scan of `entries_` per register. For N=1000 entries and
arity=3 func, each register is O(N*arity) = O(3000) operations.
Per test (5-10 calls per primitive), that's 30K ops — negligible
compared to the 12ms test runtime.

For long-running sessions (N=10K, arity=5), 50K ops per register.
A type checker pass might do 100K register calls = 5G ops. **Could be
slow.** Mitigation: switch to hash-based interning if this becomes
hot. Defer until measured.

## Implementation plan

1. Add `type_equals` and `type_hash` to `TypeRegistry` (private
   methods, both in `type.ixx` declarations and `type_impl.cpp`).
2. Modify each `register_*` method to check for an existing match
   first. Return existing TypeId on match, append new entry on
   miss.
3. Update `compact()` to clear the dedup state (no-op since dedup
   state is implicit in `entries_`).
4. Add `tests/suite/type_id_interning.aura` covering:
   - `register_func({Int}, Int)` called twice → same TypeId
   - `register_record({a:Int, b:Int})` and `register_record({a:Int, b:Int})` → same TypeId
   - `register_func({Int}, Int)` and `register_func({String}, Int)` → different TypeIds
   - Type-checker behavior: `(: f (Int -> Int) (double 5))` returns 10 (not 5)
5. Run all existing tests for regressions.

## What's NOT in this fix (out of scope)

- Hash-based dedup (only switch if linear scan becomes hot)
- Type variable interning (vars are placeholders; always fresh)
- Forall alpha-renaming for proper subtyping (separate #70 follow-up)
- Bounded polymorphism constraints

## Acceptance criteria

- All existing tests pass (no regression in 30+ suite tests, 285 run-tests)
- New `tests/suite/type_id_interning.aura` covers structural equality
- The `(: f (Int -> Int) (double 5))` test that returned 5 now returns 10
- The test `register_func({Int}, Int) == register_func({Int}, Int)` returns true
- No new memory leaks (verified via `tests/load_module_leak.py` etc.)
