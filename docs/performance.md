# Performance

Numbers on this page come from `python3 bench/run.py`. The class-2
medians below are the committed baseline (`bench/baselines/agent.json`).
Re-run the harness and it must reproduce these checksums. Times move
with the machine; the statistical gate allows that within the #1936
tolerance.

Measured at commit `d7de6a11b0e36683f7585e0c5b9e1583f7194f25` on Linux-7.0.14-orbstack-00380-ga7e0a2dc9535-aarch64-with-glibc2.44
(14 CPUs). Clock is in-process
`(monotonic-ms)`. Startup is excluded. No new primitive.

Class 2 still runs with `AURA_SANDBOX=off`. That variable is the dev
escape hatch. New Soft experiments use `aura soft`. `AURA_SOFT` is not
set and the harness does not mint the scoped grant.

LuaJIT column: skipped (interpreter not on PATH in the dev image that
produced this page). Speculative JIT: no separate mode, column absent.
CPython is reported and does not gate. A JIT mismatch or hang is #4567
and does not gate.

CI (`bench/run.py --gate`, also the pre-push hook) runs the 10-define
agent rows, rollback, 8 fibers, fib, the int loop, and evo-kv.
It fails only on a wrong class-2 checksum or a #1936 regression
against the medians below. 200 and 2000 defines are `--full`. On this
box a 200-define closed loop is about 16 s (3 samples). One 2000-define
leaf sample produced no BENCH line before the 2700 s wall-clock timeout,
so the baseline has no 2000 median. The case is still selected by `--full`.

## Class 2 baseline

| case | median ms | p95 ms | min ms | max ms | noise | result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| agent-leaf-10 | 66 | 67.8 | 59 | 68 | 0.0567 | 4 |
| agent-hot-10 | 75 | 78.4 | 74 | 79 | 0.0247 | 11 |
| agent-rollback | 78 | 82.6 | 76 | 83 | 0.0357 | 70 |
| agent-fibers-8 | 312 | 337 | 290 | 343 | 0.0564 | 1001002000 |
| agent-leaf-200 | 16148 | 16380.2 | 16023 | 16406 | 0.0099 | 4 |
| agent-hot-200 | 16241 | 16619 | 14866 | 16661 | 0.0472 | 11 |

## Recorded medians

agent-leaf-10 median_ms=66
agent-hot-10 median_ms=75
agent-rollback median_ms=78
agent-fibers-8 median_ms=312
agent-leaf-200 median_ms=16148
agent-hot-200 median_ms=16241

## This run

Five interpreter samples, median. JIT is one extra shot.

| class | case | ok | median ms | result | jit |
| --- | --- | --- | ---: | --- | --- |
| compute | fib | True | 136 | 2584 | ok 33ms |
| compute | int-loop | True | 213 | 18003000 | ok 33ms |
| compute | float-loop | True | 97 | 9000 | ok 33ms |
| compute | nbody | True | 640 | 4173024 | timeout |
| compute | fannkuch | True | 521 | 7 | timeout |
| compute | hash-lookup | True | 214 | 1124250 | error |
| compute | cons-throughput | True | 128 | 4000 | timeout |
| compute | string-build | True | 58 | 2000 | ok 33ms |
| agent | agent-leaf-10 | True | 66 | 4 |  |
| agent | agent-hot-10 | True | 75 | 11 |  |
| agent | agent-rollback | True | 78 | 70 |  |
| agent | agent-fibers-8 | True | 312 | 1001002000 |  |
| agent | agent-leaf-200 | True | 16148 | 4 |  |
| agent | agent-hot-200 | True | 16241 | 11 |  |
| workload | evo-kv | True | 70 | 392925 |  |
| workload | multi-agent | True | 117 | 358828 |  |
| workload | control-plane | True | 68 | 79800 |  |

JIT matched the interpreter on fib (33 ms), the int loop (33 ms), the float
loop (33 ms), and string building (33 ms). nbody, fannkuch, and cons-throughput
hit the JIT timeout. hash-lookup aborted under `--jit` (SIGABRT). CPython
checksums matched and are not a pass/fail column.
