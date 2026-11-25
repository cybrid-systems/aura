// ──────────────────────────────────────────────────────────────
//  budget.cpp — task/global quotas (#5403)
// ──────────────────────────────────────────────────────────────
#include "bus/budget.hh"

#include <string_view>

namespace aura::bus {

void BudgetLedger::start(std::uint64_t task, std::uint64_t now_ms) {
    if (global_.started_ms == 0)
        global_.started_ms = now_ms;
    BudgetUsage& usage = tasks_[task];
    if (usage.started_ms == 0)
        usage.started_ms = now_ms;
}

bool BudgetLedger::over_limit(const BudgetLimits& limits, const BudgetUsage& usage,
                              std::uint64_t now_ms, const char* scope) const {
    if (limits.token_limit != 0 && usage.tokens > limits.token_limit)
        return true;
    if (limits.failure_limit != 0 && usage.failures > limits.failure_limit)
        return true;
    if (limits.wall_limit_ms != 0 && usage.wall_ms > limits.wall_limit_ms)
        return true;
    // The task wall clock is a lifetime bound. The global wall clock is a
    // rolling window; charge() rolls it before checking, so its age alone
    // must not trip here.
    if (std::string_view(scope) != "global" && limits.wall_limit_ms != 0 && usage.started_ms != 0 &&
        now_ms >= usage.started_ms && now_ms - usage.started_ms > limits.wall_limit_ms)
        return true;
    return false;
}

bool BudgetLedger::check(const BudgetLimits& limits, BudgetUsage* usage, std::uint64_t now_ms,
                         std::string* why, const char* scope) {
    if (usage->tripped)
        return false;
    if (over_limit(limits, *usage, now_ms, scope)) {
        usage->tripped = true;
        usage->reason = std::string(scope) +
                        (limits.token_limit != 0 && usage->tokens > limits.token_limit
                             ? " token budget exceeded"
                         : limits.failure_limit != 0 && usage->failures > limits.failure_limit
                             ? " failure budget exceeded"
                             : " wall-clock budget exceeded");
    }
    if (usage->tripped) {
        if (why)
            *why = usage->reason;
        return false;
    }
    return true;
}

void BudgetLedger::reevaluate(const BudgetLimits& limits, BudgetUsage* usage, std::uint64_t now_ms,
                              bool global) {
    if (global && limits.wall_limit_ms != 0 && usage->started_ms != 0 &&
        now_ms >= usage->started_ms && now_ms - usage->started_ms > limits.wall_limit_ms) {
        *usage = BudgetUsage{};
        usage->started_ms = now_ms;
    }
    if (over_limit(limits, *usage, now_ms, global ? "global" : "task")) {
        usage->tripped = true;
        usage->reason = std::string(global ? "global" : "task") +
                        (limits.token_limit != 0 && usage->tokens > limits.token_limit
                             ? " token budget exceeded"
                         : limits.failure_limit != 0 && usage->failures > limits.failure_limit
                             ? " failure budget exceeded"
                             : " wall-clock budget exceeded");
    } else {
        usage->tripped = false;
        usage->reason.clear();
    }
}

void BudgetLedger::reconcile(std::uint64_t now_ms) {
    for (auto& [task, usage] : tasks_) {
        (void)task;
        reevaluate(policy_.task, &usage, now_ms, false);
    }
    reevaluate(policy_.global, &global_, now_ms, true);
}

bool BudgetLedger::charge(std::uint64_t task, std::uint64_t tokens, std::uint64_t wall_ms,
                          std::uint64_t failures, std::uint64_t now_ms, BudgetUsage* task_out,
                          BudgetUsage* global_out, std::string* why) {
    BudgetUsage& usage = tasks_[task];
    if (usage.started_ms == 0)
        usage.started_ms = now_ms;
    if (global_.started_ms == 0)
        global_.started_ms = now_ms;
    // #5440: the global wall clock is a rolling window. Once the window
    // has elapsed, start a fresh one before counting this charge; a long
    // uptime must not turn the global budget into a permanent trip.
    if (policy_.global.wall_limit_ms != 0 && global_.started_ms != 0 &&
        now_ms >= global_.started_ms &&
        now_ms - global_.started_ms > policy_.global.wall_limit_ms) {
        global_ = BudgetUsage{};
        global_.started_ms = now_ms;
    }
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
