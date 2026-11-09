# std/surrogate — prescreening expensive evaluations (#4943)

Aura's evolution loop evaluates an agent variant by replaying tasks, running
the safety checks and a regression. That is the expensive step. `std/surrogate`
ranks and filters candidates *before* that price is paid, and it says out loud
what it skipped.

References: MOEA/D-EGO (Zhang, Liu, Tsang, Virginas, IEEE TEVC 2010) for
per-subproblem GP models + EI pre-selection; ShinkaEvolve (arXiv:2509.19349)
for the cheap-checks-first gate and novelty rejection.

## When to use which model

| Model | Use when | Notes |
|-------|----------|-------|
| **GP, rbf** | Numeric genes / weights, n <= 200, d small | Smooth default |
| **GP, matern52** | Numeric genes, less smooth response | Sharper mean, wider error |
| **kNN** | Code candidates (AST counts), tiny samples, d > 32 | No linear algebra |

`(surr:auto n d opts)` picks kNN for `n < 4` or `d > 32`, GP otherwise.

## API

```aura
(require "std/surrogate" all:)

(define m (surr:fit! (surr:gp (hash "kernel" "rbf" "noise" 1e-6 "ls" 0.5))
                     X y))                   ; X = list of points, y = numbers
(surr:predict m (list 0.3))                  ; -> (mean . var)

(surr:ei m x best)                           ; expected improvement (maximize)
(surr:lcb m x 2.0)                           ; lower confidence bound
(surr:ts m x)                                ; one Thompson sample

(define ps (surr:prescreen m cands 5 (hash "acq" "ei" "best" best
                                            "audit-sample" 0.1)))
(hash-ref ps "selected")                     ; top-k rows, "sent" #t
(hash-ref ps "rejected")                     ; the rest
(hash-ref ps "audit")                        ; mean, var, score, sent per candidate
(surr:false-kill-rate audit-rows 5)          ; sampled-reject false-kill rate
```

`sense` is `"min"` (default) or `"max"`; EI / LCB / TS and the prescreen rank
accordingly. In the min sense an LCB prescreen ranks by `-LCB`, so a higher
score always means "more worth evaluating".

## Numerics

- Inputs are standardized per dimension and `y` to zero mean / unit sd, so raw
  genes and token counts share one well-conditioned `K`.
- `ls` (default 0.5, standardized units) is the kernel length scale. It is
  deliberately shorter than the sample range: an over-long length scale makes
  the rbf matrix numerically singular and produces huge `alpha`.
- The Cholesky factorization retries with escalating jitter on a non-positive
  pivot. It also rejects an `alpha` with magnitude > 1e4 — a factorization that
  "succeeded" on a near-singular matrix is silent garbage — and regularizes
  instead.
- Fixed seeds make `surr:ts` and every prescreen reproducible.

## Multi-fidelity evaluation

```aura
(evo:staged-eval cand
  (list (cons "parse+contract" cheap-fn)
        (cons "smoke-10"       mid-fn)
        (cons "holdout-80"     full-fn)))
```

A stage returns `#f` or `(hash "ok" #f)` to fail; a number or
`(hash "ok" #t "score" n)` to pass; a thrown error is a failure. The ladder
stops at the first failure, so a level-1 reject never pays for a holdout run —
`"evaluations"` counts the stage functions that actually ran.

## Racing

```aura
(evo:race cands (lambda (cand instance) score)
           (hash "alpha" 0.05 "instances" insts "batch" 1 "sense" "max"))
```

Survivors are evaluated in rounds over successive instance blocks. A survivor
is dropped when its paired mean is worse than the incumbent's by more than
`z(alpha) * se` — clearly worse, not noise. The result carries the winner, the
final ranking, per-round audit and the evaluation count.

## Acceptance evidence

`tests/suite/surrogate_4943.aura` uses the analytic oracle `f(x) = (x-0.3)^2`:

- GP rbf/matern52 reproduces the sample, predicts the optimum, and the truth
  stays inside 4 sigma wherever the interval is wide; variance is the noise
  floor at a sample and grows between samples.
- kNN fallback predicts within 0.05 of the oracle and reports uncertainty;
  `surr:auto` routing is asserted for tiny/high-dimensional samples.
- EI and LCB prescreens land within 0.15 of the optimum; every candidate has an
  audit row; TS is deterministic per seed; the max sense is covered.
- Staged evaluation runs 1 stage on a level-1 failure (counters prove the later
  stages were never called), and all 3 when every level passes.
- Racing finds the clear winner with fewer than a quarter of the full-grid
  evaluations, honours the min sense, and does not flip on noise.
- The false-kill accounting is checked on a synthetic row set and end-to-end
  against the oracle.

Full-fidelity claims from the issue's acceptance list (HV parity on ZDT1 with a
100 ms delay, >= 40 % fewer full evaluations on aura-harness over 5 seeds) need
the P6 benchmark harness and are reported there; this module ships the
mechanism plus the unit-level contract above.
