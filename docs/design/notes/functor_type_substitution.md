# Functor Type Parameter Substitution in Type Checker (Issue #29)

**Status**: Design
**Design Author**: Ani

## Problem

When a functor is instantiated (e.g., `(Stack Int)`), the evaluator handles substitution at runtime by re-evaluating the template body in a new environment. However, the **type checker's** `synthesize_flat_call` path (used by `typecheck-current`) returns the ModuleType without substituting type parameters:

```cpp
// type_checker_impl.cpp:1620-1624
if (auto* mt = reg_.module_of(func_type)) {
    // For functor instantiation... 
    // Type parameter substitution (T → Int) would go here
    return func_type;  // returns template-level ModuleType with type vars
}
```

Result: `(Stack Int)` is typed as `Module{push: (__t0 __t1 -> Any)}` instead of `Module{push: (List Int) Int -> (List Int)}`.

## Scope

This fix addresses **the type checker path only** — the evaluator/runtime path already works correctly. After the fix, `typecheck-current` will report precise types for functor instances.

## Design

### Type parameter tracking in `define-module`

When `synthesize_flat` encounters `define-module` (type_checker_impl.cpp:1363), the type params from the form `(define-module (Stack T) ...)` are already available as `v.params` (the SymId span in the Lambda/Module node — actually stored differently for module forms).

The issue: the type checker's `define-module` handler at line 1363 processes the body and builds a `ModuleType`, but it doesn't track the type parameter names from the form header.

**Fix**: When processing `define-module`:
1. Extract type parameter names from the define-module's param list
2. Store them in a new field on ModuleType: `std::vector<std::string> type_params`
3. When a functor is called, substitute the formal type params for actual types

### Functor call substitution

In `synthesize_flat_call` at line 1620, when the callee type is a `ModuleType`:
1. Check if `ModuleType::type_params` is non-empty (it's a functor template)
2. Match actual arguments 1..N against formal type params
3. Create a new ModuleType with substituted member types
4. Return the substituted ModuleType

### Implementation

#### Phase 1: Track type params in ModuleType

In `aura/core/type.ixx`:
```cpp
struct ModuleType {
    std::vector<std::pair<std::string, TypeId>> members;
    std::vector<std::string> type_params;  // NEW: e.g. ["T"] for (define-module (Stack T) ...)
};
```

In `type_checker_impl.cpp`, `define-module` handler: populate `type_params` from the `define-module` node's param list.

#### Phase 2: Substitute in synthesize_flat_call

In `type_checker_impl.cpp` around line 1620:

```cpp
if (auto* mt = reg_.module_of(func_type)) {
    if (!mt->type_params.empty()) {
        // Functor instantiation: substitute type params
        // args start at v.child(1) (child(0) is the functor name)
        std::unordered_map<std::string, TypeId> subst;
        for (size_t i = 0; i < mt->type_params.size() && (i + 1) < v.children.size(); i++) {
            auto arg_id = v.child(i + 1);
            auto arg_type = synthesize_flat(flat, pool, arg_id, flat.get(arg_id));
            subst[mt->type_params[i]] = arg_type;
        }
        // Build substituted ModuleType
        auto result = reg_.register_module(substitute_module(mt, subst));
        return result;
    }
    return func_type;
}
```

#### Phase 3: Substitute function

Replaces TypeId references in a ModuleType's member types:

```cpp
TypeId substitute_type(TypeId ty, const std::unordered_map<std::string, TypeId>& subst) {
    if (auto* ft = reg_.func_of(ty)) {
        // Substitute function params and return type
        ...
    }
    return ty;  // literal types (Int, String, etc.) unchanged
}
```

## Testing

- `typecheck-current` returns `Module{push: (List Int) Int -> (List Int)}` for `(Stack Int)` instead of `Module{push: (__t0 __t1 -> Any)}`
- Functors with multiple type params
- Nested functors
- Existing functor tests still pass (no regression)

## References

- Issue #29
- `docs/design/functor_modules.md`
- `docs/design/aura_typesystem.md` §5.4
- `src/compiler/type_checker_impl.cpp` line 1620
- `src/core/type.ixx` (ModuleType struct)
