# std/edit-search + std/race — M4 rehearsal search (#5413)

`aura-agents` M4 rehearses N candidates in one live process: N fibers, each
running a different edit of the program, and a driver that has to pick the
nodes worth rehearsing and stop the ones that cannot win. Two things were
missing for that on `main`; this page is both. Neither is a new algorithm.

| Module | Role | Reuses |
|--------|------|--------|
| `std/edit-search` | tree search over AST **edit sequences** under a **stage gate** | `prop:tree` (MCTS-AHD) and the `std/search` beam |
| `std/race` | successive halving / bandit cancellation over fibers that **already run** | `fiber:cancel`, the `std/search` bandit ledger, the `std/portfolio` result shape |

`std/portfolio` allocates a budget across member *factories*: it spawns each
member itself and learns a score only when the member returns. `std/search`
`beam` searches a tree that is supplied whole. `prop:tree` is a proposer
wrapper whose children come from an inner proposer. None of them gates
expansion on a milestone, and none of them races a fiber somebody else
spawned.

## The stage gate

A node is an edit sequence: the ordered list of edit descriptors applied to
one base program. A child appends one edit that the **current** stage
permits. "no `fight` edit until the `move` contract is met" is therefore a
gate on expansion, re-checked every time a node is expanded. The tree is
never materialised up front.

```aura
(require "std/edit-search" all:)

(define spec
  (hash "stages" (list "move" "fight" "boss")
        "edits" (list (hash "name" "walk"  "stage" "move" "op" 'mutate:rebind "args" (list "step" "(lambda (e dx) ...)"))
                      (hash "name" "turn"  "stage" "move" "op" 'mutate:tweak-literal "args" (list "speed" 2))
                      (hash "name" "swing" "stage" "fight" "op" 'mutate:rebind "args" (list "attack" "(lambda (e t) ...)")))
        ; apply the sequence on a snapshot, measure, restore
        "evaluate" (lambda (seq)
          (let ((sid (mutate :snapshot "edit-search")))
            (for-each apply-edit seq)
            (let ((r (run-contract-tests)))
              (mutate :restore sid)
              r)))
        "max-depth" 3
        "budget" 60))

(define r (edit-search:run spec))
(hash-ref r "best-seq")     ; the edit descriptors, best sequence found
(hash-ref r "best-score")
(hash-ref r "ranking")      ; ((name ...) . info) for every evaluated sequence

(edit-search:legal spec (list "walk"))   ; the edits that may follow a sequence
```

**Edits.** A descriptor is any value with a `"stage"` and, when it is a
hash, a unique `"name"` (default: its index). The rest of the hash is the
caller's: `op` / `args` is a convention, not a requirement.

**evaluate** is `(lambda (seq) ...)` with the list of descriptors. It returns
a number, or a hash with `"score"` and optionally:

- `"passed"`: the stages whose contract `seq` meets (a list of names);
- `"ok"`: `#f` for a broken candidate — it is ranked with its score and never
  expanded. A thrown error is the same verdict.

A number, or a hash without `"passed"`, inherits the parent's `passed`.
Higher is better. A sequence is evaluated once per run.

**Default rule.** An edit of stage `s` may follow `seq` once every stage
before `s` in `"stages"` is in `seq`'s `passed`. The base program's own
milestones are `"root-passed"`. With no `"stages"` every edit is legal.
`"rule"` `(lambda (edit seq info) ...)` **replaces** the rule (any non-`#f`
answer is legal); `info` is the parent's evaluation info.

## Engines

`"engine"` selects one; the result has the same shape either way.

| engine | what it is | promises |
|--------|-----------|----------|
| `"mcts"` (default) | `prop:tree` with this module as its **inner proposer**: UCT with constant `c`, every node widened to all of its legal children before the tree descends | best-first within `"budget"` distinct evaluations; finds the gated optimum of a small tree |
| `"beam"` | the `std/search` beam over a tree this module materialises one generation ahead | a bounded enumeration: `"width"` offers per generation in offer order (`"prior"` `(lambda (edit seq info) ...)` reorders them); finished sequences leave the beam and keep their real score in the ranking |

`"mcts"` is an engine name here, not a `std/search` kind: `(search:init
"mcts")`, `"cma-es"` and `"bayes-opt"` still refuse.

The `prop:tree` wrapper needed two decisions worth knowing:

