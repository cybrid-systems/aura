# tests/domain/arena/ — Arena / compaction pilot (#1959)

**First theme-directory pilot** for the domain migration plan
([#1957](https://github.com/cybrid-systems/aura/issues/1957) inventory ·
[#1958](https://github.com/cybrid-systems/aura/issues/1958) guidelines).

Use this directory as the **reference layout** for future themes
(`domain/mutation/`, `domain/fiber/`, …).

## Scope

Arena allocation, auto-compact policy, compaction orchestration, compact
hooks / mutation-log compact, compact_sweep, GC root / defer helpers that
touch arena lifecycle, and concurrent defrag / safepoint primitives.

| File | Role | Build |
|------|------|--------|
| `test_arena_batch.cpp` | Family batch: #1621 #405 #1662 #546 #1546/#1554 | `EXCLUDE_FROM_ALL` — `ninja test_arena_batch` |
| `test_compact_batch.cpp` | Family batch: #1842 #1666 #1362 #1757 | `EXCLUDE_FROM_ALL` — `ninja test_compact_batch` |
| `test_compact_sweep_batch.cpp` | Family batch: #1732 #1865 #1866 | `EXCLUDE_FROM_ALL` — `ninja test_compact_sweep_batch` |
| `test_gc_batch.cpp` | Family batch: #1667 #1734 #1864 | `EXCLUDE_FROM_ALL` — `ninja test_gc_batch` |
| `test_arena_defrag_concurrent.cpp` | Default-build concurrent defrag (#1390) | in `all_test_issue_targets` |

Harness: `#include "test_harness.hpp"` ([#1960](https://github.com/cybrid-systems/aura/issues/1960)).

## Intentionally not moved yet

| Path | Why |
|------|-----|
| `tests/test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp` (#743) | Still a **bundle member** (`jit_late3`); keep at `tests/` until batch+bundle entry points are unified |
| `tests/test_gc_evaluator_integration.cpp` | Custom `add_executable` + contract sources in CMakeLists |
| `tests/issues/test_issue_*arena*` / compact-related issues | Bundle sources under `tests/issues/`; coverage largely superseded by the batch drivers above — marked deprecated in headers |

## CMake resolution

`aura_resolve_test_cpp(NAME)` (cmake/AuraTest.cmake) searches:

1. `tests/issues/<NAME>.cpp`
2. `tests/domain/<NAME>.cpp`
3. `tests/domain/*/<NAME>.cpp` ← **this directory**
4. `tests/<NAME>.cpp`

Target names are unchanged (`test_arena_batch`, …); only the source path moved.

## How to add a new arena AC

1. Prefer extending the matching `*_batch.cpp` section (or this README’s
   default-build target for light gates).
2. Do **not** add `tests/issues/test_issue_N.cpp`.
3. Use `test_harness.hpp` only.
4. Rebuild: `ninja -C build test_arena_batch` (or the target you edited).

## Migration recipe (for the next theme)

1. Inventory theme bucket (`python3 scripts/inventory_legacy_tests.py`).
2. `mkdir tests/domain/<theme>/` + README (copy this file as a template).
3. `git mv` family `*_batch.cpp` + any default-build standalone into the dir.
4. Rely on `aura_resolve_test_cpp` (no CMake target renames).
5. Deprecate leftover issue files with a one-line header pointer.
6. Refresh inventory; document “not moved yet” exceptions (bundles / custom link).

Parent policy: [`../README.md`](../README.md) · [`../../README.md`](../../README.md).
