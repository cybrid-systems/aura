// ──────────────────────────────────────────────────────────────
//  budget.cpp — task/global quotas (#5403)
// ──────────────────────────────────────────────────────────────
#include "bus/budget.hh"

namespace aura::bus {

void BudgetLedger::start(std::uint64_t task, std::uint64_t now_ms) {
    if (global_.started_ms == 0)
        global_.started_ms = now_ms;
    BudgetUsage& usage = tasks_[task];
    if (usage.started_ms == 0)
        usage.started_ms = now_ms;
}

bool BudgetLedger::check(const BudgetLimits& limits, BudgetUsage* usage, std::uint64_t now_ms,
                         std::string* why, const char* scope) {
    if (usage->tripped)
        return false;
    if (limits.token_limit != 0 && usage->tokens > limits.token_limit) {
        usage->tripped = true;
        usage->reason = std::string(scope) + " token budget exceeded";
    } else if (limits.failure_limit != 0 && usage->failures > limits.failure_limit) {
        usage->tripped = true;
        usage->reason = std::string(scope) + " failure budget exceeded";
    } else if (limits.wall_limit_ms != 0 && usage->started_ms != 0 && now_ms >= usage->started_ms &&
               now_ms - usage->started_ms > limits.wall_limit_ms) {
        usage->tripped = true;
        usage->reason = std::string(scope) + " wall-clock budget exceeded";
    } else if (limits.wall_limit_ms != 0 && usage->wall_ms > limits.wall_limit_ms) {
        usage->tripped = true;
        usage->reason = std::string(scope) + " wall-clock budget exceeded";
    }
    if (usage->tripped) {
        if (why)
            *why = usage->reason;
        return false;
    }
    return true;
}

bool BudgetLedger::charge(std::uint64_t task, std::uint64_t tokens, std::uint64_t wall_ms,
                          std::uint64_t failures, std::uint64_t now_ms, BudgetUsage* task_out,
                          BudgetUsage* global_out, std::string* why) {
    BudgetUsage& usage = tasks_[task];
    if (usage.started_ms == 0)
        usage.started_ms = now_ms;
    if (global_.started_ms == 0)
        global_.started_ms = now_ms;
    usage.tokens += tokens;
    usage.wall_ms += wall_ms;
    usage.failures += failures;
    global_.tokens += tokens;
    global_.wall_ms += wall_ms;
    global_.failures += failures;

    const bool task_ok = check(policy_.task, &usage, now_ms, why, "task");
    const bool global_ok = check(policy_.global, &global_, now_ms, why, "global");
    if (task_out)
        *task_out = usage;
    if (global_out)
        *global_out = global_;
    return task_ok && global_ok;
}

bool BudgetLedger::report(std::uint64_t task, BudgetUsage* out) const {
    if (task == 0) {
        if (out)
            *out = global_;
        return true;
    }
    const auto it = tasks_.find(task);
    if (it == tasks_.end()) {
        if (out)
            *out = BudgetUsage{};
        return false;
    }
    if (out)
        *out = it->second;
    return true;
}

std::uint64_t BudgetLedger::task_started(std::uint64_t task) const {
    const auto it = tasks_.find(task);
    return it == tasks_.end() ? 0 : it->second.started_ms;
}

} // namespace aura::bus
