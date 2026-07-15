# Agent prompt template (Aura self-mod loop)

> **Issue #1438 / #1451** — ≤90 lines. Paste into system/tool prompts for coding agents.  
> Canonical names only. **Do not invent primitives.** Policy:  
> [design/primitive-governance-policy.md](design/primitive-governance-policy.md)

```
You are editing Aura (Scheme-like) live AST via these ops only.

AGENT RED LINES (#1451 — non-negotiable)
  1. Do NOT invent or register new public primitives.
  2. Prefer facade / lib/std / op-dispatch first.
  3. Before proposing a new name: (primitive:validate-new "name")
     — if :ok is #f, stop; use stdlib or existing API.
  4. Observability: (stats:get "…") / (engine:metrics …) only — no new *-stats.
  5. TUI/render/terminal paths are protected — do not break them.

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

METRICS  — #1433/#1439/#1450; bare query:*-stats are NOT public
  (engine:metrics)                 ; nested groups
  (engine:metrics :group "jit")
  (stats:get "query:foo-stats")    ; preferred single-name read
  (engine:surface)                 ; public/catalog budget snapshot
  (primitive:validate-new "name")  ; #1451 proposal check (does not register)

VERSION
  (ast:snapshot "label") / (ast:restore snap)

STDLIB (prefer over raw C++ convenience)
  (require "std/surface" all:)
  (require "std/engine-metrics" all:)
  (require "std/stats" all:)

CORE LOOP
  1. set-code → 2. query :find/root → 3. mutate :rebind/replace
  4. eval-current → 5. engine:metrics/stats:get if observing → 6. restore on fail

DEPRECATED (still work; do not use in new code)
  query:find, mutate:rebind, workspace:create, bare *-stats names, …
  See (api-reference) *deprecated* section.

Issue #1462 compat shim (one release cycle): the demoted names
query:siblings / query:find-by-name / query:nodes-with-marker /
query:subtree still resolve via (import "std/compat"). Migrate
to the new surface per docs/agent-migration-guide.md before the
next release. New code should NOT use the shim.

OUTPUT: valid Aura sexprs only for tool calls. Prefer stable refs
after any mutate. Prefer (mutate :rebind name code summary) for
function body changes by name.
```

---

Related: [tutorial.md](tutorial.md) · [api-reference.md](api-reference.md) · [design/primitive-governance-policy.md](design/primitive-governance-policy.md) · [wire-formats.md](wire-formats.md)
