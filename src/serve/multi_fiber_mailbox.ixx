// multi_fiber_mailbox.ixx — Issue #1211 / #1585
// Module inventory surface. Full production API (multi-attach, blocking
// recv, broadcast, backpressure) lives in multi_fiber_mailbox.h so Fiber
// can be included without circular module deps.

module;

export module aura.serve.multi_fiber_mailbox;

import std;

export namespace aura::serve::mf_mailbox {

inline constexpr int kMultiFiberMailboxPhase = 2; // #1585
inline constexpr int kMultiFiberMailboxIssue = 1585;

// Re-export priority enum for module importers (header has full class).
enum class MailPriority : std::uint8_t { Low = 0, Normal = 1, High = 2, Critical = 3 };

} // namespace aura::serve::mf_mailbox
