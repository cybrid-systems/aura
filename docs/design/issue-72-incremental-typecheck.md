# Design: Issue #72 — Incremental type checking cache ignores contained TYPE_VARs

## Current state

`src/compiler/type_checker_impl.cpp:1114`:

```cpp
TypeId InferenceEngine::synthesize_flat(FlatAST& flat, StringPool& pool,
                                         NodeId id, NodeView v) {
    cur_loc_ = {v.line, v.col, 0};

    // Incremental: if node is clean AND has a resolved cached type, return cached result.
    // ...
    if (!flat.is_dirty(id)) {
        auto cached = flat.type_id(id);
        if (cached > 0 && cached < reg_.size()) {
            auto tid = TypeId{cached, 1};
            // Type vars cached before constraint solving are stale;
            // only accept concrete resolved types for incremental reuse.
            if (reg_.tag_of(tid) != TypeTag::TYPE_VAR) {
                return tid;
            }
        }
        // Clean but not cached / stale cache: fall through to recompute
    }
    // ... synthesize ...
}
```

`src/compiler/type_checker_impl.cpp:1526` (end of synthesize_flat):

```cpp
// Cache result for future incremental calls.
// Store the type index even if it's a fresh var — after constraint solving
// in infer_flat, the root's cache will be updated with the normalized type.
// The cache read path skips TYPE_VAR entries, so stale vars cause a
// re-compute which then stores the resolved type.
flat.set_type(id, result.index);
flat.clear_dirty(id);
return result;
```

## The bug

The cache stores PRE-SOLVE types (set in `synthesize_flat` at the
end, before `cs_.solve()` is called in `infer_flat`). Many of
these contain free `TYPE_VAR`s — e.g. a polymorphic let-binding
synthesized as `a → Int` where `a` is a fresh constraint variable.

The read-path check `reg_.tag_of(tid) != TypeTag::TYPE_VAR` only
inspects the **top-level** tag. A `FUNC` type whose body
**contains** a `TYPE_VAR` passes the check and is returned as a
"cached" result. But the contained `TYPE_VAR` is from a previous
constraint solve — the union-find has been cleared, so the var
no longer points anywhere valid. The next unification against
this stale type gives wrong results.