- a node with fewer legal children than the slot `prop:tree` asks for
  answers with a **stop** (it repeats the parent's sequence), because
  `prop:tree` always stores a child and an empty answer would leave a node
  with no proposal. A stop step evaluates nothing and is not backed up; the
  stop node, and a node with no legal child at all, get a UCT value no score
  reaches so the tree stops asking them and their siblings get explored;
- the run ends at `"budget"` distinct evaluations, or when the tree spends
  `32 + 8 × edits` consecutive steps without a new sequence.

The beam is **not** a best-first search: `std/search` scores a path when it
is evaluated, keeps only leaves forward, and prunes to `"width"` children a
generation before any of them is scored. `tests/suite/edit_search.aura`
shows the consequence (a width-2 beam never sees the `fight` edits unless a
prior offers them). Use it to enumerate a small gated tree; use `"mcts"` to
search.

Result keys: `engine`, `best` (last edit of the best sequence), `best-name`,
`best-seq`, `best-score`, `best-info`, `evaluated`, `ranking`, `state` (the
engine's own value: the `prop:tree` hash, or the `std/search` state).

## Racing fibers that already run

```aura
(require "std/race" all:)

(define cells (hash))
(define (spawn-trial name body)
  (hash-set! cells name (atomic:counter 0))
  (fiber:spawn body))

(define r (race:make
  (hash "score" (lambda (name) (atomic:counter-value (hash-ref cells name))))))
(race:enter! r "a" (spawn-trial "a" trial-a))     ; fibers that already run
(race:enter! r "b" (spawn-trial "b" trial-b))
(race:enter! r "c" (spawn-trial "c" trial-c))

(race:halving r (hash "round-ms" 200 "rounds" 3))
; or
(race:bandit r (hash "slot-ms" 100 "slots" 12 "policy" "ucb"))

(hash-ref (race:result r) "winner")   ; name of the best arm
(race:arms r)                         ; ((name mean samples) ...)
```

`"score"` is `(lambda (name) ...)` and answers an arm's **current**
intermediate score: a number, `#f` (none yet), or `(hash "score" n "done"
#t)` to say the trial finished by itself. It runs on the race driver's
thread; a trial publishes through its own cell (`atomic:counter`, a channel),
never by writing into the race.

| policy | rule |
|--------|------|
| `race:halving` | each round keeps the better half of the live arms (never fewer than `"keep"`) and cancels the rest; `"rounds"` defaults to what halves the field down to `"keep"` |
| `race:bandit` | one slot at a time. Reward = "in the top half of the live arms this slot". An arm is cancelled once its Hoeffding upper bound on that share (`delta`, default 0.05) is below another arm's lower bound; at the end the rest are cut down to `"keep"` by lower bound. The ledger is a `std/search` bandit — one pull per arm per slot, the policy (`ucb` / `thompson`) picks the pull order — and travels in the result as `"ledger"` |

Budget accounting is `std/portfolio`'s shape. `(race:result r)` carries
`rows` (`member` / `fid` / `budget-ms` / `actual-ms` / `status` / `score` /
`samples` / `value` / `cut`), `winner`, `winner-score`, `rounds`,
`budget-ms`, `spent-ms`, and `arms` as `(name mean n)` rows like a
`portfolio:run` with `ucb` / `thompson` allocation. A cut arm cannot win.

`"join"` (default `#t`) waits for the arms still live and reports their
results; `#f` leaves them running for `(race:join r name)`. `(race:cancel r
name)` stops one arm and reclaims its fiber.

Cancellation is cooperative ([fiber-cancel.md](fiber-cancel.md)): a trial
must reach a safepoint (`fiber:yield`, a channel, a join) to observe it. A
cut arm is cancelled **and joined** before the policy goes on, so its thread
is reclaimed; a body that never yields blocks the race in that join.

**Waiting.** There is no kernel sleep yet: the default timed wait is a yield
loop on the steady clock (the `orch-wait-sec` shape) and burns its thread
for the length of the round. A host with a timer injects `"sleep"`
`(lambda (ms) ...)`; the suite injects one to stay deterministic.

## Recommended composition for a rehearsal

1. `edit-search:run` (engine `"mcts"`) ranks the gated sequences; its
   `"evaluate"` applies each sequence on a `(mutate :snapshot ...)` and
   restores it.
2. Take the top of `"ranking"`, spawn one fiber per candidate under its own
   COW overlay (`docs/m4-live-fork.md`), and have each body publish its
   progress in an atomic cell.
3. `race:halving` (or `race:bandit`) over those fibers with `"score"`
   reading the cells.
4. `evo:staged-eval` / `std/gate` decide the survivor's contract; the
   driver commits that overlay.

## What changed in std/search

- `(search:arms state)` returns the bandit ledger as data:
  `((arm pulls keeps) ...)`; another kind answers `()`.
- The UCB pick leaked a `(score . arm)` pair once every arm had a pull, so
  no row matched the pick and the ledger froze at one pull per arm.
  `search_bandit.aura` did not see it (it stops at "the best arm wins");
  `search_clonal.aura` now pins twelve pulls.

## Acceptance evidence

- `tests/suite/edit_search.aura`: the gate at the root and after a
  milestone, the gated optimum and the budget under `"mcts"`, offer order
  and prior under `"beam"`, no stages, a custom rule, a broken candidate
  (reported, never expanded), the refusal messages, and the reserved
  `std/search` kinds still refused.
- `tests/suite/fiber_race.aura`: halving rounds and budget rows, the cancel
  condition inside a cut trial, a self-finished trial never cut, bandit
  elimination with its ledger, thompson, equal arms, `sense "min"`,
  `enter!` / `cancel` edges, and one round against real atomic counters.
- `tests/suite/search_clonal.aura`: `search:arms` and the UCB ledger.

## Limits

- No new search kind and no AgenticPD system: stage *gating* only; the
  caller owns the edit vocabulary, the evaluation and the commit.
- `edit-search` maximises a deterministic score; it has no noise model.
- `race:bandit`'s bounds are on a Bernoulli "top half" indicator; scores
  that oscillate around the median are not separated, and the final cut
  then falls back to the stable order.
- A race sees what `"score"` reports; there is no `fiber:done?` to read.
