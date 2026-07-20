# Benchmark SLO gate — Issues #1569 / #1936

## Quick start

```bash
./build.py build
./build.py bench                     # soft: warn on regression
./build.py bench --strict            # hard SLO gate (also AURA_CI_STRICT_BENCH=1)
./build.py bench --strict --tolerance 5 --statistical
python3 tests/bench/benchmark.py --update --rationale "explain the intentional change"
```

Thin entrypoint: `python3 tests/benchmark.py …` → `tests/bench/benchmark.py`.

## Comparison modes (#1936)

| Mode | Flag | Behaviour |
|------|------|-----------|
| **relative** (default) | `--mode relative` | Fail when `Δ > min_delta` **and** (`cur/base > 1+tol/100` **or** `cur/base ≥ catastrophic`) |
| **exact** | `--mode exact` | Near-exact ratio band (0.1% tol) still with absolute floor |

Defaults (backward compatible with #1569):

- `tolerance` = **20%** (ratio 1.2×)
- `min_delta` = **20 ms**
- `catastrophic` = **3.0×**

Tighter local gate:

```bash
./build.py bench --strict --tolerance 5 --runs 3
```

## Statistical noise control

| Flag | Meaning |
|------|---------|
| `--runs N` | Run each case N times; store/compare **median** `time_s` |
| `--statistical` | Shorthand for `--runs 3` |
| `AURA_BENCH_RUNS=N` | Env override for CI |

Samples are recorded on each case as `time_samples`, `time_min_s`, `time_max_s`.

## Per-benchmark metadata

`tests/bench/benchmark_meta.json`:

```json
{
  "defaults": { "tolerance_percent": 20, "min_delta_ms": 20 },
  "cases": {
    "literal_int": { "tolerance_percent": 25, "description": "cold-start noise" }
  }
}
```

## Updating the baseline

Baselines must stay **explainable**:

```bash
python3 tests/bench/benchmark.py --update \
  --rationale "IR lower hot path sped up by #NNNN; re-base after medians"
```

- Writes `tests/bench/benchmark_baseline.json`
- Appends an entry to `tests/bench/benchmark_updates.md`
- Fails without `--rationale` / `AURA_BENCH_RATIONALE`

### When is an update OK?

1. Intentional compiler/runtime change that moves hot-path cost.
2. Environment change (new LLVM major, different CI image) after re-measure.
3. Fixture set change (added/removed cases) — still require rationale.

### When is it NOT OK?

- Silencing a real regression without investigation.
- “CI was red once” without local median re-run.

## CI

GitHub Actions sets `AURA_CI_STRICT_BENCH=1`, which enables `--strict`.
Optional: `AURA_BENCH_RUNS=3` for median-of-3 on main/nightly if wall time allows.

## Files

| Path | Role |
|------|------|
| `tests/bench/benchmark.py` | Runner + gate |
| `tests/bench/benchmark_cases.py` | Case loader (fixtures) |
| `tests/bench/benchmark_baseline.json` | Stored timings |
| `tests/bench/benchmark_meta.json` | Tolerances / owners |
| `tests/bench/benchmark_updates.md` | Changelog of updates |

## Related

- Issue #1569 original hard SLO gate
- Issue #1936 statistical/relative robustness
- Coverage: `docs/testing.md` (#1933)
