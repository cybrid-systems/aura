# Agent prompt template (Aura self-mod loop)

> **Issue #1438** — ≤80 lines. Paste into system/tool prompts for coding agents.
> Canonical names only. Do **not** invent new `query:*-stats` primitives.

```
You are editing Aura (Scheme-like) live AST via these ops only:

BOOT / EVAL
  (set-code "<source string>")     ; load workspace AST
  (eval-current)                   ; run workspace
  (current-source)                 ; read back source if needed

QUERY  (query :op …)   — #1435
  (query :root)
  (query :find "name")             ; symbol → node list
  (query :node id)
  (query :children id)             ; raw NodeIds
  (query :children id :stable #t)  ; (id . gen) across mutate — prefer this
  (query :parent id)
  (query :parent id :stable #t)
  (query :def-use "var")
  (query :mutation-log)

MUTATE  (mutate :op …)  — #1436
  (mutate :rebind "name" "<code>" "summary")
  (mutate :replace id :subtree|:pattern|:value|:type …)
  (mutate :move node parent idx)
  (mutate :extract id "new-name")
  (mutate :validate "<code>" "schema")
  (mutate :atomic ops-list)        ; transactional batch

WORKSPACE  (workspace :op …)  — #1437
  (workspace :create "name")       ; → id
  (workspace :switch id)
  (workspace :merge id)
  (workspace :lock id) / (workspace :unlock id)

METRICS  (engine:metrics …)  — #1433; never add *-stats names
  (engine:metrics)                 ; nested groups
  (engine:metrics :group "jit")
  (engine:metrics "query:foo-stats")  ; legacy read by name only

VERSION
  (ast:snapshot "label") / (ast:restore snap)

STDLIB (prefer over raw C++ convenience)
  (require "std/surface" all:)
  (require "std/engine-metrics" all:)

CORE LOOP
  1. set-code → 2. query :find/root → 3. mutate :rebind/replace
  4. eval-current → 5. engine:metrics if observing → 6. restore on fail

DEPRECATED (still work; do not use in new code)
  query:find, mutate:rebind, workspace:create, query:*-stats, …
  See (api-reference) *deprecated* section.

OUTPUT: valid Aura sexprs only for tool calls. Prefer stable refs
after any mutate. Prefer (mutate :rebind name code summary) for
function body changes by name.
```

---

Related: [tutorial.md](tutorial.md) · [api-reference.md](api-reference.md) · [wire-formats.md](wire-formats.md)
