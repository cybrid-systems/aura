# lookup_stats_impl heterogeneous lookup (#1671)

**Issue:** [#1671](https://github.com/cybrid-systems/aura/issues/1671)  
**Builds on:** #1439 (internal stats table) · #891 / #914 (Primitives transparent hash)  
**Status:** P2 perf — no per-lookup `std::string` allocation.

## Problem

```cpp
// pre-#1671
auto it = m.find(std::string(name));  // malloc + memcpy every call
```

Hot paths (`engine:metrics :all` / `:prefix`, `stats:get`, catalog walks)
call `lookup_stats_impl` once per stats name (~400+). Each call allocated
a temporary key. Under 1M-iter poll benches that is large heap churn
(same class of pollution as #1488 dead heap push).

## Fix

C++20 heterogeneous lookup on the private map:

```cpp
std::unordered_map<std::string, PrimFn, StatsNameHash, StatsNameEq>
// StatsNameHash / StatsNameEq: using is_transparent = void;

m.find(name);  // string_view — zero alloc
```

`stats_impl_registered` uses `m.find(name) != m.end()` (no `optional<PrimFn>`
construction).

## Contract

| API | Behavior |
|-----|----------|
| `register_stats_impl(string, PrimFn)` | unchanged (owns key copy) |
| `lookup_stats_impl(string_view)` | same optional result; **no** key alloc |
| `stats_impl_registered(string_view)` | same bool; cheaper existence check |

No public surface change. Pattern mirrors `Primitives::StringHash` / `StringEq`.

## Tests

`tests/test_lookup_stats_impl_heterogeneous_1671.cpp` — facade by-name,
`:all`, `:prefix`, repeated lookup parity.

## Non-goals

- Changing `PrimFn` return to pointer (avoids std::function copy; separate).
- Replacing the map with `string_view` keys (lifetime would bind to registration strings only — current owned `std::string` keys are safer).
