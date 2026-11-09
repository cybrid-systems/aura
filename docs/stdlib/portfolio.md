# std/portfolio + std/algo-select + std/hyper-heuristic (#4945)

No Free Lunch (Wolpert & Macready, IEEE TEVC 1997) says no algorithm wins on
every instance: an advantage on one family is paid for on another. Downstream
already measured this — four packing heuristics each won on a different order
class. The library therefore ships the *mechanism*, not a champion: combine
members, select per instance, allocate compute, adapt operators, race configs.

| Module | Role |
|--------|------|
| `std/portfolio` | registry, budgeted runs, performance matrix, VBS/SBS |
| `std/algo-select` | offline (kNN/tree/pairwise) + online (context bandit) selection |
| `std/hyper-heuristic` | FRRMAB operator credit, irace-lite racing, multi-seed gate, parameter adaptation |

References: Kerschke et al., *Automated Algorithm Selection* (2019,
arXiv:1811.11597); MOEA/D-FRRMAB (Li, Fialho, Kwong, Zhang, IEEE TEVC 2014);
irace (Lopez-Ibanez et al., ORP 2016); EoH-S complementary heuristic sets
(arXiv:2508.03082).

## Portfolio

```aura
(require "std/portfolio" all:)
(define pf (portfolio:make (hash "sense" "max")))
(portfolio:add! pf "moead" (lambda (inst ctx) score) (hash "kind" "moo"))
(portfolio:add! pf "wall" (portfolio:external "build/pack" "space=0"))
(portfolio:run pf inst (hash "budget-ms" 8000 "alloc" "equal"))
(portfolio:matrix pf instances (hash "seeds" (list 1 2 3)))
(portfolio:vbs pf m) (portfolio:sbs pf m)
```

A member is a factory `(lambda (instance ctx) score)`; `ctx` carries
`"budget-ms"`, `"member"`, `"instance"` and `"clock"`. `portfolio:external`
runs a command (default runner `command-output`, replaceable with `"runner"`)
and parses the last number on the last output line.

**Compute alignment.** With `"budget-ms"` set, each member runs in its own
fiber with a watchdog that cancels it at its deadline. The audit row records
the budget, the actual time, the status (`ok` / `cancelled` / `error`) and the
score; a cancelled member cannot win. Cancellation is *cooperative* — a fiber
observes it at a safepoint (`fiber:yield`, channel send/recv, `fiber:join`).
A member that spins in pure CPU without yielding cannot be interrupted; give
its inner loop a yield. The AC test asserts cancellation lands within 100 ms
of the budget and the audited time is within 10 % of it.

**Allocation.** `equal` splits the budget and runs all members in parallel;
`successive-halving` re-runs survivors with a growing share and drops the
worse half each round; `ucb` / `thompson` spend fixed slots on bandit pulls.

## Selection

```aura
(require "std/algo-select" all:)
(define s (select:make (hash "model" "knn" "k" 3)))
(select:features-fn! s (lambda (inst) (hash "types" (inst-types inst))))
(define s (select:offline s matrix))     ; matrix from portfolio:matrix
(select:choose s inst 2)                 ; -> two member names
(select:report s)                        ; SBS, VBS, LOO, gap closure, regret
(select:feedback! s inst "member-b" 0.9)
(select:drift? s)
```

Models: `knn` (k nearest feature-space neighbours), `tree` (depth-2 split
tree over the numeric features) and `pairwise` (pairwise wins among the
nearest neighbours). `select:report` re-fits on every leave-one-out fold and
reports the SBS→VBS gap closure; the chosen member is scored by its *actual*
performance, not by the model's internal score.

Online feedback is a context bandit: the feature vector is bucketed against
the training medians, and each bucket keeps per-member counts and sums.
`select:choose` prefers a bucket with evidence (mean + UCB bonus) and falls
back to the offline ranking.

