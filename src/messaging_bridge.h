#ifndef AURA_MESSAGING_BRIDGE_H
#define AURA_MESSAGING_BRIDGE_H

#include <string>
#include <optional>

// ── Messaging Bridge ───────────────────────────────────────────
//
// Cross-module function pointers for inter-agent messaging.
// Set by CompilerService (module aura.compiler.service) and
// read by Evaluator primitives (module aura.compiler.evaluator).
//
// This header is intentionally NOT in a module to avoid circular
// dependency issues.

namespace aura::messaging {

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


// Access a CompilerService's mailbox through a void*.
// Since evaluator can't import the CompilerService module,
// we provide a function that the CompilerService registers.
using MailboxReadFn = std::optional<std::string> (*)(void* compiler_service, int timeout_ms);
using SessionIdFn = std::string (*)(void* compiler_service);

// These are set by CompilerService during construction
extern MailboxReadFn g_mailbox_read;
extern SessionIdFn g_session_id;
extern void* g_current_compiler_service;  // set before each eval in serve

}  // namespace aura::messaging

#endif  // AURA_MESSAGING_BRIDGE_H
