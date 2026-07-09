# JIT Production Hardening (Issues #821 / #822 / #823)

Phase 1 observability surfaces for concurrent AI mutation workloads.

## #821 Fiber-local exception stacks

Guest `Raise` keeps the EH bridge (`aura_throw_exception`). Cross-fiber
handler confusion is tracked via:

- `(query:jit-fiber-exception-stats)` schema **821**
  - `fiber-local-ex-stack`, `cross-fiber-prevented`, `deopt-to-interpreter`

Phase 2: true per-fiber ExStack keyed by fiber_id + version check.

## #822 L2 specialization maturity

- `(query:l2-specialization-deopt-stats)` schema **822**
  - pair fastpath, deopt-on-version-mismatch, GuardShape narrow, linear probe

## #823 Opcode coverage + per-fn deopt

- `(query:opcode-coverage-deopt-stats)` schema **823**
  - coverage hits, unhandled hot, per-fn deopt, zero-fallback policy flag

See also `jit_exception_bridge.md` (#811).
