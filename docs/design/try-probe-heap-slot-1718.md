# Fixed probe string_heap slot for synthesize:optimize (#1718)

**Issue:** [#1718](https://github.com/cybrid-systems/aura/issues/1718)  
**Files:** `evaluator_primitives_agent.cpp`  
**Status:** P1 correctness — try_probe unbounded string_heap growth.

## Problem

Each `try_probe` call did `string_heap_.push_back(call_src)`. GA fitness
runs gen × pop × probes → hundreds of never-freed probe strings and
realloc risk for concurrent string indices.

## Fix (Option C-style)

Allocate **one** `probe_slot` per `compute_fitness` invocation:

1. First probe: `push_back` and remember index  
2. Later probes: `string_heap_[probe_slot] = call_src`  
3. `eval` always receives `make_string(probe_slot)`

Not global index 0 (would clobber live strings). Slot is fitness-local.

## Tests

`tests/test_try_probe_heap_slot_1718.cpp`
