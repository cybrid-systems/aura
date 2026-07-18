# mutate:remove-node inverted parent-edge index (#1689)

**Issue:** [#1689](https://github.com/cybrid-systems/aura/issues/1689)  
**Sibling:** [#1688](https://github.com/cybrid-systems/aura/issues/1688) (remove all parents)  
**Files:** `ast.ixx`, `mutators.ixx`  
**Status:** P1 perf — parent-find was O(N×C) per remove.

## Problem

`collect_incoming_child_edges` (used by `remove-node`) scanned every node’s
children list. Large workspaces + many removes pay O(N×C) per call.

`parent_` only stores **one** parent (tree view) and cannot express DAG
multi-parent edges required by #1688.

## Fix

Inverted index on `FlatAST`:

```
incoming_parent_edges_[child] = [(parent, child_index), ...]
```

| Path | Behavior |
|------|----------|
| Bulk (`link_children`, topology restore) | mark dirty / rebuild O(N+E) |
| `set_child` / `insert_child` / `remove_child` locked | incremental O(C_parent) when index valid |
| `collect_incoming_parent_edges` / mutator collect | O(deg) after ensure |

Stats: `incoming_parent_index_{rebuilds,lookups,hits}`.

## Tests

`tests/test_mutate_remove_node_parent_index_1689.cpp`
