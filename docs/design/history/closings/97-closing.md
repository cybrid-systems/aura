# Issue #97 — Long-Lifecycle Self-Adaptive Production Systems

## Status: ⚠️ PARTIAL — Design complete, primitives not all exposed

Issue #97 is "Top Insight 2" — a high-level market insight about 7x24
production agents (trading, DevOps) requiring hot-patching, evolution
history, and isolation without downtime. Like #96, it has 3 proposed
actions at different states of completion.

## Action 1: Complete AOT + Secure Hot-Patch

**Current state**: AOT is well-developed; secure hot-patch is missing.

| Component | File | Status |
|---|---|---|
| LLVM-based AOT backend | `src/compiler/aura_jit.cpp` (1751 lines) | ✅ Implemented |
| AOT C bridge | `src/compiler/aura_jit_bridge.cpp` (384 lines) | ✅ Implemented |
| IR-level hot-swap (replace function body, same ID) | `src/compiler/ir.ixx` `IRModule::hot_swap_function()` | ✅ Implemented |
| Hot-swap primitive (`hot-swap:fn` or similar) | Not exposed | ❌ Missing |
| Secure hot-patch (auth/signature on patches) | — | ❌ Missing |

**Why this matters**: With `hot_swap_function` in place but no Aura-level
primitive, the only way to hot-swap is to write a C++ extension. The
fix is small (expose the IR-level function as a primitive), and the
"secure" part (HMAC over the new function body) is a separate,
orthogonal concern.

## Action 2: Add Automatic Log-to-Mutate Closed Loop

**Current state**: AutoFixEngine exists at the C++ level; full closed-loop
orchestration is demoed in `projects/evo-kv/evo-kv-auto.aura` but not
exposed as a reusable primitive.

| Component | File | Status |
|---|---|---|
| MutationLog tracking | `src/compiler/service.ixx` `MutationLogEntry` | ✅ Implemented |
| AutoFixEngine (one-shot fix rules) | `src/compiler/query.ixx` | ✅ Implemented |
| Mutation history primitives | `mutation-history`, `mutation-count` | ✅ Implemented |
| Evo-kv full auto-evolution loop (log → gap → fix → re-eval) | `projects/evo-kv/evo-kv-auto.aura` | ✅ Demoed |
| Reusable `auto-evolve` primitive (generalize evo-kv-auto) | — | ❌ Missing |
| Real-time log → mutate bridge (background fiber watches logs) | — | ❌ Missing |

**Why this matters**: The pieces exist. A reusable `auto-evolve` primitive
that takes a log source and a fix-strategy would let any Aura project
plug into the same pattern without writing it from scratch.

## Action 3: Resource Isolation for Sub-Workspaces

**Current state**: Sub-workspaces exist (WorkspaceNode, WorkspaceTree)
with COW. No per-workspace memory/CPU limits.

| Component | File | Status |
|---|---|---|
| WorkspaceTree (parent/child nodes) | `src/compiler/evaluator.ixx` | ✅ Implemented |
| COW (clone on first mutate) | `src/compiler/evaluator_impl.cpp` | ✅ Implemented |
| Global set-memory-policy (auto-gc, warn-pct, etc.) | `src/compiler/evaluator_impl.cpp` | ✅ Implemented |
| Per-sub-workspace memory budget | — | ❌ Missing |
| Per-sub-workspace CPU/instruction budget | — | ❌ Missing |
| Per-sub-workspace error budget (e.g., max errors per minute) | — | ❌ Missing |
| `subworkspace:create` / `subworkspace:destroy` primitives | — | ❌ Missing |

**Why this matters**: For 7x24 production, one runaway sub-workspace can
take down the whole system. The global set-memory-policy covers the
whole evaluator, not individual sub-workspaces. Adding per-workspace
quotas is the missing piece.

## Summary: What 1 Person Can Ship in ~1 Day

This issue is design-complete but needs Aura-level primitives to be
exposed. Suggested order:

1. **Expose `hot-swap:fn` primitive** (~1 hour)
   - Tiny wrapper around `IRModule::hot_swap_function`
   - One Aura primitive, ~30 lines of evaluator code
2. **Expose `subworkspace:create` / `:destroy` / `:set-quota` primitives** (~3 hours)
   - Workspace creation, destruction, memory budget
3. **Expose `auto-evolve` primitive** (~4 hours)
   - Generalize `evo-kv-auto.aura` into a reusable Aura function
   - Takes a log source and a fix-strategy
4. **Add HMAC verification to hot-swap** (~4 hours, separate concern)
   - Sign patches with a per-deployment secret

## How to Close on GitHub

```bash
gh issue close 97 -c "See docs/design/history/closings/97-closing.md for analysis
of the 3 proposed actions. AOT is complete; secure hot-patch needs
the hot-swap primitive exposed. Auto-fix is one-shot; closed-loop
log-to-mutate is demoed in evo-kv/evo-kv-auto.aura but needs to be
exposed as a primitive. Sub-workspace isolation needs per-workspace
quotas. All three are follow-up issues with the design already in
place."
```

Or keep it OPEN and link to this closing file as a status update.
