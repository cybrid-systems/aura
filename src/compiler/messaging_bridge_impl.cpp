#include "messaging_bridge.h"

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
// (mutate:atomic-batch) Guard entry.
aura::messaging::FiberSetYieldReasonMutationFn aura::messaging::g_fiber_set_yield_reason_mutation_boundary = nullptr;
aura::messaging::FiberJoinFn aura::messaging::g_fiber_join = nullptr;
std::function<aura::messaging::SessionCreateFn>* aura::messaging::g_session_create = nullptr;
aura::messaging::SessionListFn aura::messaging::g_session_list = nullptr;
aura::messaging::ResetArenaFn aura::messaging::g_reset_arena = nullptr;
aura::messaging::HttpPostAsyncFn aura::messaging::g_http_post_async = nullptr;
aura::messaging::ThreadPoolEnqueueFn aura::messaging::g_thread_pool_enqueue = nullptr;
aura::messaging::EvalAsyncFn aura::messaging::g_eval_async = nullptr;
aura::messaging::GetMetricsFn aura::messaging::g_get_scheduler_metrics;
aura::messaging::ResetMetricsFn aura::messaging::g_reset_scheduler_metrics;
aura::messaging::FiberSetAffinityFn aura::messaging::g_fiber_set_affinity = nullptr;
aura::messaging::GCRootFlushFn aura::messaging::g_gc_flush_root_set;
aura::messaging::GCCollectFn aura::messaging::g_gc_collect;
aura::messaging::GCSweepFn aura::messaging::g_gc_sweep;
aura::messaging::HeapMutexFn aura::messaging::g_heap_mutex;

// Issue #285: flush hook implementation.
aura::messaging::FlushMutationBoundaryFn
    aura::messaging::g_flush_mutation_boundary = nullptr;
// Issue #354: mutation-boundary-held check implementation
// (defaults to null; wired at static init by
// evaluator_fiber_mutation.cpp). Returns true when an
// outermost MutationBoundaryGuard is alive.
aura::messaging::MutationBoundaryHeldFn
    aura::messaging::g_mutation_boundary_held = nullptr;
// Issue #453: panic checkpoint lifecycle hooks (defaults to
// null; wired at static init by evaluator_fiber_mutation.cpp).
aura::messaging::PendingPanicCheckpointFn
    aura::messaging::g_pending_panic_checkpoint = nullptr;
aura::messaging::TransferPanicCheckpointFn
    aura::messaging::g_transfer_panic_checkpoint = nullptr;
aura::messaging::BlockGCForPendingCheckpointFn
    aura::messaging::g_block_gc_for_pending_checkpoint = nullptr;
aura::messaging::MailboxCountFn aura::messaging::g_mailbox_count = nullptr;
aura::messaging::SessionIdFn aura::messaging::g_session_id = nullptr;
aura::messaging::SessionExistsFn aura::messaging::g_session_exists = nullptr;
void* aura::messaging::g_current_compiler_service = nullptr;
