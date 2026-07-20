# tests/fixtures/ — case data philosophy (#1962)

## Classification

| Path | Kind | Maintained how | Consumers |
|------|------|----------------|-----------|
| **`regression/*.json`** | Hand-written regression cases | Edit the matching theme shard | `tests/regression_cases.py` → `test_regression.py` |
| **`integ/*.json`** | Integration matrix | Split by pipeline + eval theme | `tests/integ_cases.py` → `build.py test integ` |
| **`benchmark/*.json`** | Bench / typecheck suite | Split by pipeline | `tests/benchmark_cases.py` → `benchmark.py` |
| **`smoke/*.json`** | Tiny CLI smoke | Single `all.json` | `tests/smoke_cases.py` |
| `issues_fast.json` | PR fast-tier targets | Hand list | `issue_tier.py`, CI |
| `issue_link_profiles.json` | Bundle membership | Hand / gen_issue_bundles | CMake bundles |
| `issue_bundle_extras.json` | Bundle CMake extras | Hand | `gen_issue_bundles.py` |
| `issue_integ_migrated.json` | Migration notes | Hand | docs/history |
| `issue_616/` | Issue-specific sample data | Hand | targeted tests |
| `basic_add.aura` | Tiny program sample | Hand | misc |

**Generated elsewhere (not here):** `docs/generated/*`, `tests/benchmark_baseline.json`
(SLO baseline next to `tests/`; must stay in sync with benchmark shards).

## Why shards

Monolithic `*_tests.json` files made every case edit a huge diff and frequent
merge conflicts. Cases now live under `tests/fixtures/<kind>/*.json`:

- **regression/** — one shard per name prefix (`functor.json`, `issue.json`, …)
- **integ/** — `typecheck.json`, `ir.json`, `serve.json`, `eval_<theme>.json`
- **benchmark/** — `eval.json`, `ir.json`, `typecheck.json`
- **smoke/** — `all.json`

Loaders merge all shards for a kind (sorted by filename). See
`tests/fixture_store.py`.

## Soft budgets (enforced by `fixture_check`)

| Budget | Limit |
|--------|------:|
| Max bytes per shard | 12 000 |
| Max cases per shard | 50 |

If a check fails, split the shard (new theme file) instead of growing it.

## Tooling

```bash
python3 tests/run.py fixtures                 # validate (gate uses this)
python3 scripts/fixtures_tool.py validate     # same checks
python3 scripts/fixtures_tool.py status       # inventory + oversized flags
python3 scripts/fixtures_tool.py pack integ   # merge to stdout (debug only)
```

Do **not** re-commit large mono `regression_tests.json` / `integ_tests.json` /
`benchmark_tests.json` / `smoke_tests.json` — they were removed in #1962.

## Adding a case

1. Pick the kind (regression / integ / benchmark / smoke).
2. Open the smallest matching shard (or create `theme.json` under that kind).
3. Append a JSON object with the same schema as siblings (`name` unique globally
   within the kind).
4. Run `python3 tests/run.py fixtures`.
5. For benchmark cases, refresh baseline if needed:
   `./build.py bench` / project baseline update flow.

## Legacy mono paths

Consumers must use `fixture_store.load_case_array(kind)` or the `*_cases.py`
helpers. Direct reads of `fixtures/*_tests.json` are obsolete.
