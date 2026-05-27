#include "messaging_bridge.h"

// ── Messaging bridge globals (non-module .cpp for linker visibility) ──
// These are set by CompilerService during construction and read by
// Evaluator messaging primitives.
aura::messaging::MessagingBridge aura::messaging::g_messaging_bridge;
aura::messaging::MailboxReadFn aura::messaging::g_mailbox_read = nullptr;
aura::messaging::MailboxSenderFn aura::messaging::g_mailbox_last_sender = nullptr;
void (*aura::messaging::g_fiber_block)() = nullptr;
aura::messaging::FiberSpawnFn aura::messaging::g_fiber_spawn = nullptr;
std::function<aura::messaging::SessionCreateFn>* aura::messaging::g_session_create = nullptr;
aura::messaging::ResetArenaFn aura::messaging::g_reset_arena = nullptr;
aura::messaging::HttpPostAsyncFn aura::messaging::g_http_post_async = nullptr;
aura::messaging::MailboxCountFn aura::messaging::g_mailbox_count = nullptr;
aura::messaging::SessionIdFn aura::messaging::g_session_id = nullptr;
aura::messaging::SessionExistsFn aura::messaging::g_session_exists = nullptr;
void* aura::messaging::g_current_compiler_service = nullptr;
