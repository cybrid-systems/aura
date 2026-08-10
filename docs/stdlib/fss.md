# std/fss — Fish School Search (#2880)

Feeding-driven continuous optimizer for [`std/swarm`](swarm.md)
(`kind: "fss"`). Complements PSO (always the same velocity rules) with a
**volitive** step scale: expand exploration when the school stagnates,
contract when total fitness improves.

## When to use which backend

| Backend | Use when |
|---------|----------|
| **FSS** | Load-adaptive control: expand search when stuck, contract when improving |
| **PSO** | Smooth continuous params with inertia + social pull |
| **ABC** | Multi-cand exploit / abandon / scout after `limit` fails |
| **grid** | Discrete bin / policy scan |
| **ant** | Mutation-type name ranking |
| **boids** | Multi-agent diversity + alignment (coordination, not sole optimizer) |

## Core ideas (v1 simplified FSS)

| Phase | Behavior |
|-------|----------|
| **Individual** | Local swim: random step scaled by `step × volitive × span` |
| **Feeding** | Weight ↑ when a fish improves fitness |
| **Collective** | Drift toward weighted barycenter of the school |
| **Volitive** | If sum-fitness improved → contract volitive; else expand |

`report` keys include `"volitive"` (exploratory scale) and `"step"` (base step).

## API

```aura
(require "std/fss" all:)

(fss:init 2 16 (list (list -5.0 5.0) (list -5.0 5.0)) 0.1 42)
;; or
(fss:init-hash (hash "dim" 2 "pop" 16 "step" 0.1
                     "bounds" (list (list -5.0 5.0) (list -5.0 5.0))
                     "seed" 42))

(define (sphere-fit x)  ; higher better
  (let loop ((xs x) (s 0.0))
    (if (null? xs) (- 0.0 s)
      (loop (cdr xs) (+ s (* (car xs) (car xs)))))))

(let loop ((i 0))
  (if (>= i 25) #t
    (begin (fss:step! sphere-fit) (loop (+ i 1)))))
(fss:best)
(fss:volitive)   ; grows under flat/stagnant fitness
```

Via swarm:

```aura
(swarm:init (hash "kind" "fss" "pop" 16 "step" 0.1 "dim" 2
                  "bounds" (list (list -5.0 5.0) (list -5.0 5.0))))
(swarm:step! fitness)
(hash-ref (swarm:report) "volitive")
```

## Defaults

`pop=16`, `step=0.1`, volitive starts at `1.0` (range roughly 0.25–4.0).

## Apply best

Callers own apply (policy knobs / `hot-strategy` / `mutate:rebind`).
