# Benchmark gate

Issue 1936 is the statistical gate for Aura timings. A case is a regression
only when the absolute delta is above `min_delta_ms` and the ratio is past
the tolerance. The default tolerance is 20 percent (`tolerance_percent`),
the absolute floor is 20 ms, and the catastrophic ratio is 3.0. The
rationale for the floor: a jump from 5 ms to 18 ms is environment noise,
while a jump that clears both the ratio and the floor is a real hit.
Catastrophic ratios still have to clear the floor.

`tests/bench/benchmark.py` implements that rule as `classify_regression`.
`./build.py bench` runs the older whole-process suite. Those cases are
startup-dominated and stay as they are.

Issue 4566 adds a second harness, `python3 bench/run.py`, which times three
classes inside the process with `(monotonic-ms)` and excludes startup. The
CI slice fails only on a class-2 checksum miss or a 1936 regression against
`bench/baselines/agent.json`. Class 1 and class 3 are reported. The numbers
the harness measured are on the performance page (`docs/performance.md`).

```
python3 bench/run.py --class compute
python3 bench/run.py --class agent
python3 bench/run.py --class workload
python3 bench/run.py --gate
python3 bench/run.py --full
```

Class 2 still sets `AURA_SANDBOX=off` on the child. That variable is the
dev escape hatch. New Soft experiments use `aura soft`, which mints a
scoped Mutate grant on the Restricted face. The harness does not set
`AURA_SOFT` and does not mint that grant.
