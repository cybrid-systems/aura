# Benchmark baseline updates

Log of intentional baseline refreshes (Issue #1936).
Each `--update` with `--rationale` appends an entry here.

## 2026-07-22 — orch / multi-agent CI noise (meta only)

- **Rationale:** CI strict SLO failed `multi_agent_pipeline` / `par_orch_3_agents` /
  `par_orch_5_agents` at ~2.5× (≈370ms → ≈917ms). Local times stay ≈365–410ms
  (within baseline). All three share `require std/orchestrator` + fiber work and
  hit nearly identical CI wall times → co-schedule / cold require noise, not a
  product regression. Raised per-case `tolerance_percent` to 180 and
  `catastrophic_ratio` to 4.0 in `benchmark_meta.json` (baseline unchanged).
- **Cases:** multi_agent_pipeline, par_orch_3_agents, par_orch_5_agents
- **Command:** meta edit only (no `--update`)

## 2026-07-21 — seed entry

- **Rationale:** Introduce changelog file with #1936 statistical/relative gate; historical baseline retained from #1569 era without re-measure.
- **Cases:** 55 (see `benchmark_baseline.json`)
- **Command:** `docs bootstrap (no --update run)`

## 2026-08-25 — fib_20 CI runner drift (meta only)

- **Rationale:** CI strict SLO failed `fib_20` at 1.28× (133.7ms → 171.8ms, Δ38.1ms).
  Identical numbers on the previous commit (`6b3a8b8ec`, pre-#3294) → not a product
  regression from #3294. Same run showed `literal_int` 5× FASTER (98.8 → 20.5ms) on
  the same runner → environment drift since the 2026-07-18 baseline, not code.
  Raised per-case `tolerance_percent` to 40 and `min_delta_ms` to 50 in
  `benchmark_meta.json` (baseline unchanged; catastrophic 3.0 still fails hard).
- **Cases:** fib_20
- **Command:** meta edit only (no `--update`)
