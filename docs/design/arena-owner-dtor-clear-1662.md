# ~Evaluator must clear arena_owner (#1662)

**Issue:** [#1662](https://github.com/cybrid-systems/aura/issues/1662)  
**Builds on:** #1546 · #1554 · #63723  
**Status:** P0 hotfix — prevent UAF when ASTArena outlives Evaluator.

## Bug

`set_arena` / `set_temp_arena` install:

```
arena_->set_arena_owner(this, &Evaluator::arena_quota_allow);
arena_->set_on_compact_hook([this]{ on_arena_compact_hook(); });
```

`~Evaluator` cleared TLS slots (`g_yield_hook_evaluator`, …) but **not**
`arena_owner_` / compact hook. A surviving arena's next `allocate_raw`
or compact would call into a dead `Evaluator*` → **use-after-free**.

## Fix

At the **top** of `~Evaluator`:

```cpp
if (arena_) {
  arena_->clear_arena_owner();
  arena_->set_on_compact_hook({});
  arena_ = nullptr;
}
if (temp_arena_) {
  temp_arena_->clear_arena_owner();
  temp_arena_ = nullptr;
}
if (arena_group_)
  arena_group_->clear_default_arena_owner();
```

Also: `set_arena` rebind clears previous arena's owner **and** compact hook.

## Tests

| File | Role |
|------|------|
| `tests/test_arena_owner_dtor_clear_1662.cpp` | **#1662** AC |
| `tests/test_arena_quota_wired.cpp` | Install/quota path |

## Related

- `ASTArena::clear_arena_owner` — resets `{owner, allow_fn}` to null
- `ArenaGroup::clear_default_arena_owner` — module arenas too
