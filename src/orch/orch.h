// orch.h — Issue #1588: unified orchestration facade.
// Aggregates agent_spawn, MultiFiberMailbox, parallel_orch under aura::orch.
//
// ════════════════════════════════════════════════════════════════════
// STATUS: Advanced / Experimental (Issue #1945, 2026-07 through 2026-10)
// ════════════════════════════════════════════════════════════════════
//
// This module is marked Advanced / Experimental for the next 2-3
// months while the core mutation + hot-update MVP are being hardened.
// Callers should use the MVP surface (spawn_agent_with_mailbox +
// join_agent + agent_send + agent_recv + AgentHandle/AgentSpec).
// See docs/agent-orchestration-status.md for full status + P0 guarantee.
//
// ════════════════════════════════════════════════════════════════════
//
// Issue #1965 / #1966: single-agent MVP only on the public orch surface.
// MVP (safe to ship in production):
//   - spawn_agent_with_mailbox  (orch/agent_spawn.h)
//   - join_agent / join_agents
//   - agent_send / agent_recv
//   - AgentHandle / AgentSpec structs
//   - OrchModuleStats (single-agent observability counters)
//   - release_agent_memory_reservation
//   - parallel_intend / parallel_run (re-exports of serve::parallel_orch)
//
// Removed from public orch/ (#1966 — was // DEFERRED under #1965 cycle 1):
//   - AgentRegistry / global_agent_registry — evaluator-local name table
//   - conduct_parallel alias — use parallel_intend directly
// Reintroduction is blocked by scripts/check_orch_mvp_scope.py --strict.

#ifndef AURA_ORCH_ORCH_H
#define AURA_ORCH_ORCH_H

#include "orch/agent_spawn.h"

// Re-export serve orchestration building blocks into a single include surface.
// Prefer:
//   #include "orch/orch.h"
// over reaching into serve/ for batch / mailbox work.

namespace aura::orch {

// Aliases for discovery (call sites may use aura::orch:: names).
using Mailbox = serve::mf_mailbox::MultiFiberMailbox;
using MailMessage = serve::mf_mailbox::MailMessage;
using MailPriority = serve::mf_mailbox::MailPriority;
using PushStatus = serve::mf_mailbox::PushStatus;

using ParallelPolicy = serve::parallel_orch::ParallelPolicy;
using TaskSpec = serve::parallel_orch::TaskSpec;
using TaskResult = serve::parallel_orch::TaskResult;
using BatchResult = serve::parallel_orch::BatchResult;
using BatchStatus = serve::parallel_orch::BatchStatus;

// Primary batch API (re-export). No orch-specific parallel alias.
using serve::parallel_orch::parallel_intend;
using serve::parallel_orch::parallel_run;
using serve::parallel_orch::sequential_run;
using serve::parallel_orch::validate_policy;

} // namespace aura::orch

#endif // AURA_ORCH_ORCH_H
