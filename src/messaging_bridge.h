#ifndef AURA_MESSAGING_BRIDGE_H
#define AURA_MESSAGING_BRIDGE_H

#include <string>
#include <optional>
#include <functional>

// ── Messaging Bridge ───────────────────────────────────────────
//
// Cross-module function pointers for inter-agent messaging.
// Set by CompilerService (module aura.compiler.service) and
// read by Evaluator primitives (module aura.compiler.evaluator).
//
// This header is intentionally NOT in a module to avoid circular
// dependency issues.

namespace aura::messaging {

// Fiber blocking callback — set by serve_async.cpp, used by pop_message
// for async recv yield. Null means non-blocking fallback.
extern void (*g_fiber_block)();

// Function pointer types
using SendFn = bool (*)(const std::string& target, const std::string& msg);
using RecvFn = std::optional<std::string> (*)(int timeout_ms);
using MyIdFn = std::string (*)();

// Global bridge — set once by CompilerService during construction
struct MessagingBridge {
    SendFn send = nullptr;
    RecvFn recv = nullptr;
    MyIdFn my_id = nullptr;
};

// The global bridge instance
extern MessagingBridge g_messaging_bridge;

// Fiber spawn — set by serve_async.cpp, used by evaluator
// Returns 0 on failure, non-zero fiber ID on success
using FiberSpawnFn = int64_t (*)(void (*fn)(void*), void* arg);
extern FiberSpawnFn g_fiber_spawn;

// Session create — set by serve_async.cpp, used by evaluator
// Returns true on success
using SessionCreateFn = bool(const std::string& name);
extern std::function<SessionCreateFn>* g_session_create;


// Access a CompilerService's mailbox through a void*.
// Since evaluator can't import the CompilerService module,
// we provide a function that the CompilerService registers.
using MailboxReadFn = std::optional<std::string> (*)(void* compiler_service, int timeout_ms);
using MailboxSenderFn = std::string (*)(void* compiler_service);  // sender of last read
using MailboxCountFn = std::size_t (*)(void* compiler_service);
using SessionIdFn = std::string (*)(void* compiler_service);
using SessionExistsFn = bool (*)(const std::string& id);

// These are set by CompilerService during construction
extern MailboxReadFn g_mailbox_read;
extern MailboxSenderFn g_mailbox_last_sender;
extern MailboxCountFn g_mailbox_count;
extern SessionIdFn g_session_id;
extern SessionExistsFn g_session_exists;
extern void* g_current_compiler_service;  // set before each eval in serve

// Arena reset callback — set by CompilerService, called by Evaluator
// to reclaim arena memory between benchmark tasks.
// Takes a pointer to the service, returns void.
using ResetArenaFn = void(*)(void* compiler_service);
extern ResetArenaFn g_reset_arena;

// Async HTTP callback — set by serve_async.cpp, used by http-post primitive.
// When set, http-post uses thread + eventfd for non-blocking HTTP.
// Returns the response string (or "" on failure).
using HttpPostAsyncFn = std::string (*)(const std::string& url, const std::string& body,
                                         const std::string& auth);
extern HttpPostAsyncFn g_http_post_async;

}  // namespace aura::messaging

#endif  // AURA_MESSAGING_BRIDGE_H
