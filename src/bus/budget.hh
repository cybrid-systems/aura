// ──────────────────────────────────────────────────────────────
//  budget.hh — task/global token, wall-clock and failure quotas (#5403)
//
//  The coordinator accumulates the usage a worker reports. Exceeding
//  any limit trips the task (or the whole workspace) and fails closed.
//  Defaults are bounded; there is no unlimited production default.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_BUDGET_HH
#define AURA_BUS_BUDGET_HH

#include <cstdint>
#include <map>
#include <string>

namespace aura::bus {

struct BudgetLimits {
    std::uint64_t token_limit = 2000000;
    std::uint64_t wall_limit_ms = 3600000;
    std::uint64_t failure_limit = 3;
};

struct BudgetPolicy {
    BudgetLimits task{};
    BudgetLimits global{20000000, 86400000, 20};
};

struct BudgetUsage {
    std::uint64_t tokens = 0;
    std::uint64_t wall_ms = 0;
    std::uint64_t failures = 0;
    std::uint64_t started_ms = 0;
    bool tripped = false;
    std::string reason;
};

class BudgetLedger {
public:
    void configure(const BudgetPolicy& policy) { policy_ = policy; }
    [[nodiscard]] const BudgetPolicy& policy() const { return policy_; }

    // The task was claimed: start its wall clock if this is the first time.
    void start(std::uint64_t task, std::uint64_t now_ms);
    // Add reported usage. Returns false and fills *why when a limit is
    // exceeded; the corresponding usage is marked tripped.
    bool charge(std::uint64_t task, std::uint64_t tokens, std::uint64_t wall_ms,
                std::uint64_t failures, std::uint64_t now_ms, BudgetUsage* task_out,
                BudgetUsage* global_out, std::string* why);
    [[nodiscard]] bool report(std::uint64_t task, BudgetUsage* out) const;
    [[nodiscard]] bool global_report(BudgetUsage* out) const {
        *out = global_;
        return true;
    }
    [[nodiscard]] bool global_tripped() const { return global_.tripped; }
    [[nodiscard]] std::uint64_t task_started(std::uint64_t task) const;
    // #5440: re-evaluate the restored usage against the current policy.
    // A raised limit clears a trip it no longer justifies, and an expired
    // global wall-clock window rolls to now instead of tripping forever.
    void reconcile(std::uint64_t now_ms);
    // #5427: re-apply persisted usage (checkpoint replay). task 0 is the
    // global ledger.
    void restore(std::uint64_t task, const BudgetUsage& usage) {
        if (task == 0)
            global_ = usage;
        else
            tasks_[task] = usage;
    }
    [[nodiscard]] const std::map<std::uint64_t, BudgetUsage>& all_tasks() const { return tasks_; }
    // #5460: the task left the retention window; drop its usage.
    void forget(std::uint64_t task) { tasks_.erase(task); }

private:
    bool check(const BudgetLimits& limits, BudgetUsage* usage, std::uint64_t now_ms,
               std::string* why, const char* scope);
    [[nodiscard]] bool over_limit(const BudgetLimits& limits, const BudgetUsage& usage,
                                  std::uint64_t now_ms, const char* scope) const;
    void reevaluate(const BudgetLimits& limits, BudgetUsage* usage, std::uint64_t now_ms,
                    bool global);
    BudgetPolicy policy_{};
    std::map<std::uint64_t, BudgetUsage> tasks_;
    BudgetUsage global_{};
};

} // namespace aura::bus

#endif
