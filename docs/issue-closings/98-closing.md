# Issue #98 — Multi-Agent Research & Complex Project Collaboration

## Status: ⚠️ PARTIAL — 3 actions at different states

Issue #98 is "Top Insight 3" — a high-level market insight about
multi-agent research teams (Isomorphic Labs style) doing scientific/vertical
AI work. Three proposed actions.

## Action 1: Strengthen Conflict Resolution in Workspace merge

**Current state**: `workspace:merge` exists; conflicts resolved by
"later overrides earlier" (child overrides parent).

| Component | Status |
|---|---|
| `workspace:merge child-id` primitive | ✅ Implemented |
| Source-level concatenation | ✅ Implemented |
| "Child overrides parent" strategy | ✅ Implemented (default) |
| 3-way merge (base, ours, theirs) | ❌ Missing |
| Conflict detection (report which symbols conflict) | ❌ Missing |
| Configurable strategies (manual, ours, theirs) | ❌ Missing |
| CRDT-based merge for shared data structures | ❌ Missing |
| Atomic merge (rollback on partial failure) | ❌ Missing |

**Why this matters**: In multi-agent teams, two agents editing the
same workspace in parallel will silently overwrite each other. The
"last writer wins" approach is a data-loss footgun in 24/7 production.

## Action 2: Add Intent-Based Role Routing

**Current state**: `(intend ...)` exists as a primitive; per-role routing
is described in design docs but not implemented as a runtime feature.

| Component | Status |
|---|---|
| `(intend goal-expr [strategy: ...])` primitive | ✅ Implemented |
| `(intend-history)` append-only timeline | ✅ Implemented |
| `(intend-analytics)` per-strategy metrics | ✅ Implemented |
| Design doc for role routing | `docs/design/intent_orchestration.md` ✅ |
| Runtime role-based dispatcher (routing `intend` calls to specific agents) | ❌ Missing |
| Role registration (define role → strategy) | ❌ Missing |
| Capability matching (route to agent with right skills) | ❌ Missing |

**Why this matters**: A "research team" needs dynamic task assignment
based on who's best at what. Without role routing, every agent
gets every task.

## Action 3: Build BioTech / Drug Discovery Demo

**Current state**: No BioTech demo exists. The demos are general-purpose
(KV, chat, agent orchestration).

| Demo | Domain |
|---|---|
| `projects/evo-kv/` | Self-evolving key-value store |
| `projects/chat/` | Multi-agent chat |
| `projects/kv/` | Standard key-value store |

**Why this matters**: A vertical demo (BioTech, drug discovery, or
similar) is what makes the "agent research team" concept tangible.
It would showcase the multi-agent + intent-routing + workspace-merge
features in a real domain.

## Suggested Implementation Order

1. **Conflict detection primitive** (~2 hours)
   - `(workspace:conflicts-with child-id)` → list of conflicting symbols
   - Quick win, makes merge safer
2. **3-way merge primitive** (~3 hours)
   - `(workspace:merge-3way base-id ours-id theirs-id [strategy: ...])` → #t
3. **Role registration + dispatcher** (~6 hours)
   - `(role:register role-name strategy-name [capability ...])` → #t
   - `(intend goal strategy: "auto" [preferred-roles: ...])` dispatches
4. **BioTech demo** (~10+ hours)
   - Compound demo: protein structure prediction, drug-target interaction,
     etc. Showcases all 3 above features in a domain context.

## How to Close on GitHub

```bash
gh issue close 98 -c "See docs/issue-closings/98-closing.md for the 3
proposed actions. workspace:merge exists but only with 'last-writer
wins' conflict resolution. intend primitive exists but role-based
routing is design-only. No BioTech demo. All three are follow-up
issues — see the suggested implementation order in the closing file."
```

Or keep it OPEN and link to this closing file as a status update.
