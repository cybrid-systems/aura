#include "messaging_bridge.h"

// ── Messaging bridge globals (non-module .cpp for linker visibility) ──
// These are set by CompilerService during construction and read by
// Evaluator messaging primitives.
aura::messaging::MessagingBridge aura::messaging::g_messaging_bridge;
aura::messaging::MailboxReadFn aura::messaging::g_mailbox_read = nullptr;
aura::messaging::MailboxSenderFn aura::messaging::g_mailbox_last_sender = nullptr;
void (*aura::messaging::g_fiber_block)() = nullptr;
aura::messaging::FiberSpawnFn aura::messaging::g_fiber_spawn = nullptr;
aura::messaging::FiberYieldFn aura::messaging::g_fiber_yield = nullptr;
aura::messaging::FiberYieldMutationFn aura::messaging::g_fiber_yield_mutation_boundary = nullptr;
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
aura::messaging::MailboxCountFn aura::messaging::g_mailbox_count = nullptr;
aura::messaging::SessionIdFn aura::messaging::g_session_id = nullptr;
aura::messaging::SessionExistsFn aura::messaging::g_session_exists = nullptr;
void* aura::messaging::g_current_compiler_service = nullptr;
