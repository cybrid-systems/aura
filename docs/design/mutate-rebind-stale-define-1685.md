# mutate:rebind re-resolve Define after parse_to_flat (#1685)

**Issue:** [#1685](https://github.com/cybrid-systems/aura/issues/1685)  
**Files:** `src/compiler/evaluator_primitives_mutate.cpp`  
**Status:** P1 correctness — target NodeId must not be applied blindly after append.

## Problem

`mutate:rebind` captured `old_define` (NodeId) then called `parse_to_flat` into
the same `FlatAST`. The issue report framed SoA vector reallocation as making
the index “stale.” In this codebase a NodeId is an integer SoA column index;
indices remain valid across `std::vector` growth. Real risks after append are:

1. Applying `set_child` without re-checking the slot is still a live Define
2. Full `(define name …)` parse roots share the same `sym_id` — a naïve
   post-parse first-match can prefer the **new** free-list-recycled Define if
   it lands at a lower index
3. Sibling mutates (`set-body`, `insert-child`, `replace-subtree`) capture
   parent / define ids across the same append window

## Fix (Option A + preferred-id)

```cpp
NodeId resolve_define_after_parse(flat, sym, preferred, size_before_parse);
```

- Prefer `preferred` when it is still a live `Define` with `sym` in
  `[0, size_before_parse)`
- Else linear scan that range only (ignore defines appended by this parse)
- Wire on `mutate:rebind` and `mutate:set-body`
- `insert-child` / `replace-subtree`: re-validate parent (and child slot) after parse

## Tests

`tests/test_mutate_rebind_stale_define_1685.cpp`
