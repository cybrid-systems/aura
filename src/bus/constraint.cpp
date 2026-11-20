// ──────────────────────────────────────────────────────────────
//  constraint.cpp — constraint gate (#5126)
// ──────────────────────────────────────────────────────────────
#include "bus/constraint.hh"

#include <utility>

namespace aura::bus {

GateResult ConstraintStore::check(const ConstraintContext& ctx, AuditLog* audit,
                                  std::uint64_t token) {
    GateResult result{};
    result.pass = true;
    result.version = current_.version;
    for (const auto& rule : current_.rules) {
        std::string why;
        if (rule.check(ctx, &why))
            continue;
        result.pass = false;
        result.code = 1;
        result.reason = rule.name.empty() ? why : rule.name + ": " + why;
        audit->add_constraint(AuditKind::ConstraintResult, token, ctx.worldline, 0,
                              current_.version, result.reason);
        return result;
    }
    audit->add_constraint(AuditKind::ConstraintResult, token, ctx.worldline, 1, current_.version,
                          {});
    return result;
}

bool ConstraintStore::install(ConstraintSet set, const Capability& caps, AuditLog* audit,
                              std::uint64_t token, std::string* why) {
    if ((caps.bits & kCapConstraintWrite) == 0) {
        if (why)
            *why = "missing capability: constraint-write";
        return false;
    }
    // A rule-only install keeps the current allow policy; a set carrying a
    // policy replaces it atomically with the rules.
    if (set.policy.empty())
        set.policy = current_.policy;
    set.version = current_.version + 1;
    history_.push_back(current_);
    current_ = std::move(set);
    audit->add(AuditKind::ConstraintChange, token, 0, current_.version);
    return true;
}

bool ConstraintStore::rollback(const Capability& caps, AuditLog* audit, std::uint64_t token,
                               std::string* why) {
    if ((caps.bits & kCapConstraintWrite) == 0) {
        if (why)
            *why = "missing capability: constraint-write";
        return false;
    }
    if (history_.empty()) {
        if (why)
            *why = "no previous constraint set";
        return false;
    }
    current_ = history_.back();
    history_.pop_back();
    audit->add(AuditKind::ConstraintRollback, token, 0, current_.version);
    return true;
}

} // namespace aura::bus
