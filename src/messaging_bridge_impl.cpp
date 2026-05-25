#include "messaging_bridge.h"

// ── Messaging bridge globals (non-module .cpp for linker visibility) ──
// These are set by CompilerService during construction and read by
// Evaluator messaging primitives.
aura::messaging::MessagingBridge aura::messaging::g_messaging_bridge;
aura::messaging::MailboxReadFn aura::messaging::g_mailbox_read = nullptr;
aura::messaging::SessionIdFn aura::messaging::g_session_id = nullptr;
void* aura::messaging::g_current_compiler_service = nullptr;
