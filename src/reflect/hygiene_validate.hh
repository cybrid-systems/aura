// ──────────────────────────────────────────────────────────────
//  hygiene_validate.hh — Issue #1611 SyntaxMarker hygiene gates
//
//  Header-only, zero reflection / module dependencies.
//  Usable from evaluator partitions and ordinary test TUs without
//  -freflection. reflect.hh re-exports the same symbols.
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_HYGIENE_VALIDATE_HH
#define AURA_REFLECT_HYGIENE_VALIDATE_HH

#include <cstdint>
#include <string>

namespace aura::reflect {

// Mirrors aura::ast::SyntaxMarker without depending on the AST module.
enum class HygieneMarker : std::uint8_t {
    User = 0,
    MacroIntroduced = 1,
    BoolLiteral = 2,
};

// Default: MacroIntroduced requires explicit allow_macro_evolution.
inline bool hygiene_allows_evolution(HygieneMarker context_marker, bool allow_macro_evolution,
                                     std::string* error = nullptr) noexcept {
    if (context_marker == HygieneMarker::MacroIntroduced && !allow_macro_evolution) {
        if (error)
            *error = "macro-introduced schema evolution requires explicit allow "
                     "(:allow-macro? #t or hygiene:set-allow-macro-mutate! #t)";
        return false;
    }
    return true;
}

// Mutation-specific reflect health probe for Guard post-mutate snapshots.
struct MutationReflectHealth {
    bool marker_consistent = true;
    bool generation_healthy = true;
    std::uint64_t dirty_nodes = 0;
    std::uint64_t macro_markers = 0;
    std::uint64_t dirty_macro_nodes = 0;
    bool allow_macro_evolution = false;
    // When true, dirty_macro_nodes without allow is a hard hygiene reject.
    bool enforce_macro_hygiene_reject = false;
};

inline bool validate_mutation_reflect_health(const MutationReflectHealth& health,
                                             std::string* error = nullptr) {
    if (!health.generation_healthy) {
        if (error)
            *error = "generation health check failed";
        return false;
    }
    if (!health.marker_consistent) {
        if (error)
            *error = "macro marker consistency check failed";
        return false;
    }
    if (health.enforce_macro_hygiene_reject && health.dirty_macro_nodes > 0 &&
        !health.allow_macro_evolution) {
        if (error)
            *error = "macro-introduced schema evolution requires explicit allow";
        return false;
    }
    (void)health.dirty_nodes;
    (void)health.macro_markers;
    return true;
}

// Phase-4 module deserialize path hygiene gate.
inline bool validate_deserialize_hygiene(HygieneMarker payload_marker, bool allow_macro_evolution,
                                         std::string* error = nullptr) noexcept {
    return hygiene_allows_evolution(payload_marker, allow_macro_evolution, error);
}

} // namespace aura::reflect

#endif // AURA_REFLECT_HYGIENE_VALIDATE_HH
