// typed_mutation_audit_pass.ixx — Issue #1589 / #1216: TypedMutationAuditPass inventory.
// Production API lives in typed_mutation_audit.h (thread-safe trail + capture).

module;

export module aura.compiler.typed_mutation_audit_pass;

import std;

export namespace aura::compiler::typed_audit {

// Production phase (header: typed_mutation_audit.h).
// Phase 4 = #1894 hotpath contextual gate + Full force-rollback.
// Phase 3 = #1614 real invariant enforcement (type + linear + provenance).
inline constexpr int kTypedMutationAuditPassPhase = 4;
inline constexpr int kTypedMutationAuditIssue = 1894;

// Scaffold types retained for module consumers / Phase-1 sweep.
// Production gate: typed_mutation_audit.h::should_audit /
// should_audit_contextual (wired from MutationBoundaryGuard exit).
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

inline TypedMutationAuditStats g_typed_mutation_audit_stats_module{};

// Thin module-side gate. Mirrors Sampled every-4th for module-only
// consumers; production hot path uses typed_mutation_audit.h.
[[nodiscard]] inline bool should_audit_scaffold(std::uint64_t mutation_id) noexcept {
    ++g_typed_mutation_audit_stats_module.audits;
    if (g_typed_mutation_audit_stats_module.strategy == AuditStrategy::Off)
        return false;
    if (g_typed_mutation_audit_stats_module.strategy == AuditStrategy::Full)
        return true;
    if ((mutation_id & 3u) != 0) {
        ++g_typed_mutation_audit_stats_module.samples_skipped;
        return false;
    }
    return true;
}

// Issue #1894: DirtyAwarePass inventory marker (actual enforcement is
// Guard-exit hot path, not IR pipeline — no IR mutation log to walk).
inline constexpr bool kTypedMutationAuditDirtyAware = true;

} // namespace aura::compiler::typed_audit
