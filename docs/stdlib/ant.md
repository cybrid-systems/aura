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

## colony:search contract (#4738)

`(colony:search expected-str max-variants)` → `(found? output tag)`.

- **Budget is a hard cap** - every attempted variant is charged against
  `max-variants`, including each delta of one literal (the report tag
  carries the exact count, e.g. `colony:6var`).
- **Failure restores the workspace** - the search checkpoints with
  `ast:snapshot` on entry and `ast:restore`s on every non-success exit, so
  a failed search never leaves its last mutation installed. Success keeps
  the winning mutation (that is the point of the search).
- **Literal search is scoped** - literals are only tweaked inside the
  define whose body is being searched (`find-literal-ints` honours its
  `fn-name`); an unrelated define is never touched.
- **Strategy order** - `display-ref` (replace a body with
  `(display <ref>)`), then integer literals (`+/-1`, `+/-2`), then float
  literals with relative factors (`+/-10%`, `+/-25%`, `+/-50%`:
  `new = old * (1 + factor)`), so thresholds like `0.15` are searchable.
- **Provenance** - every attempt passes a reason to `mutate:rebind` /
  `mutate:tweak-literal` (`colony:display-ref`, `colony:lit-tweak`,
  `colony:float-tweak`).