The author knew about the staleness (the comment says "Type vars
cached before constraint solving are stale") but the fix is
incomplete: it doesn't walk into compound types to check for
contained stale vars.

## Why this matters in practice

For ML-style polymorphic inference, MANY synthesized types
contain free vars before solving. After the let-poly fix (#71)
and the substitute fix (#77), every synthesize_flat call creates
fresh `cs_.fresh_var()` instances for polymorphic bindings. The
pre-solve type almost always contains them. So the incremental
cache check **almost never hits** in practice — every node is
re-synthesized, even when nothing changed.

This is the bug Issue #72 is describing:
> dirty_ propagation exists in FlatAST but TypeChecker ignores it
> completely. Every mutate triggers full re-typecheck.

The propagation works; the cache check is just too restrictive to
ever fire.

## The fix

Strengthen the cache check to also reject types that contain
`TYPE_VAR`s (not just types whose top-level tag is `TYPE_VAR`).
The simplest way: use `reg_.free_vars(tid)` — if the cached type
has any free vars, the cache is stale and we must recompute.

```cpp
if (!flat.is_dirty(id)) {
    auto cached = flat.type_id(id);
    if (cached > 0 && cached < reg_.size()) {
        auto tid = TypeId{cached, 1};
        // Reject if the cached type has any free vars (stale
        // pre-solve type variables that the previous constraint
        // solve would have resolved). The top-level check above
        // was too lax — a FUNC/LINEAR/RECORD/etc. that contains
        // a TYPE_VAR is also stale.
        if (reg_.free_vars(tid).empty()) {
            return tid;
        }
    }
    // Clean but not cached / stale cache: fall through to recompute
}
```

### Why this is correct

After `infer_flat` calls `cs_.solve()` and normalizes, all free
vars in the result are resolved. The post-solve type has no free
vars. So `reg_.free_vars(tid).empty()` is true for any
post-solve cached type.

Pre-solve cached types (the bug) always have free vars in
polymorphic contexts. So `free_vars(tid).empty()` is false for
those, and we correctly fall through to recompute.

### Cost

`reg_.free_vars(tid)` walks the type. For a fully resolved type
(e.g. `Int -> String`), the walk is fast — it returns empty
quickly. For a poly type, the walk recurses through the type
structure. In practice, both are O(type_size) which is bounded
by the AST node's local type structure (small).

The alternative — `forall_of`, `func_of`, etc. checks on every
compound type tag — would be O(type_size * tag_dispatches),
slower.

### Why not just resolve stale vars

A more ambitious fix would be: when the cached type contains
stale vars, rebind them to fresh vars in the current constraint
system, then return. This preserves the cached structure and
only fixes the staleness.

But this introduces new problems:
- The cached "shape" might not be the right shape for the new
  context. E.g. the previous solve might have unified `a` with
  `Int` (resolved via union-find), but the cached type stored
  the pre-solve `a` itself. Rebinding `a` to a fresh var loses
  the `Int` binding.
- The semantic equivalence between "the old var was Int" and
  "the new var is unconstrained" is broken.

Safer to recompute. The cost is small (each clean node is
re-synthesized, but no constraint solve is needed for them — the
synthesize_flat call is cheap).

## Test plan

Add tests to `tests/test_ir.cpp` (new "DP" section or extend the
existing one) that verify:

1. **Clean node with fully-resolved cached type returns the
   cache** (the new behavior). Construct a deep tree, typecheck
   twice (with no mutation in between), verify the second
   typecheck is consistent. Add a counter for how many
   synthesize_flat calls happened and verify the second
   typecheck does fewer.
2. **Stale cached type with contained TYPE_VAR is rejected**
   (regression for the bug). Construct a tree that creates a
   pre-solve cached type with a contained TYPE_VAR (e.g. a
   let-bound poly used in a Call). Manually call
   `flat.set_type(id, <stale_type>)` on a clean sub-node. Verify
   the typecheck recomputes (does not return the stale cache).
3. **A real mutation only re-infers the dirty subtree** (the
   end-user benefit). Build a deep tree, mutate a leaf, typecheck
   again, verify the result is correct and (optionally) that a
   counter for synthesize_flat calls is small.

The test #1 is the most important: it verifies the cache
actually fires for clean nodes with fully-resolved types. The
existing "DP OK: typecheck after mark_dirty_upward returns
consistent type" test verifies correctness but not speedup.

For test #1, I need a counter. The simplest way: count
synthesize_flat calls in the test (via a wrapper or by adding
a debug counter to the engine). Let me add a `stats_` field to
InferenceEngine that counts synthesize_flat invocations and
cache hits. The test can check `stats_.cache_hits > 0` after
the second typecheck.

## Implementation plan

1. Update the cache check in `synthesize_flat` (3 lines)
2. Add a stats counter for cache hits and misses
3. Add 2-3 new test cases in `tests/test_ir.cpp`
4. Run all tests for regressions

## Acceptance criteria

- Cache check correctly identifies stale (contained TYPE_VAR)
  cached types and recomputes
- Cache check returns cached type for fully-resolved clean
  nodes (this is the actual speedup)
- After a mutation that affects only a small subtree, the
  typechecker re-infers only the dirty subtree (not the whole
  tree)
- All existing 30/30 suite, 5/5 leak, 201/201 run-tests,
  67/67 TS tests still pass

## Out of scope

- Stale var rebinding (more ambitious; risk of breaking
  semantic equivalence)
- True persistent caching across `infer_flat` calls (current
  registry is per-call, so cross-call caching is a different
  problem)
- QueryEngine integration with delta updates (mentioned in the
  issue; would be a follow-up)
- Multi-threaded type checking
