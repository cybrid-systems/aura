# std/ant — mutation-type pheromone + colony search

## Model (Issues #2876 / #444)

This is **not** classical graph ACO / TSP. Trails score **mutation operator
types** (e.g. `edsl-lit-tweak`, `edsl-op-swap`) for denseness and strategy
control — same family as closed strategy-evolution pheromone metrics (#444).

| API | Role |
|-----|------|
| `pheromone:init` | Seed default trail table |
| `pheromone:score` / `update` | Read / deposit on a type name |
| `pheromone:rank` | Sort a type list by score (descending) |
| `pheromone:evaporate!` | Global multiply-by-rho (#2876) |
| `pheromone:types` | Keys currently in the table |
| `pheromone:export` | JSON-ish snapshot |
| `colony:search` | Local workspace mutate+test (compat, unchanged) |

## Swarm bridge

```aura
(require "std/swarm" all:)
(swarm:init (hash "kind" "ant" "ops" (list "edsl-lit-tweak" "edsl-op-swap")
                  "evaporate" 0.95 "pop" 4))
(swarm:step! fitness-on-op-name)
```

See `docs/stdlib/swarm.md` and `tests/suite/swarm_ant_bridge_2876.aura`.
