# MutationBoundaryGuard move transfers full ownership (#1767)

**Issue:** [#1767](https://github.com/cybrid-systems/aura/issues/1767)  
**Sibling:** [#184](https://github.com/cybrid-systems/aura/issues/184),
[#1764](https://github.com/cybrid-systems/aura/issues/1764)  
**Files:** `evaluator.ixx`  
**Status:** P2 — move left depth math correct but dropped fields.

## Depth math (already correct)

| Event | depth |
|-------|-------|
| source ctor | ++ |
| move ctor | no change (slot stays with owner) |
| moved-from dtor (`ev_==nullptr`) | no-op |
| moved-to dtor | -- |

Net: +1 / −1. `noexcept` move keeps this safe.

## Real gap fixed (#1767)

Move ctor/assign only transferred `ev_`, `flag_`, `lock_` (+ two
bools). After `std::move`:

- target had default `enter_ts_` / `is_outermost_` → hold metrics
  skipped (`enter_ts_.has_value()` false)
- `is_outermost()` lied; panic / atomic-batch flags lost

## Fix

Move ctor and move-assign transfer:

`had_panic_checkpoint_`, `fine_rollback_`, `atomic_batch_active_`,
`suppress_bump_`, `is_outermost_`, `inert_`, `enter_ts_`, `ev_`,
`flag_`, `lock_`.

Moved-from is fully cleared. Move-assign releases `*this` by
move-to-local + dtor (full depth/lock path).

## Tests

`tests/test_guard_move_ownership_1767.cpp`
