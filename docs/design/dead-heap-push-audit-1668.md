# Dead string_heap_ push pollution audit (#1668)

**Issue:** [#1668](https://github.com/cybrid-systems/aura/issues/1668)  
**Builds on:** #1488 · #1072  
**Status:** P1 audit — full EDSL surface scanned; confirmed-dead cleaned; gate strict.

## Goal

Run the #1488 heuristic across the whole `src/` tree (not only
`arena:adaptive-stats`), distinguish **dead** vs **legitimate**
`string_heap_.push_back`, fix confirmed dead sites, and hard-gate
regressions.

## Tooling

```bash
python3 scripts/audit_dead_heap_push.py          # report
python3 scripts/audit_dead_heap_push.py --strict # CI / gate (exit 1 on hits)
python3 scripts/audit_dead_heap_push.py --json   # machine-readable
./build.py dead-heap-push                        # unit tests + --strict
./build.py gate                                  # includes dead-heap-push
```

### Heuristics (#1668 hardened)

| Kind | Rule |
|------|------|
| `unused-index` | `auto idx = string_heap_.size();` + `push_back` within ~40 lines, then `idx` never *really* used |
| `(void)idx` | Counts as **non-use** (warning silence only — #1488 design note) |
| `bare-push` | `push_back` with **no** `string_heap_.size()` in the previous ~30 lines |
| Comments | `// … string_heap_.push_back …` ignored |
| Allowlist | IR dual-heap mirror in `ir_executor_impl.cpp` (local heap kept aligned with primitives for coercion; `make_string` uses `prim_idx`) |

## Audit results (2026-07-18 / #1668)

### Issue-listed grep candidates (stale line numbers)

The original #1668 body listed ~20 line numbers from a coarse grep. Re-checked
against current main: **all remaining size→push sites use the index**
(`make_string(idx)`, hash keys, etc.). Examples:

| Site (pattern) | Verdict |
|----------------|---------|
| `evaluator_eval_flat` LiteralString / Variable intern | USED (`make_string(idx)`) |
| `compile_02` / `compile_06` `cvt` / `add_entry` | USED (string keys/values) |
| `compile_07` JSON / `seva:generate-regression` | USED |
| `messaging` session/id returns | USED |
| `agent` / `mutate` listed lines | **stale** — not heap-push sites anymore |

No per-file follow-up issues (#1670+) required for those candidates.

### Confirmed dead (fixed in this issue)

| File | Fix |
|------|-----|
| `src/compiler/evaluator_module_loader.cpp` | Removed bare `string_heap_.push_back(resolved)` after `load_module` — display name already in `module_names_`; index never captured (#1488 bare-push class) |

### Intentional allowlist

| File | Reason |
|------|--------|
| `ir_executor_impl.cpp` ConstString | Dual write: primitives heap (returned index) + local `string_heap_` (coercion table) |

## Tests

| File | Role |
|------|------|
| `tests/test_audit_dead_heap_push.py` | Auditor unit tests + src/ clean strict |
| `tests/test_issue_1488.cpp` | Long-poll adaptive-stats heap growth (parent) |

## AC map (#1668)

| AC | Status |
|----|--------|
| 1 Script present and runnable on full `src/` | Done |
| 2 Run audit; classify dead vs used | Done — clean after module_loader fix |
| 3 Fix confirmed dead (or file follow-ups) | Done in-tree (1 site); no #1670+ needed |
| 4 Gate / CI strict | `./build.py gate` → `cmd_dead_heap_push` |
| 5 Docs | this file + `string-heap-pollution.md` update |

## Non-goals

- Removing legitimate hash-key / return-string interning.
- Full string_heap GC / compaction.
- Blind deletion of every `push_back` near `to_string` without use analysis.
