# std/abc — Artificial Bee Colony (#2878)

Multi-candidate **exploit / abandon / scout** search backend for
[`std/swarm`](swarm.md) (`kind: "abc"`).

## When to use which backend

| Backend | Use when |
|---------|----------|
| **ABC** | Multi-cand structure/param neighborhoods: exploit winners, abandon stagnant sources after `limit` fails, scout random |
| **PSO** | Smooth continuous params with velocity memory (inertia + social pull) |
| **grid** | Discrete exhaustive / windowed bin scan |
| **ant** | Rank **mutation operator type names** (strings), not continuous vectors |

ABC is closer to denseness multi-cand explore/exploit than pure grid.

## Roles

| Bee | Behavior |
|-----|----------|
| **Employed** | Each food source tries one neighbor; keep if fitter |
| **Onlooker** | Roulette pick by fitness; same local search |
| **Scout** | If trials > `limit`, replace source with random individual |

## Neighbor modes

| `"neighbor"` | Rule |
|--------------|------|
| `"gaussian"` (default) | All dims small Gaussian-like uniform noise (10% span) |
| `"classic"` | One random dim; \(\phi \in [-1,1]\) mix with another source |

## API

```aura
(require "std/abc" all:)

(abc:init 2 12 (list (list -5.0 5.0) (list -5.0 5.0)) 5 42)
;; or
(abc:init-hash (hash "dim" 2 "pop" 12 "limit" 5
                     "neighbor" "gaussian"
                     "bounds" (list (list -5.0 5.0) (list -5.0 5.0))
                     "seed" 42))

(define (sphere-fit x)  ; higher better
  (let loop ((xs x) (s 0.0))
    (if (null? xs) (- 0.0 s)
      (loop (cdr xs) (+ s (* (car xs) (car xs)))))))

(let loop ((i 0))
  (if (>= i 25) #t
    (begin (abc:step! sphere-fit) (loop (+ i 1)))))
(abc:best)
```

Via swarm:

```aura
(swarm:init (hash "kind" "abc" "pop" 12 "limit" 5
                  "neighbor" "gaussian" "dim" 2
                  "bounds" (list (list -5.0 5.0) (list -5.0 5.0))))
(swarm:step! fitness)
```

## Defaults

`pop=12`, `limit=5`, `neighbor="gaussian"`.

## Apply best

Callers own apply (policy knobs / `hot-strategy` / `mutate:rebind`).