**Non-stationary streams.** Every update decays the bucket's cells
(`"decay"`, default 0.9), so stale pulls fade instead of being averaged in
forever — the effective window is the decay half-life. `"warm-start" #t`
lets each member be tried once before exploitation; without it a bandit can
lock onto the offline favourite and never sample the arm a regime change
made good. `select:drift?` combines two signals: the total-variation
distance between the recent bucket mix and the training mix, and a
reward-window shift (newest half vs older half, in standard deviations).
A flagged drift decays the old cells twice and doubles the UCB coefficient
for the next choice.

## Hyper-heuristic

```aura
(require "std/hyper-heuristic" all:)
(define hh (hh:make (hash "low-level" (list "crossover" "mutate" "ls")
                          "credit" "frr" "window" 50 "decay" 0.5)))
(define step (hh:next hh))          ; (op . next-hh)
(hh:credit! (cdr step) (car step) reward)
(tune:gate cand-seeds base-seeds (hash "alpha" 0.05))
(tune:race configs (hash "instances" is "seeds" 5
                         "evaluate" (lambda (cfg inst seed) score)))
```

`frr` is the FRRMAB rule: an exponentially decayed fitness rank ratio plus
a UCB bonus, with every operator tried once before credit exists. `pm`
(probability matching) and `ap` (adaptive pursuit) are the alternatives;
both keep a small floor so a currently-unlucky operator can recover.

**Winner's curse.** `tune:gate` is a paired one-sided t-test on seed-level
results with critical values from a small Student-t table. A candidate is
KEPT only when its mean improvement is positive *and* exceeds the noise at
`alpha`. Over 400 pure-noise trials the measured false-KEEP rate is 5.0 % at
alpha = 0.05 and 0.25 % at alpha = 0.01 (the suite asserts both).
`tune:race` eliminates a configuration only when the gate says the incumbent
is better, so a single lucky seed cannot delete a candidate or crown one.

`tune:adapt!` implements the 1/5 success rule and a success-history rule for
online parameter control.

## Acceptance evidence

`tests/suite/portfolio_select_4945.aura`:

- Three synthetic algorithms each win on one feature regime; kNN and tree
  selection close 100 % of the SBS→VBS gap and pick the right member for
  every regime; pairwise selection picks correctly too.
- Equal allocation cancels an over-budget member within 100 ms and audits
  actual vs budget within 10 %; halving and UCB/Thompson pick the best member
  while spending fewer pulls on the weak one. A successive-halving slot
  cancels its over-budget spinner in every round, audits within 10 % of the
  slot, and the run spends no more than 110 % of its budget.
- FRRMAB with one bad operator: bad usage 1.5 % (limit 10 %), mean reward
  0.966 of the best single operator (limit 0.95). PM and AP also starve it.
- MOEA/D-FRRMAB on ZDT1 (2 objectives, dim 6, 1,560 evaluations): the bandit
  chooses the variation operator per subproblem and credits the scalar
  improvement over the parent. The random operator is pulled 116/1554 times
  (7.5 %, bar 10 %), and the bandit's hypervolume is 1.041 against 1.052 for
  the best fixed operator (98.9 %, bar 95 %); the random operator alone
  reaches 0.017.
- Non-stationary stream on the same context bucket (member a pays, then b):
  the decayed, warm-started bandit accumulates regret 29 against the fixed
  SBS's 100 (71 % lower, bar 30 %), plays the new best member from the first
  step after the drift and in 20 of the next 25 pulls, and `select:drift?`
  reports the reward shift even though the context mix did not change.
- The multi-seed gate rejects noise winners (5.0 % at alpha 0.05) and accepts
  a real improvement; `tune:race` finds the best config with fewer
  evaluations than the grid.
- The EoH-style admission mock: a complementary member raises VBS and is
  KEPT; a redundant member leaves VBS unchanged and is DROPPED, both with
  the measured delta recorded.

The aura-harness held-out study (40 % fewer full evaluations at the same
success) is measured by std/surrogate's cheap-first wiring and reported in
that module's evidence section.

## Limits

- Portfolio members share the process; isolation is fiber + overlay, not an
  OS sandbox. Use `portfolio:external` for an external kernel.
- Cancellation is cooperative (see above).
- The context bandit is deliberately small (bucketed features), matching the
  cardinality of instance descriptors; a linear LinUCB is future work.
