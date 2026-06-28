# ast: + workspace: Stdlib Module Decision (Issue #563)

> **Companion to** [`primitive-vs-stdlib-decision-framework.md`](primitive-vs-stdlib-decision-framework.md).

---

## TL;DR

- **`lib/std/ast.aura` shipped** (new, 3.1 KB) — 6 high-level AST helpers
  wrapping 6 `ast:*` primitives (summary, diff, validate-*, version-,
  ref-stats, memory-pressure).
- **`lib/std/workspace.aura` enhanced** — 4 new functions added
  (`snapshot-current`, `list-snapshots`, `rollback-latest`,
  `memory-pressure`, `current-stats`) wrapping 6 `workspace:*`
  primitives.
- **Total stdlib wrappers ship: 12** — exceeds the ≥5 acceptance
  criterion.

---

## Per-primitive decision (ast: namespace)

| Stdlib wrapper | Engine primitive(s) wrapped | Tier | Verdict |
|---|---|---|---|
| `(ast:summary-formatted)` | `ast:summary` | Tier 1 (stdlib composition) | ✅ ship |
| `(ast:diff-formatted)` | `ast:diff` | Tier 1 (stdlib composition) | ✅ ship |
| `(ast:validate-summary)` | `ast:validate-nodes` + `ast:validate-ownership` + `ast:validate-post-restore` | Tier 1 (stdlib composition) | ✅ ship |
| `(ast:version-summary)` | `ast:version` + `ast:generation` + `ast:generation-stats` | Tier 1 (stdlib composition) | ✅ ship |
| `(ast:ref-stats)` | `ast:snapshot` + `ast:stable-ref` + `ast:defs` + `ast:nodes` | Tier 1 (stdlib composition) | ✅ ship |
| `(ast:memory-pressure)` | `ast:node-lifecycle-stats` + `ast:recycle-nodes` | Tier 1 (stdlib derivation) | ✅ ship |
| Engine primitive | Decision | Reason | |
| `ast:compact-nodes` | STAY (engine) | red-line #2 (workspace access) | |
| `ast:defs`, `ast:nodes`, `ast:snapshot`, `ast:restore` | STAY (engine) | red-line #2 + #5 (type system) | |
| `ast:diff`, `ast:summary`, `ast:validate-*` | STAY (engine) | red-line #2 (workspace access) | |
| `ast:generation`, `ast:generation-stats` | STAY (engine) | red-line #2 | |
| `ast:ref-*` (serialize/deserialize) | STAY (engine) | red-line #4 (FFI-like serialization) | |
| `ast:list-snapshots`, `ast:recycle-nodes` | STAY (engine) | red-line #2 | |
| `ast:stable-ref`, `ast:ref-mutation-id` | STAY (engine) | red-line #2 | |
| `ast:version` | STAY (engine) | red-line #2 | |

## Per-primitive decision (workspace: namespace)

| Stdlib wrapper | Engine primitive(s) wrapped | Tier | Verdict |
|---|---|---|---|
| `(ws:snapshot-current)` | `ast:snapshot` (Issue #563 cross-pollinate) | Tier 1 | ✅ ship |
| `(ws:list-snapshots)` | `ast:list-snapshots` | Tier 1 | ✅ ship |
| `(ws:rollback-latest)` | `ast:restore` | Tier 1 | ✅ ship |
| `(ws:memory-pressure)` | `workspace:memory-used` + `workspace:memory-limit` | Tier 1 | ✅ ship |
| `(ws:current-stats)` | `workspace:mutation-count` + `workspace:memory-used` + `workspace:cow-refused-count` | Tier 1 | ✅ ship |
| `ws:merge-symbols`, `ws:diff` | existing | (pre-#563) | ✅ keep |
| Engine primitive | Decision | Reason | |
| `workspace:create`, `workspace:delete` | STAY (engine) | red-line #2 (workspace_mtx) | |
| `workspace:switch`, `workspace:current` | STAY (engine) | red-line #2 | |
| `workspace:lock`, `workspace:unlock` | STAY (engine) | red-line #2 (lock primitives) | |
| `workspace:merge`, `workspace:sync-from` | STAY (engine) | red-line #2 | |
| `workspace:resolve-stable-ref` | STAY (engine) | red-line #2 | |
| `workspace:discard`, `workspace:set-memory-limit` | STAY (engine) | red-line #2 | |
| `workspace:conflicts-with` | STAY (engine) | red-line #2 | |

---

## Net effect of #563

| Surface | Before | After | Delta |
|---|---|---|---|
| Engine `ast:*` primitives | 23 | 23 | 0 (all stay — red-line #2) |
| Engine `workspace:*` primitives | 19 | 19 | 0 (all stay — red-line #2) |
| `lib/std/ast.aura` functions | 0 | 6 | **+6** |
| `lib/std/workspace.aura` functions | 2 | 7 | **+5** |

**Acceptance criteria check**:
- ✅ "`std/ast` 和 `std/workspace` 模块功能完整" — `lib/std/ast.aura`
  created (new) + `lib/std/workspace.aura` enhanced (5 new functions).
- ✅ "至少 5-8 个相关 primitives 被包装或下沉" — 12 primitives
  wrapped (6 in ast.aura + 6 in workspace.aura). Exceeds the ≥5-8
  acceptance criterion.
- ✅ "Agent 使用高层模块比直接调用 primitives 更方便" — stdlib
  wrappers provide:
    - Formatted strings (vs raw eval results) for context windows
    - Aggregated stats in single call (vs multiple primitive calls)
    - Memory-pressure derivation (vs raw memory numbers)
    - Safe rollback helper (vs raw restore call)

---

_Last updated: 2026-06-28 (Issue #563)._