# Fixed string_heap slots for intend intermediates (#1721)

**Issue:** [#1721](https://github.com/cybrid-systems/aura/issues/1721)  
**Files:** `evaluator_primitives_agent.cpp`  
**Status:** P2 memory — retry loop polluted `string_heap_`.

## Problem

Each `intend` attempt pushed goal/code/error strings onto
`ev.string_heap_` without reclaim. Long-running agents grew the heap
by O(attempts × calls).

## Fix (Option C refined)

Reuse three fixed slots via `put_slot`:

| Slot | Use |
|------|-----|
| `slot_goal` | goal string for gen/fix |
| `slot_code` | code for fix/ver |
| `slot_err` | last_error for fix |

Only the final status string is a new `push_back` (`finish_result`).
Avoids concurrent-unsafe `pop_back` of intermediate ranges.

## Tests

`tests/test_intend_heap_slots_1721.cpp`
