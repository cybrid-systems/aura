# std/swarm — pluggable population search (Issues #2874 / #2875)

Pure-Aura control-layer module next to `std/ant`. Callers supply **fitness**
and own **apply** (policy knobs, `hot-strategy:swap!`, `mutate:rebind`, …).
This module only searches individuals.

## Require

```aura
(require "std/swarm" all:)
```

## API (v1 — #2875)

| Form | Role |
|------|------|
| `(swarm:init opts-hash)` | Reset state; opts: `kind`, `pop`, `dim`, `bounds`, `seed`, `bins`, `parallel` |
| `(swarm:step! fitness-fn)` | One generation; `fitness-fn`: individual → number (**higher better**) |
| `(swarm:best)` | Best individual so far |
| `(swarm:population)` | Current population (list of individuals) |
| `(swarm:report)` | Hash snapshot (stable schema below) |
| `(swarm:export)` | JSON-ish string for journals |
| `(swarm:parallel! flag)` | Toggle flat fiber fitness fanout |
| `(swarm:kind)` / `(swarm:gen)` | Current backend / generation counter |
| `(swarm:help)` | One-line help |

### `opts-hash` keys

| Key | Default | Meaning |
|-----|---------|---------|
| `"kind"` | `"grid"` | `"grid"` \| `"pso"` \| `"ant"` \| `"abc"` \| `"boids"` \| `"fss"` |
| `"dim"` | `1` | Parameter dimension |
| `"pop"` | `8` (`16` for pso) | Population / window size |
| `"bounds"` | `((-1.0 1.0))` | Per-dim `(lo hi)` lists |
| `"bins"` | `8` | Discrete bins per dim (grid) |
| `"w"` / `"c1"` / `"c2"` | `0.7` / `1.4` / `1.4` | PSO coefficients (#2877) |
| `"ops"` / `"evaporate"` | defaults | ant operator list / ρ (#2876) |
| `"limit"` / `"neighbor"` | `5` / `"gaussian"` | ABC abandon limit / neighbor mode (#2878) |
| `"sep"` / `"align"` / `"cohere"` | `1.0` / `0.5` / `0.3` | Boids weights (#2879) |
| `"radius"` / `"max-speed"` | `0.5` / `0.25` | Boids neighborhood / speed clamp |
| `"step"` | `0.1` | FSS base individual swim step (#2880) |
| `"seed"` | `424242` | PRNG seed (pso/abc/boids/fss init) |
| `"parallel"` | `#f` | If `#t`, evaluate fitness with flat `fiber:spawn` then main-thread joins |

### Individual representation

- **Continuous** (grid centers, pso): list of numbers, length = `dim`
- **Discrete genes** (ant internal): bin indices; mapped to continuous via bin midpoints
- **Opaque code body**: out of scope — pass strings into fitness yourself; apply via mutate outside

### Report schema (stable)

```
kind, gen, best-fit, mean-fit, best, pop, dim, diversity, parallel
```

`swarm:export` serializes: `kind`, `gen`, `best-fit`, `mean-fit`, `pop`, `dim`, `diversity`.

## Backends

### `kind: "grid"` (#2875 baseline)

Discrete linspace product over `bounds` × `bins` (Unify-style policy scan).
Each step evaluates a **window** of size `pop` and advances the cursor through
the full grid so multi-generation scans cover the product without nested joins.

### `kind: "pso"` (#2877)

Classical particle swarm: \(v ← w v + c_1 r_1 (pbest-x) + c_2 r_2 (gbest-x)\),
then clamp \(x+v\) to bounds. Defaults `w=0.7`, `c1=c2=1.4`, `pop=16`.
Dedicated surface: [`std/pso`](pso.md). Prefer for **continuous** knobs;
use **grid** for discrete scans, **ant** for mutation-type ranking.

### `kind: "abc"` (#2878)

Artificial bee colony: employed + onlooker local search, scout after
`limit` failed improvements. Continuous food sources. Surface: [`std/abc`](abc.md).
Prefer multi-cand exploit/abandon; PSO for velocity memory; grid for discrete scan.

### `kind: "boids"` (#2879)

Flocking coordination (separation / alignment / cohesion) on param vectors
via [`std/boids`](boids.md). **Not** a sole optimizer — multi-agent diversity
and shared search direction; high `sep` increases diversity.

### `kind: "fss"` (#2880)

Fish school search: individual swim + feeding weights + collective barycenter
+ **volitive** expand/contract of step size. Surface: [`std/fss`](fss.md).
Stagnation increases `report.volitive`; fitness improvement contracts it.
Prefer load-adaptive explore/contract vs PSO’s fixed velocity rules.

### `kind: "ant"` (#2876 bridge)

**Not** classical graph ACO. Uses `std/ant` mutation-type pheromone table
(same model as #444 strategy trails):

| Piece | Role |
|-------|------|
| Individual | Operator type **name** (string), e.g. `"edsl-lit-tweak"` |
| Population | `pheromone:rank` over `ops` (default: ant default trails) |
| Step | optional `pheromone:evaporate!`, eval fitness, `pheromone:update` ±delta |
| `colony:search` | **Unchanged** — local workspace search still available |

```aura
(swarm:init (hash "kind" "ant"
                  "pop" 7
                  "ops" (list "edsl-lit-tweak" "edsl-op-swap" "edsl-disp-ref")
                  "evaporate" 0.95))
(swarm:step! (lambda (op)
  ;; synthetic / denseness fitness: higher = prefer this mutation type
  (if (string=? op "edsl-lit-tweak") 1.0 0.1)))
(swarm:best)   ; → preferred operator name
```

## Parallel fitness (safe)

```aura
(swarm:init (hash "kind" "grid" "parallel" #t ...))
;; or (swarm:parallel! #t)
```

Implementation: **spawn all** fitness fibers on the caller, then **join all** on
the caller. Never `fiber:join` inside a worker (nested-join host residual).

## Example

```aura
(require "std/swarm" all:)
(define (sphere ind)
  (let loop ((xs ind) (s 0.0))
    (if (null? xs) (- 0.0 s)
      (loop (cdr xs) (+ s (* (car xs) (car xs)))))))

(swarm:init (hash "kind" "grid" "dim" 1 "pop" 8
                  "bounds" (list (list -2.0 2.0)) "bins" 8))
(swarm:step! sphere)
(swarm:best)                 ; → e.g. (-0.25)
(hash-ref (swarm:report) "mean-fit")
(swarm:population)
```

See also: `tests/suite/swarm_2874.aura` (PSO), epic #2874.
