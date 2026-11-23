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
#include <string_view>
#include <unordered_map>
#include <vector>
#include "bus/image_format.hh"

#include <span>

namespace aura::bus {

struct ConstraintContext {
    std::uint64_t worldline = 0;
    std::uint64_t node = 0;
    std::uint64_t new_value = 0;
    // #5380: the trial's committed patch records, so structural rules
    // (protected set, target allowlist, purity) can inspect what a KEEP
    // would actually apply. Empty for legacy immediate-only keeps.
    std::span<const PatchRecord> patches{};
    // #5380: the candidate define source texts, parallel to patches
    // (empty for immediate field patches). Purity rules scan these.
    std::span<const std::string> sources{};
};

using ConstraintPredicate = std::function<bool(const ConstraintContext&, std::string*)>;

// #5401: fail-closed parameterized rules. An empty or unparsable source
// yields a predicate that refuses the operation it guards; a valid
// source yields the executable allowlist / floor.
ConstraintPredicate target_allowlist_predicate(std::string_view source);
ConstraintPredicate metric_floor_predicate(std::string_view source);

struct ConstraintRule {
    std::string name;
    ConstraintPredicate check;
    // Optional Aura EDSL source for this rule. The coordinator keeps the
    // compiled predicate for enforcement and stores the source in the
    // image constraints section so other tools can read the rule (#5126).
    std::string source;
};

struct ConstraintSet {
    std::uint64_t version = 1;
    std::vector<ConstraintRule> rules;
    // Workspace allow policy is part of the same versioned set; a rollback
    // restores both the rules and the policy (#5126).
    WorkspacePolicy policy{};
    // #5396: who installed each rule. A change may not drop a rule
    // another session installed — that is how a rollback would relax
    // the gate behind the owner's back.
    std::unordered_map<std::string, std::uint64_t> installers;
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
    [[nodiscard]] const ConstraintSet& current() const { return current_; }
    [[nodiscard]] const WorkspacePolicy& policy() const { return current_.policy; }
    void set_initial_policy(WorkspacePolicy policy) { current_.policy = std::move(policy); }

private:
    ConstraintSet current_{};
    std::vector<ConstraintSet> history_;
};

} // namespace aura::bus

#endif
