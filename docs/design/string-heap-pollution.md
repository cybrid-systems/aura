# string_heap_ dead-push pollution (observability)

**Issues:** #1488 (follow-up), #1072 (arena:adaptive-stats), **#1668** (full-surface audit + gate)  
**Related:** memory safety review; long-running agent/metrics poll loops  
**Full audit catalog:** [dead-heap-push-audit-1668.md](./dead-heap-push-audit-1668.md)

## Problem

Some primitives performed `string_heap_.push_back(...)` while building
return values, then **never used** the resulting indices — typically after an
incomplete refactor from string-typed pairs to int-typed pairs. Each call
leaked entries into `string_heap_`.

Example (pre-#1072 `arena:adaptive-stats`):

```cpp
auto idx_t = ev.string_heap_.size();
ev.string_heap_.push_back(std::to_string(trig));  // dead
auto idx_s = ev.string_heap_.size();
ev.string_heap_.push_back(std::to_string(skip));  // dead
// pair built with make_int(trig/skip); (void)idx_* only silences warnings
```

At ~50 bytes/call, a 1M-iteration poll loop could add tens of MB of dead
strings with no functional benefit.

## Contract (post-#1488 / #1668)

| Surface | Rule |
|---------|------|
| `arena:adaptive-stats` | Returns `(trigger . skip)` as **pair of ints** only; **no** `string_heap_` growth per call (#1072 / #1488) |
| Other stats / metrics | May push **used** keys/values for hash/list returns; must not push discarded intermediates |
| Module / mutate paths | No bare dead intern leftovers (`load_module` uses `module_names_` only — #1668) |
| Full `src/` | `./build.py gate` runs `audit_dead_heap_push.py --strict` |

## Detection

```bash
python3 scripts/audit_dead_heap_push.py          # report
python3 scripts/audit_dead_heap_push.py --strict # CI-style gate
./build.py dead-heap-push                        # unit tests + strict
```

Heuristics (#1668):

1. `auto idx = string_heap_.size();` + `push_back`, then `idx` never *really*
   used — `(void)idx` counts as **non-use**.
2. Bare `push_back` with no nearby `string_heap_.size()` capture.
3. Comment-only lines ignored; IR dual-heap mirror allowlisted.

## Tests

| File | Role |
|------|------|
| `tests/test_issue_1488.cpp` | 1000× re-eval poll: heap growth ~1N (re-parse only), residual vs pairs ~0 (not +2N dead push); pair-of-ints shape |
| `tests/test_production_hardening_1072_1096.cpp` | Smoke: adaptive-stats ok + hardening flag |
| `tests/test_audit_dead_heap_push.py` | Auditor unit tests + src/ clean (#1668) |

## AC map (#1488)

| AC | Status |
|----|--------|
| 1 Remove dead push in `arena:adaptive-stats` | Done (#1072); comment retained under #1488 |
| 2 Global scan + clean similar dead sites | #1488 partial; **#1668 full surface + gate** |
| 3 Long poll: `string_heap_.size()` stable | `test_issue_1488` AC2 |
| 4 Docs / metrics note | this file + dead-heap-push-audit-1668.md |

## Non-goals

- Eliminating **legitimate** per-call `pairs_` growth for returned pair values.
- Changing stats hash key interning for hash-returning dashboards (those
  indices are live).
- Full string_heap GC / compaction (separate track).
