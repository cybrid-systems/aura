# std/pso — Particle Swarm Optimization (#2877)

Continuous parameter search backend for [`std/swarm`](swarm.md) (`kind: "pso"`).

## When to use which backend

| Backend | Use when | Individual |
|---------|----------|------------|
| **PSO** | Continuous / real-valued knobs with velocity memory | list of numbers |
| **ABC** | Multi-cand exploit/abandon/scout neighborhoods ([`std/abc`](abc.md)) | list of numbers |
| **grid** | Discrete policy scan, enumerate known bins (Unify-style) | grid point (numbers) |
| **ant** | Rank **mutation operator types** / discrete strategy names via pheromone | type name (string) |

PSO is **not** for ranking string operators — use `kind: "ant"`.  
PSO is **not** a full combinatorial grid — use `kind: "grid"` when you need exhaustive bin coverage.  
For multi-cand abandon/scout without velocity, prefer **ABC**.

## Algorithm (v1)

- Particles: position \(x\), velocity \(v\)
- Personal best \(pbest\), global best \(gbest\)
- Update:
  - \(v ← w\,v + c_1 r_1 (pbest - x) + c_2 r_2 (gbest - x)\)
  - \(x ← \mathrm{clamp}(x + v,\ \mathrm{bounds})\)

**Defaults:** `w=0.7`, `c1=c2=1.4`, `pop=16`.

## API

```aura
(require "std/pso" all:)

;; Short form
(pso:init 2 16 (list (list -5.0 5.0) (list -5.0 5.0)) 42)

;; Full opts
(pso:init-hash (hash "dim" 2 "pop" 16
                     "bounds" (list (list -5.0 5.0) (list -5.0 5.0))
                     "w" 0.7 "c1" 1.4 "c2" 1.4
                     "seed" 42 "parallel" #f))

;; fitness: higher better — maximize -sphere
(define (sphere-fit x)
  (let loop ((xs x) (s 0.0))
    (if (null? xs) (- 0.0 s)
      (loop (cdr xs) (+ s (* (car xs) (car xs)))))))

(let loop ((i 0))
  (if (>= i 30) #t
    (begin (pso:step! sphere-fit) (loop (+ i 1)))))

(pso:best)       ; → best position
(pso:population) ; → all particles
(pso:report)     ; → swarm report hash
```

Or via swarm:

```aura
(require "std/swarm" all:)
(swarm:init (hash "kind" "pso" "dim" 3 "pop" 16
                  "bounds" (list (list 0 64) (list 0 64) (list 1 128))
                  "w" 0.7 "c1" 1.4 "c2" 1.4))
```

## Parallel fitness

`"parallel" #t` (or `swarm:parallel!`) evaluates fitness with **flat**
`fiber:spawn` then main-thread joins — never join inside a worker.

## Apply best

Callers own apply: write policy knobs, `hot-strategy:swap!`, or
`mutate:rebind` from `(pso:best)` — this module only searches.

## Example

`tests/suite/swarm_2874.aura` — sphere minimization via PSO.

## Instances + ask/tell (#4739)

The legacy `pso:init` / `pso:step!` / `pso:best` / `pso:report` /
`pso:export` surface runs over **one default instance**. When a caller has
to gate, parallelise, dedupe or record candidates itself, drive an
instance by hand:

```aura
(define s (pso:make (hash "dim" 3 "pop" 16
                          "bounds" (list (list 0.0 64.0)
                                         (list 0.0 64.0)
                                         (list 1.0 128.0))
                          "seed" 7
                          "vmax" 0.2             ; velocity clamp, fraction of span
                          "boundary" "reflect"))) ; clip | reflect | absorb

(pso:ask s)        ; -> this generation's positions - no evaluation
(pso:tell! s fits) ; fits: one number per position, or #f = rejected/failed
(pso:best s)
(pso:population s)
(pso:report s)     ; kind/gen/best-fit/mean-fit/diversity/rejected
(pso:export s)     ; -> blob
(pso:import blob)  ; -> an independent instance resumed from the blob
```

Semantics:

- **ask/tell split** - `ask` never evaluates and never changes state;
  `tell!` consumes exactly one ask's positions, then advances the
  generation.
- **`#f` fitness** - a rejected or failed candidate: it is counted in
  the report's `rejected` key and never updates a personal best.
- **Instances are independent** - all state (positions, velocities,
  personal bests, PRNG, generation) lives in the instance hash; two
  instances can run interleaved, and `import` resumes a search.
- **`vmax`** clamps |v_i| <= vmax * span_i; **`boundary`** chooses
  `clip` (legacy), `reflect` (velocity flips at the wall) or
  `absorb` (velocity zeroes).

Instance calls are fibre-safe for evaluation on the caller side: only the
caller touches the instance hash, so run `ask` on the coordinator, evaluate
the batch wherever you like, then `tell!` back.
