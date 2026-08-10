# std/boids — flocking coordination (#2879)

Pure-Aura **Boids-style** rules on abstract state vectors for multi-agent /
strategy populations. **Not** a 3D graphics physics engine, and **not** a
sole global optimizer — use with orch multi-agent or as `std/swarm`
`kind: "boids"`.

## Rules

| Rule | Effect |
|------|--------|
| **Separation** | Steer away from nearby agents (diversity / avoid strategy collapse) |
| **Alignment** | Match average neighbor velocity (shared search direction) |
| **Cohesion** | Pull toward neighborhood centroid (promising region) |

Neighborhood: agents within `radius` (euclidean); if none, fall back to all others.

## Pure API

```aura
(require "std/boids" all:)

(define pos (list (list 0.0 0.0) (list 0.1 0.0) (list 0.8 0.8)))
(define vel (list (list 0.0 0.0) (list 0.0 0.0) (list 0.0 0.0)))

(boids:separation pos 0 0.5)
(boids:alignment pos vel 0 0.5)
(boids:cohesion pos 0 0.5)

(define r (boids:step pos vel
  (hash "sep" 1.5 "align" 0.3 "cohere" 0.2
        "radius" 0.5 "max-speed" 0.2
        "bounds" (list (list -1.0 1.0) (list -1.0 1.0)))))
; r = (list new-positions new-velocities)
(boids:diversity (car r))
```

## Swarm backend

```aura
(require "std/swarm" all:)
(swarm:init (hash "kind" "boids" "dim" 2 "pop" 12
                  "sep" 1.5 "align" 0.2 "cohere" 0.2
                  "radius" 0.4 "max-speed" 0.2
                  "bounds" (list (list -2.0 2.0) (list -2.0 2.0))))
(swarm:step! fitness-fn)  ; moves flock then evaluates fitness
```

## When to use vs other swarm kinds

| Kind | Role |
|------|------|
| **boids** | Coordinate multi-agent diversity + shared direction (not sole optimizer) |
| **pso** | Continuous param search with velocity memory |
| **abc** | Multi-cand exploit / abandon / scout |
| **grid** | Discrete policy scan |
| **ant** | Mutation-type name ranking |

## Acceptance notes

High `"sep"` tends to **increase** `boids:diversity` on clustered starts.
Fit with orch multi-agent / strategy populations; combine with fitness eval
for search, do not treat flocking alone as global optimization.
