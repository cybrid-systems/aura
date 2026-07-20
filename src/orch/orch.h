// orch.h — Issue #1588: unified orchestration facade.
// Aggregates agent_spawn, MultiFiberMailbox, parallel_orch under aura::orch.
//
// Issue #1965 (Phase 3 scope deferral): MVP vs deferred scope.
// MVP (single-agent primitives; safe to ship in production):
//   - spawn_agent_with_mailbox  (orch/agent_spawn.h)
//   - join_agent / join_agents
//   - agent_send / agent_recv
//   - AgentHandle / AgentSpec structs
//   - OrchModuleStats (single-agent observability counters)
//   - release_agent_memory_reservation
// Deferred (multi-agent coordination; behind AURA_ORCH_DEFERRED guard or
// tracked in scripts/check_orch_mvp_scope.py — see #1965 cycle 1 close
// comment for the follow-up issue list):
//   - AgentRegistry + global_agent_registry()   (named registry, single
//     production consumer in evaluator_primitives_agent.cpp)
//   - conduct_parallel                          (parallel batch alias;
//     reach into serve::parallel_orch::parallel_intend directly)
//
// New callers should use the MVP surface. Reach into deferred features
// only when the orch-mvp-scope linter explicitly allows (test code + the
// single evaluator_primitives_agent.cpp consumer are grandfathered).

#ifndef AURA_ORCH_ORCH_H
#define AURA_ORCH_ORCH_H

#include "orch/agent_spawn.h"

// Re-export serve orchestration building blocks into a single include surface.
// Prefer:
//   #include "orch/orch.h"
// over reaching into serve/ for multi-agent work.

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

// parallel_intend is the primary batch API; conduct_parallel is the orch alias.
using serve::parallel_orch::parallel_intend;
using serve::parallel_orch::parallel_run;
using serve::parallel_orch::sequential_run;
using serve::parallel_orch::validate_policy;

} // namespace aura::orch

#endif // AURA_ORCH_ORCH_H
