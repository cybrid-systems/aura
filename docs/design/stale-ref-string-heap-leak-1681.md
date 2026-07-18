# stale-ref error paths: no string_heap_ pollution (#1681)

**Issue:** [#1681](https://github.com/cybrid-systems/aura/issues/1681)  
**File:** `src/compiler/evaluator_primitives_mutate.cpp`  
**Status:** P2 memory — stop interning static error tags per blocked call.

## Problem

Strict stale-ref paths did:

```cpp
auto idx = ev.string_heap_.size();
ev.string_heap_.push_back("stale-ref");
return mev(ev.string_heap_[idx].c_str(), "…");
```

Every blocked call permanently grew `string_heap_` with a static literal.

## Fix

Pass the literal tag directly (static storage; same as `mev("out-of-range", …)`):

```cpp
return mev("stale-ref", "stable-ref is stale (Strict policy blocked)");
```

Sites: StableNodeRef strict, raw node-id strict, `mutate:check-stable-ref` strict.

`query:stale-ref-policy` still pushes policy name for `make_string` return values (legitimate).

## Tests

`tests/test_stale_ref_string_heap_1681.cpp` — N strict blocks → heap size stable.
