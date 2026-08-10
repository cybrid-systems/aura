# std/pso — Particle Swarm Optimization (#2877)

Continuous parameter search backend for [`std/swarm`](swarm.md) (`kind: "pso"`).

## When to use which backend

| Backend | Use when | Individual |
|---------|----------|------------|
| **PSO** | Continuous / real-valued knobs (thresholds, cache sizes as reals, kernel hyperparams) | list of numbers |
| **grid** | Discrete policy scan, enumerate known bins (Unify-style) | grid point (numbers) |
| **ant** | Rank **mutation operator types** / discrete strategy names via pheromone | type name (string) |

PSO is **not** for ranking string operators — use `kind: "ant"`.  
PSO is **not** a full combinatorial grid — use `kind: "grid"` when you need exhaustive bin coverage.

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

`examples/swarm_sphere_search.aura` — sphere minimization via PSO.
