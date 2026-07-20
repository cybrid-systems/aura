# Agent Orchestration Status

**Status: Advanced / Experimental** (Issue #1945, parent #1942 Simplification Roadmap).

The `src/orch/` module and its associated primitives are marked
**Advanced / Experimental** for the period 2026-07 through 2026-10
(approximately 2-3 months) while the core mutation + hot-update MVP
are being hardened.

## Scope

### MVP primitives (single-agent, production-safe)

These primitives are kept in the default surface and remain
production-safe. They pair with the core fiber / mailbox layer
(`serve/fiber.h`, `serve/multi_fiber_mailbox.h`) without introducing
additional cross-cutting complexity:

- `(orch:spawn-agent …)` — `spawn_agent_with_mailbox` in
  `src/orch/agent_spawn.h`. Spawns a fiber agent on a scheduler,
  optionally with a private `MultiFiberMailbox`. Includes
  ResourceQuota preflight (typed `ResourceQuotaExceeded`, never
  panic) + MutationBoundaryGuard::try_acquire wrapper around the
  agent body. Name→handle bookkeeping for join-by-name is
  evaluator-local (`OrchAgentNameTable` in
  `evaluator_primitives_agent.cpp`; not a public orch multi-agent API).
- `(orch:agent-join …)` — `join_agent` / `join_agents`. Blocks the
  caller until the agent fiber completes. Includes Issue #1879
  post-join StableNodeRef provenance enforcement.
- `(orch:parallel-intend …)` / `parallel_intend` re-export — batch
  via `serve::parallel_orch::parallel_intend` (no orch-specific
  alias).
- `(query:orch-module-stats)` — read-only observability snapshot
  of single-agent counters (spawned / joined / send / recv /
  quota-rejects).

### Removed multi-agent public surface (Issue #1966)

Per #1965 cycle 1 the following were marked `// DEFERRED`. Issue
#1966 **removed** them from the public `orch/` surface (decision:
remove, do not keep as a second public multi-agent API):

| Former symbol | Disposition |
|---|---|
| `AgentRegistry` + `global_agent_registry()` | Demoted to evaluator-local `OrchAgentNameTable` (name bookkeeping for MVP `orch:spawn-agent` / `orch:agent-join` only). Not multi-agent coordination. |
| `conduct_parallel` | Deleted. Callers use `serve::parallel_orch::parallel_intend` (re-exported as `aura::orch::parallel_intend`). Optionally bump `g_orch_module_stats.parallel_batches` at the call site. |

Reintroduction guard: `scripts/check_orch_mvp_scope.py --strict`
(wired into `./build.py gate` as `cmd_orch_mvp_scope`).

## Module façade

- `src/orch/orch.h` — re-exports serve/ orchestration building
  blocks into a single include surface. Documented MVP-only
  boundary at the top of the file.
- `src/orch/agent_spawn.h` — single-agent MVP implementation.
- `src/orch/orch.ixx` — C++20 module wrapper. Lists the 4
  orchestrated components (`agent_spawn`, `multi_fiber_mailbox`,
  `parallel_orch`, `fiber_join`). Phase 1 / Issue #1588.

## Cross-cutting mechanism dependencies

`src/orch/` introduces the following dependencies on core
mechanisms. Each is a safety-critical mechanism being hardened
during the 2026-07 through 2026-10 stabilization window:

- **MutationBoundaryGuard** — `aura_orch_agent_body_try_acquire` C
  bridge hook around agent body execution. On reject, the body is
  skipped (typed path); no panic.
- **ResourceQuota** — preflight for fibers + estimated arena /
  mailbox memory before spawn. Returns typed `ResourceQuotaExceeded`
  on failure.
- **StableNodeRef + linear ownership** — post-resume / steal
  provenance refresh on agent body exit + post-join enforcement
  on `Fiber::join` (Issue #1879).

These dependencies are intentional for the single-agent MVP but
contribute to the overall complexity surface. Any new
orchestration layer work should minimize additional mechanism
dependencies.

## P0 stability guarantee (AC #3)

During the 2026-07 through 2026-10 stabilization window:

- No new P0 issues shall be introduced by the orchestration layer.
- The single-agent MVP path (`spawn_agent_with_mailbox` +
  `join_agent` + ResourceQuota preflight + MutationBoundaryGuard
  wrapper) is the recommended surface for any agent-framework
  caller.
- Any new feature touching the orchestration layer must include a
  scope-reduction review (per #1965 AC #3 governance rule for
  features > 1000 LOC).

## Re-enable / enhancement path

When the core mutation + hot-update MVP are stable (target
2026-10), the following re-evaluation should occur:

1. **Multi-agent coordination (if still needed).** #1966 closed
   the deferred public surface by removal. A future multi-agent
   design should land as a *new* proposal (new issue + scope
   reduction review), not by reintroducing `AgentRegistry` /
   `conduct_parallel` under the old names. Prefer building on
   `serve::Fiber` + `MultiFiberMailbox` + `parallel_intend`
   directly, or a dedicated `orch/multi_agent/` module with its
   own namespace if a named registry is required.

2. **Re-evaluate the slim surface budget.** The MVP primitives
   count is small (3 primitives in the public add() surface for
   the orch facade). If new primitives are added as part of a
   multi-agent re-enable, verify they fit within the SlimSurface
   budget (`scripts/check_primitive_surface.py --strict`,
   `INTERIM_HARD_CEILING` / `TARGET_BUDGET`).

3. **Cross-cutting mechanism audit.** The dependencies on
   MutationBoundaryGuard, ResourceQuota, StableNodeRef + linear
   ownership should be re-evaluated when those mechanisms stabilize.
   If any of those mechanisms are themselves deferred or removed,
   the orch layer must be updated accordingly.

4. **Update this doc.** When the status moves from Experimental to
   Stable (or to a more fine-grained status), update this doc's
   status banner + AC checklist accordingly.

## References

- Parent: #1942 (Simplification Roadmap)
- Phase 1 (closed): #1943 (Hot-Update MVP), #1944 (EDSL primitives),
  #1963 (obs eval/jit consolidation)
- Phase 2: #1964 (provenance / epoch / guard / mutation-path
  simplification)
- Phase 3: #1965 (orch scope + commercial_readiness scope +
  governance + 11 follow-up issues)
- This status mark: #1945 (Advanced / Experimental mark + status doc)
- #1965 cycle 1: `bcb68c7c` MVP boundary + orch-mvp-scope linter
- #1966: remove deferred multi-agent public surface; linter `--strict`
  in gate; evaluator-local name table for spawn/join
- MVP pattern: #1943 (Hot-Update MVP — model for orch/ scope
  reduction)
- Linter: `scripts/check_orch_mvp_scope.py --strict` (orch surface)
- Governance: `scripts/check_primitive_surface.py --strict`
  (SlimSurface budget)
