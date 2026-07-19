# Agent Migration Guide

Status: Issue #1462 Phase 1. Tracks engine primitives demoted to stdlib
so agents can mechanically update hard-coded calls. `lib/std/compat.aura`
ships temporary shims.

## Migration table

| Old name (engine) | New name | Engine removed |
|---|---|---|
| `query:siblings` | `(filter … (query:children (query:parent n)))` or `(require "std/compat")` | #1449 batch |
| `query:find-by-name` | `(query:find n)` | one release after wrapper stabilizes |
| `query:nodes-with-marker` | `(query:by-marker m)` | one release after wrapper stabilizes |
| `query:subtree` | `(fold-tree self cons)` (planned) | pending stdlib helper |

## Shim usage

`(import "std/compat")` enables shims. Existing code keeps working
through `+1 release`, then shim becomes the only path. To verify
migration-clean: `grep -rn 'query:siblings\|query:find-by-name\|query:nodes-with-marker\|query:subtree' .aura files`.

## Timeline

- **Now**: shims available; engine primitives still exist.
- **+1 release**: engine primitives emit stderr warning on use.
- **+2 releases**: engine primitives removed; calling returns `(error 'removed "...")`.

Refs: #562, #1449, #1462.