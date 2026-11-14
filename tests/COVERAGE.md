# Coverage policy (runtime vs gate)

This is the SSOT for how an issue ships its contract. Agents: follow the
decision tree. Gate enforces it (`check_coverage_policy.py`).

## Decision tree

```
1. Behavior the engine can run?
   → Extend an existing tests/<module>/ suite or *_batch (see HOMES.md).
     Assert results, metrics, refuse paths. STOP.

2. Schema-only (query:*-stats / engine:* key)?
   → One row in tests/compiler/obs_schema_cases.hpp
     (+ production_sweep_cases.hpp if production-flagged). STOP.

3. Static “this source still contains X” (identifier, stamp, forbid-file)?
   → scripts/coverage/manifests/<N>.json
     { "issue": N, "checks": [ { "path": "...", "contains": ["..."] } ] }
     Optional: contains_any, absent, forbid, forbid_glob, symbol.
     `symbol` searches the brace-matched body (`source_block_after`),
     not a fixed character window. `absent` is a substring that must
     not appear. `forbid` is a path that must not exist.
     STOP. Do NOT add check_*.py. Do NOT wire a new cmd_* in build.py.

4. Static check that needs control flow (window around a function,
   regex, AST, order of two statements, subprocess)?
   → scripts/coverage/checks/check_<feature>.py with real logic.
     No issue digits in the filename. Put the issue number in a comment
     or in scripts/coverage/manifests/<N>.json.
     Gate auto-discovers check_*.py. STOP.

Where a behavior test goes is tests/HOMES.md, not this file.
`python3 build.py gate` rejects a newly added issue-numbered test file.

NEVER:
  ✗ new scripts/coverage/checks/check_*.py that only do `needle in file`
  ✗ thin wrapper whose body is `runner.py --issue N`
  ✗ new scripts/check_*.py (that directory is frozen)
  ✗ new named ./build.py <foo>-coverage command
  ✗ C++ CHECK(src.find("Issue #N")) as a substitute for (1) or (3)
  ✗ new check_*_<3–5 digits>.py or check_<3–5 digits>_*.py
  ✗ new tests/**/test_issue_*.py or tests/**/test_*_<3–5 digits>.py
  ✗ new tests/suite/*_<3–5 digits>.aura or tests/suite/issue_<3–5 digits>*.aura
  ✗ tests/issues/test_issue_N.cpp, test_*_<N>.cpp, docs/design/<N>-*
```

Runtime tests catch intern-cache / eval bugs. Manifests catch “someone
deleted the production call”. Grep-the-source Python files catch neither
better than a JSON row, and they break on renames.

## Layout

| Path | Role |
|------|------|
| `tests/<module>/test_*.cpp`, `tests/suite/*.aura` | behavior |
| `scripts/coverage/manifests/<N>.json` | declarative source-cite |
| `scripts/coverage/checks/check_*.py` | custom static logic only |
| `scripts/coverage/runner.py` | runs manifests |
| `scripts/coverage/run_checks.py` | gate: remaining check_*.py + manifests |
| `scripts/coverage/fold_simple_checks.py` | fold leftover substring scripts (`--wrappers-only` / `--apply`) |
| `scripts/coverage/retarget_pins.py` | rewrite a manifest path when a test file moves (`--from` / `--to`) |
| `scripts/coverage/simple_check_grandfather.txt` | existing substring `check_*.py` still allowed; **do not add names** |
| `scripts/coverage/manifest_skip.txt` | stale manifests skipped by `runner.py --all` |

```bash
python3 scripts/coverage/runner.py --issue 3457
python3 scripts/coverage/run_checks.py --changed
./build.py gate --changed
```

## Manifest shape

```json
{
  "issue": 3457,
  "title": "eval_flat intern is pool-local",
  "checks": [
    {
      "ac": "AC1",
      "path": "src/compiler/evaluator_eval_flat.cpp",
      "contains": ["bind_sym_intern_pool(p)", "Issue #3457"],
      "symbol": "bind_sym_intern_pool"
    },
    {
      "ac": "AC2",
      "forbid": ["tests/compiler/test_issue_3457.cpp"]
    },
    {
      "ac": "AC3",
      "path": "src/compiler/evaluator_eval_flat.cpp",
      "absent": ["a substring that must not return"]
    }
  ]
}
```

No `build.py` self-wiring row. Gate discovers manifests by filename.

## Why not one Python linter per issue

`run_checks.py` already auto-discovers `check_*.py`. A 40-line `must("x" in f)`
script is a slower, more fragile duplicate of a manifest row. Custom Python
is reserved for contracts JSON cannot express.
