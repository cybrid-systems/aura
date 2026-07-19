# Detailed EnvFrame resolve status (#1756)

**Issue:** [#1756](https://github.com/cybrid-systems/aura/issues/1756)  
**Sibling:** [#1754](https://github.com/cybrid-systems/aura/issues/1754), [#1360](https://github.com/cybrid-systems/aura/issues/1360)  
**Files:** `evaluator.ixx`, `evaluator_env.cpp`  
**Status:** P2 — `resolve_env_frame` collapsed all failures to `nullptr`.

## Problem

`resolve_env_frame` returned `nullptr` for NULL id and OOB without
distinguishing them, and did not surface terminal `INVALID_VERSION`
(#356) as a separate status.

## Fix

| Status | Meaning |
|--------|---------|
| `OK` | usable frame pointer |
| `NULL_ID` | `id == NULL_ENV_ID` |
| `OOB` | `id >= env_frames_.size()` |
| `INVALID_VERSION` | live slot marked terminal (#356) |
| `GENERATION_MISMATCH` | reserved for free-list reuse |

APIs:

- `resolve_env_frame_detailed` / `_mut_detailed` — status + frame
- `resolve_env_frame` / `_mut` — **unchanged BC**: nullptr only for
  NULL/OOB; in-range INVALID_VERSION still returns a pointer

`walk_env_frames` uses detailed resolve so it stops on INVALID_VERSION
without walking poison parents.

## Tests

`tests/test_resolve_env_frame_detailed_1756.cpp`
