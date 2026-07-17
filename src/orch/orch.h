// orch.h — Issue #1588: unified orchestration facade.
// Aggregates agent_spawn, MultiFiberMailbox, parallel_orch under aura::orch.

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
