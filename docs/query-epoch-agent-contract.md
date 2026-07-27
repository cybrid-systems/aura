# QueryEpoch Agent contract (#2192)

## When is my query consistent with my last mutate?

1. **Mutate commits** under outermost `MutationBoundaryGuard` (exclusive
   `workspace_mtx_`). That path bumps process-global `mutation_epoch` and
   may advance FlatAST `generation`.
2. **Query runs** under `shared_lock(workspace_mtx_)`. While a Guard is
   open, concurrent queries **block** on the lock — they never observe a
   half-mutated children topology.
3. **Primary workspace queries** (`query:find`, `query:children`,
   `query:root`, `query:defines`, `query:parent`, `query:pattern`,
   `query:children-stable`, …) capture a **QueryEpoch** at entry:
   `{ mutation_epoch, generation, bridge_epoch, workspace_id }` and
   re-check at exit.
4. **Read the snapshot** after any stamped query:

   ```scheme
   (hash-ref (engine:metrics "query:query-epoch-stats") "last-mutation-epoch")
   (hash-ref (engine:metrics "query:last-epoch") "last-generation")
   ```

5. **Correlate with mutate**: after mutate, run a cheap query (e.g.
   `query:root`) and compare `last-mutation-epoch` / `last-generation` to
   values you stored post-mutate. Equal ⇒ the query result matches that
   commit.

## Strict mode

- C++: `aura::core::set_query_epoch_strict(true)`
- Env: `AURA_QUERY_EPOCH_STRICT=1` (or `true` / `yes`)
- When strict is on and epoch advances between capture and finish, the
  primitive returns an error with tag `query-epoch-stale` and
  `stale-total` increments.

## Nested Guards / atomic-batch

Nested Guards and txn-dirty batches keep the exclusive lock; epoch bumps
inside the batch are not visible to concurrent queries until the
outermost Guard unlocks. QueryEpoch finish under a held `shared_lock`
sees a stable generation for that snapshot.

## Observability (`schema-2192`)

| Key | Meaning |
|-----|---------|
| `last-mutation-epoch` | Last capture mutation epoch |
| `last-generation` | Last capture FlatAST generation |
| `capture-total` | Times QueryEpoch was stamped |
| `mismatch-total` | Finish saw epoch/gen advance |
| `stale-total` | Strict mode refused the result |
| `strict` | 1 if strict mode on |
| `query-epoch-wired` | 1 |

Surfaces: `engine:metrics "query:query-epoch-stats"` and alias
`engine:metrics "query:last-epoch"`.
