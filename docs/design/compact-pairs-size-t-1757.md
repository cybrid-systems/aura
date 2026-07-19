# compact_pairs returns std::size_t (#1757)

**Issue:** [#1757](https://github.com/cybrid-systems/aura/issues/1757)  
**Sibling:** [#206](https://github.com/cybrid-systems/aura/issues/206),
[`compact_env_frames`](https://github.com/cybrid-systems/aura/issues/1386)  
**Files:** `evaluator.ixx`, `evaluator_gc.cpp`  
**Status:** P2 — count return used signed `int64_t`.

## Problem

`Evaluator::compact_pairs` returned `std::int64_t` for "number of
pairs after compact". That is a pure count semantic; signed return
invites `if (count < 0)` error patterns and is inconsistent with
`compact_env_frames()` which already returns `std::size_t`.

## Fix

Change the return type to `std::size_t` (match sibling compact API).
No failure path: compact always completes and returns `pairs_.size()`.

Issue body Option B (`bool` + out-param) was not taken — there is no
error condition to report, and the existing `compact_env_frames`
pattern is the project convention for compact counts.

Remap entries remain `std::int64_t` (`-1` = dead); only the *count*
return type changed.

## Tests

`tests/test_compact_pairs_size_t_1757.cpp`  
`tests/test_issue_206.cpp` (call sites updated to `std::size_t`)
