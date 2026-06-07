# Issue #110 — Deepen QueryEngine × Mutate integration (deep investigation)

## Status: ⏸ DEFERRED AGAIN — deeper bug than originally diagnosed

This is an update to the previous follow-up doc
(`issue-110-followup.md`). The original diagnosis attributed the
crash to a `std::unordered_map<string, CompilerService>` rehash
issue. After further investigation, that diagnosis was incorrect.

## What was tried in this round

After the first round of investigation, I:
1. Changed `sessions` map to use `std::unique_ptr<CompilerService>` (heap-stable)
   - Result: SAME crash
2. Changed `IRInterpreter::primitives_` from `Primitives&` to `Primitives` (by value)
   - Result: crash moved to a different place (TypeId vector in TypeRegistry)
3. Added DEBUG prints for `&primitives_` and `&last_ir_mod_.string_pool`
   - Result: confirmed address change between calls
4. Tried minimal qar stubs with various subsets of the full qar logic
   - Result: simplified stubs work; only the full qar crashes

## Refined diagnosis

The CRITICAL insight from lldb: the second call's `&primitives_` is
at address `0xfffffb9cec20` (kernel area, garbage), while the first
is at `0xaaaaf4d37340` (valid heap). This is impossible if
`primitives_` is a stable reference — references can't be reassigned.

This means: **a different `Primitives` object is being used for the
second call**. Either:
- The CompilerService is being moved (despite unique_ptr)
- OR a different Evaluator instance is being used

## What the test reproduces (verified)

Test: `(set-code "(+ 1 2)") (display "hello") (mutate:query-replace ...)`
- `set-code` works
- `display "hello"` works (ConstString case runs fine)
- `mutate:query-replace` crashes (same ConstString case in IR crashes)

The first ConstString succeeds. The third one crashes. Between them,
only `display "hello"` ran (which itself uses ConstString successfully).

So the bug is NOT in ConstString per se. It's in something the
third call triggers. The third call uses IR. The IR's `module_`
field is a `const IRModule&` (also a reference). If `last_ir_mod_`
is moved/reassigned between the second and third calls, the
module_ reference dangles.

## What we tried that doesn't help

1. **unique_ptr<CompilerService> in sessions map**:
   - CS is heap-allocated, addresses are stable
   - Crash still happens (suggests CS isn't the issue)
2. **by-value `Primitives primitives_;` in IRInterpreter**:
   - Crash moves to TypeId vector in TypeRegistry
   - The TypeRegistry is also a reference (`const TypeRegistry*`)
   - Both references are bound from the same CS, but their
     pointee types are different
   - Suggests the underlying Evaluator (or the CS) is moving

## What we need to investigate next

This bug is **architectural** in the IR pipeline. The session/
map/Evaluator/Interpreter chain has a reference-invalidation
pattern that's fragile. To diagnose properly:

1. Add a DEBUG print of `&evaluator_` (the address of the CS's
   evaluator) before each IR interpreter creation
2. Check if it changes between calls
3. If yes, the CS is being recreated — find where
4. If no, look at the closure_bridge state for dangling references

The qar primitive (~350 LOC) is correct and works in isolation.
The bug is in the IR pipeline's session management, which is
exposed by my qar's mutate behavior.

## What I tried that ALMOST worked

- A simplified qar that uses parse_to_flat + cache invalidation
  + a `for` loop over the flat (without predicate matching or
  set_child) **does NOT crash**.
- The full qar (with predicate matching, node_to_source, set_child)
  **crashes**.

So one of: predicate matching, node_to_source, set_child,
or add_mutation introduces the bug.

The DEBUG shows that when the simplified qar runs:
- `set-code "(+ 1 2)"` → builds flat with Call at id 3
- `display "hello"` → IR runs ConstString successfully
- `mutate:query-and-replace (predicate) "y"` → IR pre-compiles
  for this form. During run_function, when the IR tries to
  run a primitive call (the qar), the run crashes

The crash is in `primitives_.string_heap().size()`. The `primitives_`
field has a valid reference to a valid Primitives object. The
string_heap_ is set. The vector should be valid.

Unless... the IR's `module_.string_pool` is corrupted. The lowering
uses `state.module.string_pool.push_back(s)`. If the module is
moved/copied after the lowering, the string_pool could be
in a different address.

But the IR is stored in `last_ir_mod_` (a member of CS). If the
CS is stable, the IR module is stable. The string_pool is a
member of the IR module. Stable.

I think the underlying issue is **memory corruption** in the
qar's body, possibly in `add_mutation` or in the recursive
`node_to_source` lambda (which has unbounded recursion on
the flat).

## Recommended next steps

1. Add a DEBUG print in `add_mutation` to verify it's not
   corrupting state
2. Add a recursion-depth limit to `node_to_source`
3. Try qar without `add_mutation` to see if it's the culprit
4. If all else fails, the bug is in a downstream component
   that the qar is the first to expose

## Working state

All debug code reverted. Working tree clean. The qar primitive
itself is in a saved state (not committed). When the IR pipeline
bug is fixed, the qar can be re-applied.

The original follow-up doc (`issue-110-followup.md`) is still
accurate as a high-level summary.
