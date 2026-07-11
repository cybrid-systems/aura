// multi_fiber_mailbox.ixx — Issue #1211 Phase 1: typed MultiFiberMailbox scaffold.

module;

export module aura.serve.multi_fiber_mailbox;

import std;

export namespace aura::serve::mf_mailbox {

inline constexpr int kMultiFiberMailboxPhase = 1;

enum class MailPriority : std::uint8_t { Low = 0, Normal = 1, High = 2, Critical = 3 };

struct MailMessage {
    std::uint64_t from_fiber = 0;
    std::uint64_t to_fiber = 0; // 0 = broadcast
    MailPriority priority = MailPriority::Normal;
    std::string payload;
};

struct MultiFiberMailboxStats {
    std::uint64_t pushes = 0;
    std::uint64_t pops = 0;
    std::uint64_t broadcasts = 0;
    std::uint64_t priority_high = 0;
};

inline MultiFiberMailboxStats g_mf_mailbox_stats{};

// Phase 1: priority-aware queue stats. Full cross-fiber delivery peels into serve Mailbox.
struct MultiFiberMailbox {
    std::deque<MailMessage> queue;

    void push(MailMessage msg) {
        ++g_mf_mailbox_stats.pushes;
        if (msg.priority >= MailPriority::High)
            ++g_mf_mailbox_stats.priority_high;
        if (msg.to_fiber == 0)
            ++g_mf_mailbox_stats.broadcasts;
        // Critical / High go front; others back.
        if (msg.priority >= MailPriority::High)
            queue.push_front(std::move(msg));
        else
            queue.push_back(std::move(msg));
    }

    [[nodiscard]] bool try_pop(MailMessage& out) {
        if (queue.empty())
            return false;
        out = std::move(queue.front());
        queue.pop_front();
        ++g_mf_mailbox_stats.pops;
        return true;
    }
};

} // namespace aura::serve::mf_mailbox
