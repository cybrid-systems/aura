# Issue #110 — Deepen QueryEngine × Mutate integration (deep investigation)

## Status: ✅ SHIPPED — `d25f066` on main, #110 closed

This doc was an early-investigation writeup that diagnosed
the crash as a pre-existing IR pipeline reference-invalidation
bug. **That diagnosis was wrong.** The actual root cause was a
bug in the qar primitive itself (an infinite-loop-on-growing-flat
pattern). The real fix and the actual root cause are described
in `d25f066`'s commit message. This doc is kept for historical
context; **do not follow its recommendations** — see the
"Corrected diagnosis" section below.

## The real root cause (corrected)

`mutate:query-and-replace` (qar) ran a predicate-matching loop
that re-evaluated `flat.size()` on every iteration:

```cpp
for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
    // predicate matching only (no set_child here)
}
```

This loop itself doesn't grow the flat. But the `parse_to_flat`
call that follows the predicate parsing (via the `(query:where
:callee "...")` argument evaluation) appends new strings to a
`std::pmr::vector<string>` — which can trigger reallocation of
the vector's internal data. The reallocation mutates
`flat.size()` between iterations, so the loop condition
re-evaluates to `true` for newly-appended indices — and the
process runs forever (or until OOM).

**Fix**: snapshot the size before the loop:
```cpp
auto end_id = flat.size();
for (aura::ast::NodeId id = 0; id < end_id; ++id) { ... }
```

This guarantees the loop terminates after the original nodes
have been visited, even if the flat grows.

## Why my earlier diagnosis was wrong

I attributed the crash to:
- `unordered_map<string, CompilerService>` rehash (wrong — `unique_ptr` fix didn't help)
- IR pipeline reference invalidation (wrong — `by-value` Primitives moved the crash to a different place)
- `&primitives_ = 0xfffffb9cec20` (kernel area, garbage) (real but a SYMPTOM, not the cause)

The `0xfffffb9cec20` observation was real but explained by a
different mechanism: the qar's predicate evaluation triggered
the pmr::vector reallocation, which left dangling internal
pointers in some downstream consumers. The crash surface
shifted (sometimes `primitives_.string_heap()`, sometimes
`TypeId::operator[]`) depending on the exact timing of the
reallocation.

## What was actually learned

1. **Self-modifying collection iteration** is a recurring
   footgun in Aura's flat AST code. Any loop that calls into
   code that might grow the flat (parse_to_flat,
   set_child, add_mutation) needs to snapshot the size first.
   A search for `for (...) flat.size()` in the evaluator would
   likely turn up more instances of this pattern.

2. **The IR pipeline's reference-management** (sessions map,
   Primitives& in IRInterpreter, last_ir_mod_ handling) IS
   fragile — I just didn't trigger it in #110. The right
   architectural fix is the `IRContext` approach discussed
   elsewhere. This is a separate issue from #110.

## What's still open

- **IRContext refactor**: `Primitives&` → `const IRContext&`
  pattern. Independent of #110, separate scope.
- **General audit of self-modifying-flat loops**: search the
  evaluator for `for (...) flat.size()` patterns and add
  size snapshots. Should be a small, mechanical pass.

## Verification (final state)

- `d25f066` ships `mutate:query-and-replace` working correctly
- All `./build/test_ir` suites pass
- Fuzzers: 200/299/405
- ASAN test_ir: exit=0, 0 leaks
- ASAN stress: 30 iter × 2 qar calls = 60 ops, 0 leaks
- Issue #110 closed in GitHub with `state_reason: "completed"`
