# Agent Migration Guide — Demoted `query:*` Primitives

> **Status**: Issue #1462 Phase 1 (placeholder + framework). Real entries are
> filled in **after each demotion lands**, with a one-release-cycle deprecation
> window per primitive.

This guide tracks every `query:*` engine primitive that has been
**demoted to stdlib** (Issue #562, #1449 / SlimSurface Phase 1+2) so
agents can mechanically update their hard-coded calls.

The companion file `lib/std/compat.aura` ships **temporary shims** that
re-export the old names as thin wrappers, so existing agent code keeps
working for one release cycle. The shim emits a one-shot deprecation
warning per session per name (Issue #1462 AC3 — engine-side once
available; Aura-side guard today).

---

## Demoted Primitives — Migration Table

| Old name (engine) | New name (stdlib or compose) | Source | Removal target |
| --- | --- | --- | --- |
| `query:siblings` | `(filter (lambda (n) (not= n self)) (query:children (query:parent n)))` | `docs/design/query-namespace-decision.md` #33 | one release after stdlib helper ships |
| `query:find-by-name` | `(query:find n)` (wraps the more general `find`) | `query-namespace-decision.md` #34 | one release after wrapper stabilizes |
| `query:nodes-with-marker` | `(query:by-marker m)` (existing engine primitive) | `query-namespace-decision.md` #35 | one release after wrapper stabilizes |
| `query:subtree` | `(fold-tree self cons)` (planned; not yet shipped) | mentioned in `agentic-slim-surface-rectification.md` #89 | pending stdlib helper |

### Replacement patterns

#### `query:siblings` → filter over children

```aura
;; OLD
(define sibs (query:siblings n))

;; NEW
(define sibs
  (filter (lambda (s) (not= s n))
          (query:children (query:parent n))))
```

#### `query:find-by-name` → `query:find`

```aura
;; OLD
(define node (query:find-by-name "x"))

;; NEW
(define node (query:find "x"))
```

`query:find` is a superset (matches by name OR by symbol) — verify your
input is always a string literal.

#### `query:nodes-with-marker` → `query:by-marker`

```aura
;; OLD
(define marked (query:nodes-with-marker 'user))

;; NEW
(define marked (query:by-marker 'user))
```

#### `query:subtree` → `fold-tree` (planned)

```aura
;; OLD
(define kids (query:subtree n))

;; NEW (when stdlib fold-tree ships)
(define kids (fold-tree n cons))
```

---

## How to use the compatibility shim

Existing agent code that hard-codes the old names will keep compiling
and running through the `lib/std/compat.aura` shim. The shim defines
each demoted primitive as a thin wrapper that:

1. Calls the new stdlib/primitive equivalent.
2. Emits a deprecation warning **at most once per session per name**
   (AC3 — engine-side emission, not yet wired; today the shim is silent
   and the warning contract is documented for the engine follow-up).
3. Returns the same shape the old primitive returned.

```aura
(import "std/compat")  ;; enable shims
;; ... existing code that calls (query:siblings n) still works ...
```

To verify your agent code is migration-clean:

```bash
grep -rn 'query:siblings\|query:find-by-name\|query:nodes-with-marker\|query:subtree' .aura files
```

Any hit is using the shim — plan to migrate before the next release.

---

## Removal timeline

- **Now (Issue #1462 ship)**: shims available; engine primitives still
  exist; migration guide + table published.
- **+1 release**: shims still available; engine primitives emit a
  stderr warning on use (engine follow-up #1462.3).
- **+2 releases**: engine primitives removed; shims become the only
  path. Calling a removed engine primitive returns `(error
  'removed "...")`.

The exact release dates are tracked in the per-primitive demotion
issues (see `Related` below).

---

## Related

- `docs/design/query-namespace-decision.md` — full audit + Tier-1/2 demotion plan
- `docs/design/primitives-demotion-batch1.md` — Batch 1 demotion candidates
- `docs/design/agentic-slim-surface-rectification.md` — overall plan
- Issue #562 — `query:*` namespace audit
- Issue #1449 / Epic #1462 — SlimSurface + this guide
- `lib/std/compat.aura` — compat shims (new in this commit)
- `docs/agent-prompt-template.md` — to be updated to recommend new names

Refs: #1462