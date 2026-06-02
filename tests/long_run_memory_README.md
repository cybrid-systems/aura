# Long-running memory pressure benchmark

This is the manual-validation companion to the CI tests
`tests/suite/memory_observability.aura` and
`tests/suite/memory_governance.aura`. The CI tests cover the
primitives, the policy plumbing, and a 1-2 minute stress scenario.

This script is for verifying the production observability + auto-gc
holds up over multi-hour workloads. It is **not** run by CI.

## Scenarios

* `module-cycle` — load + use + gc-module stdlib modules in a loop
* `concurrent-agents` — many intend + spawn cycles simulating Agent load
* `mixed-workload` — combines both, with mutate + set-code operations

## What to look for

* `main` arena `used-pct` stays under 70% (low/medium)
* No `critical`-level warnings in stderr
* `auto-gc` only fires when the heap is genuinely full
* `module_count` never grows without bound
* No crashes / memory leaks over the full duration

## Usage

```bash
# Smoke check (5 minutes)
python3 tests/long_run_memory.py --duration 5m

# Default (30 minutes)
python3 tests/long_run_memory.py

# Long (1 hour)
python3 tests/long_run_memory.py --duration 1h

# Specific scenario
python3 tests/long_run_memory.py --duration 1h --scenario concurrent-agents
```

Output: appends one CSV row per second to `./long_run_memory.log`
in the current working directory.

## When to run

* After any change to the memory pressure / auto-gc sampling logic
* Before cutting a release that touches arena / gc primitives
* Periodically (e.g. weekly) to catch slow memory leaks in production
