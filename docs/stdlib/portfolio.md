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
back to the offline ranking. `select:drift?` compares the recent bucket mix
with the training mix (total-variation distance).

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
  while spending fewer pulls on the weak one.
- FRRMAB with one bad operator: bad usage 1.5 % (limit 10 %), mean reward
  0.966 of the best single operator (limit 0.95). PM and AP also starve it.
- The multi-seed gate rejects noise winners (5.0 % at alpha 0.05) and accepts
  a real improvement; `tune:race` finds the best config with fewer
  evaluations than the grid.
- The EoH-style admission mock: a complementary member raises VBS and is
  KEPT; a redundant member leaves VBS unchanged and is DROPPED, both with
  the measured delta recorded.

The full acceptance items that need the benchmark harness (ZDT1/HV parity,
aura-harness held-out success with 40 % fewer evaluations, the non-stationary
regret study and spec-vs-actual compute deviation across real members) are
P6 report material; this module ships the mechanisms and the unit contracts.

## Limits

- Portfolio members share the process; isolation is fiber + overlay, not an
  OS sandbox. Use `portfolio:external` for an external kernel.
- Cancellation is cooperative (see above).
- The context bandit is deliberately small (bucketed features), matching the
  cardinality of instance descriptors; a linear LinUCB is future work.
