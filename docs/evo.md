# Self-evolution guide (`aura evo`)

The v0.2 search stack is a small set of composable values: candidates and
lineage, an ask/tell search handle, a Pareto archive, a quality-diversity map,
proposers, surrogates and a portfolio. Every step is written to one append-only
log, so a run can be replayed, compared and reported with one command (#4938).

This page is the guide: what each algorithm is for, its API, what it costs,
what it is allowed to touch, and the paper it comes from.

## Recording

The runtime appends one JSON object per line to the file named by
`AURA_EVO_LOG` (`aura run` inside a project defaults it to
`<project>/.aura/evo/log.jsonl`; set it empty to turn recording off).

Schema v=1 fields: `id`, `parent`, `run`, `seq`, `ts_ms`, `kind`,
`target`, `decision`, `reason`, `evidence` (any JSON value), `old`,
`new`, `dur_ms`. A candidate's `parent` is the worldline tip when it was
recorded — the last `KEEP` in the same run, or the explicit parent argument.

Schema v=2 (the evolution modules) adds, after `dur_ms`: `algo`, `gen`,
`slot`, `search`, `candidate`, `parents` (array; null for a root),
`behavior`, `objectives`, `constraints`. Only supplied members are written,
always in that order, so identical runs stay byte-identical (timestamps and
durations excepted).

```scheme
(evo:record! "strategy-v7" "KEEP" "holdout 0.71 >= 0.70"
             (hash "score" 0.71) new-src 12)
```

Safety: the path is operator configuration, never program-chosen. The log is
an audit trail, not a program file write. `evo:trial` runs apply+evaluate in a
child workspace and refuses to fall back to mutating the live worldline.

## Reading a run

```
aura evo log [--json] [--all] [--run ID|last] [--log FILE]
aura evo log --front        # only non-dominated candidates (needs objectives)
aura evo log --lineage ID   # the ancestor chain of one record, root → ID
aura evo report --html DIR [--all] [--run ID|last] [--log FILE]
```

`evo log` prints decision, reason, evidence, diff size, duration and parent.
The run header counts every decision, so the numbers add up to the record
count (`270 records: 30 PROPOSE, 238 SCORED, 2 KEEP, 0 ROLLBACK`).
`--front` keeps the candidates that are non-dominated on their objective
vector (2+ numeric objectives) and prints that same header for the rows it
kept. `--lineage ID` keeps the records on the ancestor chain, which is how a
surprising KEEP is traced back.

`ID` is a record id (`<run>#<seq>`, the form in the log's `id` field), the
`#<seq>` suffix printed in the id column, or a candidate name (the v2
`candidate` member, or the `target` column). A record id wins over a
candidate that happens to use the same text. When the log has records and
none of those forms match, the command prints `no record with id '…'` and
exits non-zero. An empty log still prints `(no evolution records)` and
exits 0.

`std/evo-core` writes each record's `parent` as the previous record's id
(empty on the first record of a search). The v2 `parents` array is still
the candidate ids the proposer named. The report resolves either form, draws
one edge per distinct resolvable link (including a second recombination
parent), and counts links that match nothing instead of dropping them.

`evo report` writes a self-contained page (no network) plus standalone SVGs:

| file | content |
|---|---|
| `index.html` | timeline, score curve, Pareto scatter, HV curve, QD map, lineage, candidates |
| `pareto.svg` | objective-space scatter with the non-dominated front highlighted; the first two objectives are x/y and a third is the point size |
| `hv-curve.svg` | cumulative hypervolume of the front after each evaluated candidate |
| `qd-heatmap.svg` | behaviour-space heat map from the `behavior` member |
| `lineage.svg` | parent tree, multi-parent edges included; unresolved parent links are counted in the page |

The report folds candidates by generation (outer `<details>`) and each
candidate's evidence/diff (inner `<details>`), so a 240-candidate run is still
readable. Records that carry a `surrogate` member (or an evidence object with
`mean`/`var`/`score`/`sent`) are listed in the **Surrogate pre-screen**
section — the cheap screening step is auditable, not hidden.

## Algorithms

### std/evo-core — the base (#4939)

Candidates, lineage, the ask/tell search handle, worldline-isolated trials,
deterministic RNG and the v=2 log. Every other module builds on it; it contains
no search algorithm.

```scheme
(define s (evo:search (hash "algo" "moead" "seed" 42 "budget" 500 "pop" 8
                            "objectives" (list (cons "loss" "min")
                                               (cons "cost" "min")))))
(evo:run! s evaluate)          ; ask → evaluate → tell until budget
(evo:report s)                 ; archive, spent, budget-left, tip
```

*Use when* you need the audit trail and lineage, not a particular algorithm.
*Cost*: one record per proposal/evaluation. *Safety*: `commit!` and
`evo:trial` are the only paths that touch the live AST; a trial never does.

### std/pareto + std/moead — multi-objective (#4940)

Pareto archive (crowding / HV / epsilon trimming), constrained commit gate,
non-dominated sort, crowding distance, exact 2/3-objective hypervolume, IGD,
NSGA-II; MOEA/D with Tchebycheff/PBI/weighted-sum decomposition and
Das-Dennis weights.

```scheme
(define a (pareto:archive (hash "dirs" (list "min" "max") "cap" 64)))
(pareto:insert! a cand (list loss accuracy))
(pareto:front a) (pareto:hypervolume front ref) (pareto:report-svg front)
```

*Use when* there is no single scalar score, or the trade-off itself is the
result. *Cost*: archive maintenance is O(front) per insert. *Safety*:
`pareto:accept?` is a commit gate; the archive is a plain value.
Papers: NSGA-II (Deb et al., 2002), MOEA/D (Zhang & Li, 2007).

`moead:make`'s `"operator"` is `"de"`, `"de-explore"`, `"sbx"`, `"rand"`, a
proposal procedure, or `(hash "choose" f)`. Any other string is an error that
names that set — it is not silently treated as DE. `"rand"` is uniform random
sampling inside the bounds, not differential evolution (before v0.2 the same
spelling fell through to DE). Choosing `"rand"` appends an `OPERATOR` record
to the evolution log when `AURA_EVO_LOG` is set, and `moead:report` includes
`operator-meaning`.

`std/swarm`'s multi-objective surface is a thin delegation to this module
(#4740/#4940): vector fitness goes through `pareto:dominates?`,
`pareto:hypervolume` and the Pareto archive; swarm contributes the population
loop only.

### std/qd — quality diversity (#4941)

MAP-Elites over a descriptor grid or a fixed-seed CVT, novelty (k-NN distance),
the island model (ring migration, stale-deme restart, migration recombination),
DGM-style parent selection and structural de-duplication of programs.

```scheme
(define m (qd:map-elites (hash "descriptors" (list (list "tokens" 0 20000 10)
                                                  (list "latency" 0 5000 10))
                              "quality" "success")))
(qd:insert! m cand success (list tokens latency))
(qd:coverage m) (qd:qd-score m) (qd:novelty m behaviour 3)
(define s (qd:islands (hash "n" 4 "pop" 12 "migrate-every" 5 "reset-worst" #t)))
(qd:islands-run! s evaluate 30) (qd:islands-best s)
```

*Use when* one best answer collapses the search: keep "different good"
solutions alive. *Cost*: one evaluation per candidate, plus O(pop) per
generation for selection. *Safety*: pure values; `qd:struct-hash` refuses a
renamed/reformatted duplicate before it reaches the evaluator.
Paper: MAP-Elites (Mouret & Clune, 2015); island model in FunSearch (Romera-
Paredes et al., 2024).

**Trap benchmark (10 seeds, equal budget).** Six 3-bit deceptive blocks
(best local value 2/block at all-zeroes, global 18 at all-ones), 48 evaluations
per generation, 12 generations:

| seed | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | hits |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 islands (pop 12) | 17 | 17 | 15 | 16 | 16 | 16 | 16 | **18** | **18** | **18** | **3/10** |
| 1 population (pop 48) | 16 | 16 | 16 | 17 | 16 | 17 | 17 | 16 | 16 | 14 | **0/10** |

At 20 generations the same comparison is 5/10 vs 2/10. Migration recombines
the elites of different demes and a stale deme is restarted, so the island
model escapes the deceptive basin that the panmictic population converges on.

### std/proposer — proposal layer (#4942)

EoH / ReEvo / MCTS-AHD / EoH-S proposers as plain callables; mock and grammar
proposers are reproducible from `(seed, gen, slot)`; a scope guard limits which
defines a proposal may rewrite.

*Use when* the mutation operator is an LLM or a grammar rather than a numeric
perturbation. *Cost*: dominated by the proposer call (often seconds). *Safety*:
proposals are data until `commit!`; the scope guard runs before apply.
Paper: EoH (Liu et al., 2024), ReEvo (Ye et al., 2024).

### std/surrogate — cheap pre-screening (#4943)

GP (RBF / Matérn 5/2) and kNN surrogates, EI / LCB / TS acquisition, a
`surr:prescreen` split with a full audit table and spot-check sampling,
multi-fidelity staged evaluation and a statistical race.

```scheme
(define p (surr:prescreen (surr:fit! (surr:gp (hash)) X y) cands 5
                          (hash "acq" "ei" "audit-sample" 0.2)))
(hash-ref p "selected") (hash-ref p "audit") (surr:false-kill-rate (hash-ref p "audit") 5)
```

*Use when* one evaluation is expensive (a build, a benchmark, a model call).
*Cost*: O(n²) fit for the GP. *Safety*: rejected candidates are sampled and the
false-kill rate is reported instead of hidden.
Paper: Jones et al. (1998) for EI; Srinivas et al. (2010) for GP-UCB.

### std/portfolio, std/algo-select, std/hyper-heuristic (#4945)

Budgeted portfolio runs (equal/halving/UCB/Thompson), per-instance algorithm
selection, operator credit (FRRMAB), irace-lite racing and a multi-seed t-gate.

*Use when* no single algorithm wins everywhere (the no-free-lunch setting).
*Cost*: a portfolio runs several members, so budget the wall clock explicitly.
*Safety*: members run through the same trial isolation as everything else.

## Projects

```
aura new evolve      # live AST: KEEP / ROLLBACK in the terminal
aura new evolve-moo  # multi-objective + QD demo (mock proposer, no LLM, no network)
aura new agent       # orchestrated workflow
```

`evolve-moo` runs 240 mock proposals in seconds, prints the Pareto front size
and QD coverage, writes `out/pareto.svg` and `out/qd.svg`, and logs every
candidate so `aura evo report --html out/report` shows the scatter, the HV
curve, the heat map and the lineage. Measured install → report times and the platform
matrix are in [install.md](install.md#from-the-install-line-to-a-report) (23–55 s on
arm64/Ubuntu 26.10; the Ubuntu 24.04 x86_64 and arm64 cells are gated at
≤600 s in CI).

## Migration: std/evolve is deprecated

`std/evolve` (`evolve-strategy`) is a regex edit of a prompt body and will be
removed in v0.3 (#4938). Requiring it now prints a deprecation warning that
points here. Replacements:

| old | new |
|---|---|
| `(evolve-strategy name)` | `(require "std/evo-core" all:)` + a search handle with your evaluate/apply |
| hand-rolled keep/rollback | `evo:search` + `commit!` (records KEEP/ROLLBACK) |
| scalar fitness only | `std/pareto` archive or `std/qd` map |
| eyeballing progress | `aura evo log` / `aura evo report` |
