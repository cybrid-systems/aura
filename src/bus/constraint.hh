// ──────────────────────────────────────────────────────────────
//  constraint.hh — versioned constraint gate and audit (#5126)
//
//  The coordinator holds executable constraints. Every KEEP runs the
//  current set; a failure rejects the keep and records the reason.
//  Installing or rolling back a set needs constraint-write and is
//  audited separately.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_CONSTRAINT_HH
#define AURA_BUS_CONSTRAINT_HH

#include "bus/capability.hh"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aura::bus {

struct ConstraintContext {
    std::uint64_t worldline = 0;
    std::uint64_t node = 0;
    std::uint64_t new_value = 0;
};

using ConstraintPredicate = std::function<bool(const ConstraintContext&, std::string*)>;

struct ConstraintRule {
    std::string name;
    ConstraintPredicate check;
};

struct ConstraintSet {
    std::uint64_t version = 1;
    std::vector<ConstraintRule> rules;
};

struct GateResult {
    bool pass = true;
    std::uint32_t code = 0;
    std::string reason;
    std::uint64_t version = 1;
};

class ConstraintStore {
public:
    // Runs every rule. A failure is recorded in the audit with the
    // worldline and reason; the live workspace is untouched.
    GateResult check(const ConstraintContext& ctx, AuditLog* audit, std::uint64_t token);

    // Requires constraint-write. The new version is current+1 and the
    // previous set is retained for rollback.
    bool install(ConstraintSet set, const Capability& caps, AuditLog* audit, std::uint64_t token,
                 std::string* why);
    bool rollback(const Capability& caps, AuditLog* audit, std::uint64_t token, std::string* why);

    [[nodiscard]] std::uint64_t version() const { return current_.version; }
    [[nodiscard]] std::size_t rule_count() const { return current_.rules.size(); }

private:
    ConstraintSet current_{};
    std::vector<ConstraintSet> history_;
};

} // namespace aura::bus

#endif
