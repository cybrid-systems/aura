// typed_mutation_audit_pass.ixx — Issue #1216 Phase 1: TypedMutationAuditPass scaffold.

module;

export module aura.compiler.typed_mutation_audit_pass;

import std;

export namespace aura::compiler::typed_audit {

inline constexpr int kTypedMutationAuditPassPhase = 1;

enum class AuditStrategy : std::uint8_t {
    Off = 0,
    Sampled = 1,
    Full = 2,
};

struct TypedMutationAuditStats {
    std::uint64_t audits = 0;
    std::uint64_t samples_skipped = 0;
    AuditStrategy strategy = AuditStrategy::Sampled;
};

inline TypedMutationAuditStats g_typed_mutation_audit_stats{};

// Phase 1 strategy gate; full Pass concept peel wires into pass_manager.
[[nodiscard]] inline bool should_audit(std::uint64_t mutation_id) noexcept {
    ++g_typed_mutation_audit_stats.audits;
    if (g_typed_mutation_audit_stats.strategy == AuditStrategy::Off)
        return false;
    if (g_typed_mutation_audit_stats.strategy == AuditStrategy::Full)
        return true;
    // Sampled: every 4th mutation
    if ((mutation_id & 3u) != 0) {
        ++g_typed_mutation_audit_stats.samples_skipped;
        return false;
    }
    return true;
}

} // namespace aura::compiler::typed_audit
