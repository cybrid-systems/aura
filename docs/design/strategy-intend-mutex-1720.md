# Mutex guards for intend/strategy vectors (#1720)

**Issue:** [#1720](https://github.com/cybrid-systems/aura/issues/1720)  
**Files:** `evaluator.ixx`, `evaluator_primitives_agent.cpp`, `evaluator_ctor.cpp`  
**Status:** P2 race — plain vectors under concurrent fibers.

## Problem

`strategies_`, `intend_history_`, and `timeline_` were unguarded
`std::vector`s. Concurrent `intend` / strategy primitives could tear
vector state.

## Fix

| Mutex | Protects |
|-------|----------|
| `strategies_mtx_` | `strategies_`, `active_strategy_` |
| `intend_history_mtx_` | `intend_history_`, `next_record_id_` |
| `timeline_mtx_` | `timeline_` |

Helpers: `timeline_push/clear/snapshot/tail`, `intend_history_push`.  
Writes use `unique_lock`; reads use `shared_lock`.

## Tests

`tests/test_strategy_intend_mutex_1720.cpp`
