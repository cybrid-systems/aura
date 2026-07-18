# Compact hook exclusive install + chain (#1666)

**Issue:** [#1666](https://github.com/cybrid-systems/aura/issues/1666)  
**Builds on:** #1446 · #685 · #1662  
**Status:** P1 correctness — no silent drop of prior compact hooks.

## Problem

`set_on_compact_hook` **replaces** the previous `std::function`.  
`CompilerService` ctor did:

```
evaluator.set_arena(&arena_);           // installs re_pin hook
arena_.set_on_compact_hook(profiler);   // SILENTLY dropped re_pin
```

## Contract

| API | Semantics |
|-----|-----------|
| `set_on_compact_hook(h)` | **Replace** (document exclusive install) |
| `take_on_compact_hook()` | Move out current hook; arena empty |
| `has_on_compact_hook()` | Query |

**Multi-listener pattern** (required when stacking):

```cpp
auto prior = arena.take_on_compact_hook();
arena.set_on_compact_hook([prior = std::move(prior)] {
  if (prior) prior();
  /* new work */
});
```

## Fixes

1. **ASTArena**: `take_on_compact_hook` + `has_on_compact_hook` + docs on replace.
2. **Evaluator::set_arena**: take prior + chain re_pin after prior.
3. **CompilerService**: take prior (Evaluator re_pin) + chain ShapeProfiler.

## Tests

`tests/test_compact_hook_chain_1666.cpp`
