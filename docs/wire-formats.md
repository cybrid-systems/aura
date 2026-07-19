# Aura wire formats

Authoritative spec, frozen at Cycle 14 P3. Audience: anyone implementing
cache file format / IPC / interop that round-trips Aura AST/IR state.

Convention: little-endian multi-byte. Span fields are NON-OWNING —
deserialize span points into buffer, caller MUST keep buffer alive
for span lifetime.

## Index

1. NodeView wire format (12 fields, 53/89 bytes)
3. FlatAST SoA v2 (Cycle 14 P3 — current production)
10. Parallel orchestration contracts (#1584–#1600)

---

## 1. NodeView wire format

`aura::ast::NodeView` (`src/core/ast.ixx:375-396`) — 12 fields in
declaration order via generic `auto_serialize<T>`:

| # | Field | Type | Bytes |
|---|-------|------|------:|
| 1-8 | `id` / `tag` / `int_value` / `float_value` / `sym_id` / `line` / `col` / `type_id` | POD | 40 |
| 9 | `children` | `span<const NodeId>` | 4 + 4·N₁ |
| 10 | `params` | `span<const SymId>` | 4 + 4·N₂ |
| 11 | `param_annotations` | `span<const NodeId>` | 4 + 4·N₃ |
| 12 | `marker` | `u8` (SyntaxMarker) | 1 |

**Empty total = 53 bytes** (3 span headers = 12). **Populated
(3 children + 4 params + 2 annotations) = 89 bytes**.

Span header = u32 `byte_count` (= `elem_count · sizeof(elem)`).
Span data follows immediately.

---

## 3. FlatAST SoA wire format v2 (Cycle 14 P3 — production)

`aura::ast::FlatAST` — SoA layout, 22 PRIVATE `std::pmr::vector<T>`
columns + 2 scalars + 5 v2 side-data fields. Custom
`serialize_soa` / `deserialize_soa` in `src/core/ast.ixx` (generic
reflect path can't see private members).

```
[ u32 format_version = 2 ]
[ u32 num_nodes ]
[ 22 columns: each = u32 count + count · sizeof(elem) bytes ]
[ u32 next_mutation_id (low 32 bits) ]
[ u16 generation ]
[ u16 reserved ]
--- v2 additions ---
[ u32 mutation_log_count ] + MutationRecord records
[ u32 match_info_count ]   + MatchClauseInfo records
[ u32 region_by_sym_count ] + per-entry [ u32 key ][ u8 value ]
[ u32 region_by_lambda_id_count ] + per-entry [ u32 key ][ u8 value ]
[ u32 root NodeId ]
```

22 columns (fixed order): `tag_` / `int_val_` / `float_val_` / `sym_id_`
/ `child_begin_` / `child_count_` / `child_data_` / `parent_`
/ `param_begin_` / `param_count_` / `cap_require_count_` / `param_data_`
/ `param_annot_data_` / `line_` / `col_` / `marker_` / `dirty_`
/ `type_id_` / `error_kind_` / `value_cache_` / `node_first_mutation_`
/ `node_gen_`.

v1 caches load into v2 readers (v2 reader detects `version==1`,
reads v1 scalars, returns with v2 fields empty). v2 caches do NOT
load into v1 readers.

---

## 10. Parallel orchestration contracts (#1584–#1600)

Agent-facing hash / metrics shapes. Stable keys; unknown keys ignored.

### 10.1 `(parallel-intend …)` result hash (schema **1587**)

```scheme
(parallel-intend
  (vector (lambda () …) (lambda () …))
  :max-concurrency 8 :timeout-ms 60000
  :fail-fast #f :collect-errors #t)
```

| Key | Type | Meaning |
|-----|------|---------|
| `status` | string | `ok` · `partial` · `timeout` · `fail-fast` · `invalid` · `quota-exceeded` |
| `ok-count` / `err-count` / `aborted-count` | int | Task counts |
| `wait-us` | int | Wall time in join |
| `results` | vector | Per-task `{ok, index, value\|error}` |
| `schema` | int | **1587** |

`orch:parallel-intend` is an alias. C++ mirror:
`aura::serve::parallel_orch::BatchResult` with `BatchStatus` enum.

### 10.2 Orchestration metrics

| Surface | Schema | Keys |
|---------|--------|------|
| `query:parallel-orch-stats` | 1586 | `batches` / `spawned` / `joined` / `ok` / `err` / `fail-fast-aborts` / `timeouts` / `mailbox-posts` / `phase` |
| `query:orch-module-stats` | 1588 | `agents-spawned` / `agents-joined` / `agents-send` / `agents-recv` / `spawn-failures` / `spawn-quota-rejects` / `parallel-batches` |
| `query:mf-mailbox-stats` | 1585 | `Mailbox depth / backpressure` |
| `query:orchestration-steal-stats` | 1633 | `steal defer + starvation mitigation` |
| `query:mutation-boundary-fairness-stats` | 1635 | unified fairness dashboard |
| `query:ai-closedloop-readiness-stats` | 1613 | macro-health + orch + linear GC lineage |
| `query:macro-hygiene-stats` | 1613 | consolidated macro hygiene + audit |

### 10.3 Join status

C++ `JoinStatus`: `Ok` · `Timeout` · `Cancelled` · `Invalid`. Aura
`(orch:agent-join name-or-id [:timeout-ms n])` returns hash with
`status` / `wait-us` / `ok`. Prefer timeouts over blocking forever.

### 10.4 MultiFiberMailbox message (logical JSON for Agents)

```json
{"payload": "opaque-string", "priority": "normal",
 "from_fiber": 0, "to_fiber": 0}
```

`PushStatus`: `Ok` · `Backpressure` · `Closed`. Agents must handle
backpressure (retry / yield / drop / fail-fast). Linear-claim
payloads may use `linear-viol:` prefix (#1595).

### 10.5 ResourceQuota (#1600)

When Fibers quota is exhausted: `BatchStatus::QuotaExceeded` /
`AgentHandle.quota_exceeded = true`, error string contains
`ResourceQuotaExceeded`. Metrics: `query:resource-quota-stats`
+ `fiber_spawn_rejected_total` / `orchestration_quota_exceeded_total`.

Stress suite: `tests/suite/parallel_orchestration_stress.aura` (#1602).