#include "messaging_bridge.h"
#include "stats_registry_suppress.h"

// ── Messaging bridge globals (non-module .cpp for linker visibility) ──
// These are set by CompilerService during construction and read by
// Evaluator messaging primitives.

import std;
aura::messaging::MessagingBridge aura::messaging::g_messaging_bridge;
aura::messaging::MailboxReadFn aura::messaging::g_mailbox_read = nullptr;
aura::messaging::MailboxSenderFn aura::messaging::g_mailbox_last_sender = nullptr;
void (*aura::messaging::g_fiber_block)() = nullptr;
aura::messaging::FiberSpawnFn aura::messaging::g_fiber_spawn = nullptr;
aura::messaging::FiberLookupFn aura::messaging::g_fiber_lookup = nullptr;
aura::messaging::FiberYieldFn aura::messaging::g_fiber_yield = nullptr;
aura::messaging::FiberYieldMutationFn aura::messaging::g_fiber_yield_mutation_boundary = nullptr;
// Issue #396 Phase 1: lightweight yield-reason setter for
// (mutate :atomic-batch) Guard entry.
aura::messaging::FiberSetYieldReasonMutationFn
    aura::messaging::g_fiber_set_yield_reason_mutation_boundary = nullptr;
aura::messaging::FiberJoinFn aura::messaging::g_fiber_join = nullptr;
std::function<aura::messaging::SessionCreateFn>* aura::messaging::g_session_create = nullptr;
aura::messaging::SessionListFn aura::messaging::g_session_list = nullptr;
aura::messaging::ResetArenaFn aura::messaging::g_reset_arena = nullptr;
aura::messaging::HttpPostAsyncFn aura::messaging::g_http_post_async = nullptr;
aura::messaging::DensenessBodyUnlockFn aura::messaging::g_denseness_body_unlock_for_wait = nullptr;
aura::messaging::DensenessBodyRelockFn aura::messaging::g_denseness_body_relock_after_wait =
    nullptr;
aura::messaging::CliMainBodyLockAcquireFn aura::messaging::g_cli_main_body_lock_acquire = nullptr;
aura::messaging::CliMainBodyLockReleaseFn aura::messaging::g_cli_main_body_lock_release = nullptr;
// Issue #4722: CLI thread-backend body identity (0 outside a body).
aura::messaging::CliBodyFiberIdFn aura::messaging::g_cli_body_fiber_id = nullptr;
// Issue #4711: eval-mutex release across a blocking wait (single lock order
// body mutex -> eval mutex). Wired by CompilerService::eval.
aura::messaging::EvalMutexWaitReleaseFn aura::messaging::g_eval_mutex_wait_release = nullptr;
aura::messaging::EvalMutexWaitReacquireFn aura::messaging::g_eval_mutex_wait_reacquire = nullptr;
aura::offload::OffloadEval* aura::messaging::g_offload_eval = nullptr;
// Issue #4888: offload:* bridge, registered by src/offload/offload_primitives.cpp
// (aura binary). Null in evaluator-only test targets.
aura::messaging::OffloadSubmitFn aura::messaging::g_offload_submit;
aura::messaging::OffloadAwaitFn aura::messaging::g_offload_await;
aura::messaging::OffloadPollFn aura::messaging::g_offload_poll;
aura::messaging::OffloadCancelFn aura::messaging::g_offload_cancel;
namespace {
thread_local bool t_offload_private_eval = false;
// Issue #4887: private CompilerService construction (offload/snapshot) must
// not republish the process-global stats impl table.
thread_local bool t_stats_registration_suppressed = false;
} // namespace
bool aura::messaging::offload_private_eval_active() noexcept {
    return t_offload_private_eval;
}
void aura::messaging::set_offload_private_eval(bool on) noexcept {
    t_offload_private_eval = on;
}
bool aura::compiler::set_stats_registration_suppressed(bool suppress) {
    const bool previous = t_stats_registration_suppressed;
    t_stats_registration_suppressed = suppress;
    return previous;
}
bool aura::compiler::stats_registration_suppressed() {
    return t_stats_registration_suppressed;
}
aura::messaging::GetMetricsFn aura::messaging::g_get_scheduler_metrics;
aura::messaging::ResetMetricsFn aura::messaging::g_reset_scheduler_metrics;
aura::messaging::FiberSetAffinityFn aura::messaging::g_fiber_set_affinity = nullptr;
aura::messaging::GCRootFlushFn aura::messaging::g_gc_flush_root_set;
aura::messaging::GCCollectFn aura::messaging::g_gc_collect;
aura::messaging::HeapMutexFn aura::messaging::g_heap_mutex;

// Issue #285: flush hook implementation.
aura::messaging::FlushMutationBoundaryFn aura::messaging::g_flush_mutation_boundary = nullptr;
// Issue #354: mutation-boundary-held check implementation
// (defaults to null; wired at static init by
// evaluator_fiber_mutation.cpp). Returns true when an
// outermost MutationBoundaryGuard is alive.
aura::messaging::MutationBoundaryHeldFn aura::messaging::g_mutation_boundary_held = nullptr;
// Issue #453: panic checkpoint lifecycle hooks (defaults to
// null; wired at static init by evaluator_fiber_mutation.cpp).
aura::messaging::PendingPanicCheckpointFn aura::messaging::g_pending_panic_checkpoint = nullptr;
aura::messaging::TransferPanicCheckpointFn aura::messaging::g_transfer_panic_checkpoint = nullptr;
aura::messaging::BlockGCForPendingCheckpointFn aura::messaging::g_block_gc_for_pending_checkpoint =
    nullptr;
aura::messaging::MailboxCountFn aura::messaging::g_mailbox_count = nullptr;
aura::messaging::SessionIdFn aura::messaging::g_session_id = nullptr;
aura::messaging::SessionExistsFn aura::messaging::g_session_exists = nullptr;
void* aura::messaging::g_current_compiler_service = nullptr;
