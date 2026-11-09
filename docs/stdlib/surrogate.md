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

- Cheap-first budget: a 20 % forwarded set keeps the optimum within 0.05 and
  lands within 0.001 of the full-grid best value, i.e. the cheaper screen is
  not buying its saving on the answer.

## aura-harness evidence (5 seeds)

The held-out evaluation reduction is measured on the downstream harness
(`downstream/aura-harness`, branch v0.2-evo), not in this suite. Two cheap
stages sit in front of the full evaluation:

1. free-form proposals (MOEA/D offspring and grammar mutations) are ranked by
   `surr:prescreen` (GP/kNN, EI) and only the top 40 % are evaluated;
2. the memetic polish sweep is cheap-first: every coordinate probe is scored on
   one held-out stream, the best probe per gene by distance to the acceptance
   point is kept, and only `--polish-full` (16 of 84) probes pay the full
   5-stream evaluation.

```sh
# no surrogate
python3 -m aura_harness.evo --seeds 0 1 2 3 4 --gens 10 --ask 10 --propose 4 \
  --H 6 --T 5 --n 400 --polish-rounds 2 --polish-cap 84 --jobs 5 \
  --out results/evo/moead-nosurr --tag nosurr
# surrogate
python3 -m aura_harness.evo --seeds 0 1 2 3 4 --gens 10 --ask 10 --propose 4 \
  --H 6 --T 5 --n 400 --polish-rounds 2 --polish-cap 84 --surrogate \
  --max-gens 10 --polish-full 16 --jobs 5 \
  --out results/evo/moead-surrogate --tag surrogate
```

Full evaluations until the front first dominates the gated baseline and meets
the 10 % token cut (the issue's "same held-out success" point), per seed:

| arm | 0 | 1 | 2 | 3 | 4 | total | reduction |
|---|---|---|---|---|---|---|---|
| no surrogate | 291 | 287 | 130 | 132 | 303 | 1143 | — |
| prescreen only | 252 | 22 | 103 | 104 | 180 | 661 | 42.2 % |
| prescreen + cheap-first polish | 71 | 22 | 41 | 45 | 58 | 237 | **79.3 %** |

All three arms reach the target on all five seeds. The first cut alone
already clears the 40 % bar on this tree; the second cut (`--polish-full
16`, versus `0` for prescreen-only) takes it to 79.3 %, and all full
evaluations fall from 1,706 to 429. Counting a cheap evaluation (1 seed x
200 tasks) as half a full one (5 seeds x 400 tasks), simulated compute
falls from 2,807 to 1,347 full-equivalents, so the saving is not bought by
moving work into the cheap stage. An earlier 27.7 % measurement came from
a harness revision whose `run.json` records a `polish_values` field that
does not exist in HEAD; that exact wiring is not reproducible, so the
three arms above are the record.

Front quality, measured against `[0.40, 9800, 250]` as a ratio to the
no-surrogate front: prescreen-only 0.967 mean (1.063 / 0.761 / 0.981 /
0.984 / 1.047), cheap-first 0.846 mean (0.973 / 0.727 / 0.829 / 0.845 /
0.856). Goal-directed polish converges on the acceptance knee, so the
token/latency wings lose hypervolume even though success and the token cut
are met on every seed; prescreen-only is the choice when front spread
matters more than the evaluation budget. The HV-parity item is the
separate ZDT1 benchmark.

The numbers live in `results/evo/moead-surrogate/results.md`,
`summary.json`, `prescreen-only.json` and `baseline-nosurr.json` in that
repository.
