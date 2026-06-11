# Design: Issue #71 — `is_poly` field unused, let-poly auto-instantiation

## Current state

`src/compiler/type_checker.ixx`:

```cpp
struct Binding {
    aura::core::TypeId type;
    bool is_poly = false;       // ← set in the struct but never set to true
    std::vector<aura::core::TypeId> type_args;
};
```

`src/compiler/type_checker_impl.cpp`:

```cpp
void TypeEnv::bind(std::string name, TypeId type) {
    scopes_.back()[std::move(name)] = Binding{type, false, {}};  // hardcoded false
}

TypeId TypeEnv::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end())
            return f->second.type;  // ← returns poly as-is
    }
    return TypeId{};
}
```

`is_poly` is declared in the struct, default-initialized to `false`,
and never assigned `true` anywhere in the codebase. The `lookup`
returns the bound type as-is, even if it's a polymorphic `∀a. T`
wrapper.

## The "bug"

`is_poly` is dead code — declared but never set. The intent
(per the issue) was:

> Wire is_poly into TypeEnv lookup + ConstraintSystem
> instantiation path.

The current code gets let-polymorphism "accidentally" working in
the call path because `synthesize_flat_call` manually
instantiates the callee:

```cpp
// synthesize_flat_call in src/compiler/type_checker_impl.cpp
auto instantiate_all_direct = [&](this const auto& self, TypeId tid) -> TypeId {
    auto* ft = reg_.forall_of(tid);
    if (!ft) return tid;
    auto inst = reg_.instantiate(tid, [this]() { return cs_.fresh_var(); });
    return self(inst);
};
func_type = instantiate_all_direct(func_type);
```

So `(id 42)` and `(id "hi")` each get fresh `a` vars and type-check
correctly. But this only works in the Call path. If a poly value
is used in a non-call context (e.g. passed as an argument, stored
in a list, used in a typed binding), the poly leaks.

Worse, the call-path manual instantiation is a workaround for
`lookup` not instantiating. It's brittle: any new call site that
forgets to instantiate silently leaks the bound vars.

The dead `is_poly` field is the original HM-style design intent
that was never wired up.

## The fix

Two parts:

### Part 1: Make `bind` auto-detect poly

```cpp
void TypeEnv::bind(std::string name, TypeId type) {
    bool is_poly = reg_.forall_of(type) != nullptr;
    scopes_.back()[std::move(name)] = Binding{type, is_poly, {}};
}
```

This way `is_poly` is automatically set based on whether the type
is a Forall wrapper. No caller needs to remember to pass it.

### Part 2: Make `lookup` auto-instantiate poly

```cpp
TypeId TypeEnv::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) {
            // Let-polymorphism: if the bound type is a Forall,
            // instantiate with fresh vars so each use gets its own
            // copy. (Otherwise the bound vars leak across use sites.)
            if (f->second.is_poly) {
                return reg_.instantiate_forall(
                    f->second.type,
                    {}  // no args — fully instantiate with fresh vars (see #76)
                );
            }
            return f->second.type;
        }
    }
    return TypeId{};
}
```

Now ANY use of a poly binding gets a fresh instantiation:
- Call: instantiates (was already manually doing this, now lookup does it)
- Variable reference: instantiates (was leaking before)
- Argument: instantiates
- Return: instantiates

### Interaction with the Call path

`lookup` now returns a fully-instantiated type. The Call path's
`instantiate_all_direct` becomes a no-op (no Forall left to
peel). It can stay as defense-in-depth, or be removed. I'll
**keep it** for now — the cost is one `forall_of` check per call,
which is cheap, and it makes the call path self-documenting
("calls always get a fresh instantiation, no matter what the
env did").

## Why not other approaches

- **Status quo (call path only)**: works in practice but brittle
  and leaves the `is_poly` field as a lie.
- **Wire `is_poly` only, don't auto-instantiate in lookup**:
  the field would still be dead because lookup doesn't use it.
- **Eagerly generalize at let-binding, never instantiate**:
  that's monomorphism, not let-polymorphism.
- **Use De Bruijn indices or a typed map**: too large a refactor
  for this issue's scope.

## Test plan

Add tests to `tests/test_ir.cpp` section 19 (or a new let-poly
section) that exercise:

1. **Same poly at multiple call sites with different types**
   (regression for the dead-`is_poly` regression)
2. **Poly used as a non-call value** (the actual latent bug —
   lookup not auto-instantiating)
3. **Poly in nested scopes** (generalization across scopes)

We can also extend `tests/suite/subtyping.aura` (or add
`let_polymorphism.aura`) for the Aura-side coverage.

## Implementation plan

1. Update `TypeEnv::bind` and `TypeEnv::lookup` in
   `src/compiler/type_checker_impl.cpp` (2 functions, ~10 lines)
2. Add 2-3 new test cases in `tests/test_ir.cpp`
3. Run full regression to ensure no behavior change (the call
   path's manual instantiate makes it idempotent)

## Acceptance criteria

- `is_poly` is set automatically based on the bound type
- `lookup` returns a fresh instantiation for any Forall binding
- Existing let-poly test `let(poly-use) → String` still passes
- All 30/30 suite, 5/5 leak, 201/201 run-tests, 65/65 TS tests
  still pass
- New tests for non-call poly usage pass

## Out of scope

- True HM let-polymorphism with full rank-1 / rank-N inference
  (the current code only generalizes values with free vars;
  rank-N would need type schemes with arbitrary nesting)
- Bounded polymorphism `<: T` constraints
- Type-class-style overloading
