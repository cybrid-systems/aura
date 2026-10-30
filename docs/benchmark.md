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
