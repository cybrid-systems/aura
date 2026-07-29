// ──────────────────────────────────────────────────────────────
//  ast_pod_reflect.hh — Wave B3 small AST public PODs
//
//  Mirrors of aura::ast types that are plain public POD / containers
//  (no FlatAST SoA, no private members). Serialize with auto_serialize;
//  inspect with to_json. Layout must stay in sync with ast.ixx.
//
//  Covered:
//    SourceLocation, Patch, MatchClauseInfo,
//    NodeLifecycleStats, PostRestoreReport
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_AST_POD_REFLECT_HH
#define AURA_REFLECT_AST_POD_REFLECT_HH

#include "reflect/reflect.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace aura::ast_pod {

// Matches aura.core.mutation NodeId / SymId (uint32).
using NodeId = std::uint32_t;
using SymId = std::uint32_t;
inline constexpr NodeId kNullNode = ~0u;
inline constexpr SymId kInvalidSym = ~0u;

struct SourceLocation {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t file = 0;
};

struct Patch {
    NodeId node = kNullNode;
    std::uint32_t field_offset = 0;
    std::uint64_t new_value = 0;
};

struct MatchClauseInfo {
    std::vector<SymId> used_constructors;
    std::vector<SymId> candidate_constructors;
    bool has_wildcard = false;
    bool exhaustiveness_checked = false;
    std::uint32_t subject_type_id = 0;
};

struct NodeLifecycleStats {
    std::size_t total_slots = 0;
    std::size_t live_nodes = 0;
    std::size_t free_slots = 0;
    double fragmentation_ratio = 0.0;
};

struct PostRestoreReport {
    std::size_t violations = 0;
    std::uint16_t generation = 0;
    std::size_t live_nodes = 0;
    std::size_t free_slots = 0;
};

// ── Helpers ───────────────────────────────────────────────────

template <typename T> inline std::vector<char> pod_serialize(const T& obj) {
    return aura::reflect::auto_serialize(obj);
}

template <typename T> inline T pod_deserialize(const std::vector<char>& bytes) {
    return aura::reflect::auto_deserialize<T>(bytes);
}

template <typename T> inline std::string pod_json(const T& obj) {
    return aura::reflect::to_json(obj);
}

template <typename T> inline bool pod_validate(const T& obj, std::string* error = nullptr) {
    return aura::reflect::auto_validate(obj, error);
}

// Semantic checks beyond structural auto_validate.
inline bool validate_source_location(const SourceLocation& loc, std::string* error = nullptr) {
    if (!pod_validate(loc, error))
        return false;
    // line/column 0 is allowed (unknown); no upper clamp required for wire.
    (void)loc;
    return true;
}

inline bool validate_patch(const Patch& p, std::string* error = nullptr) {
    if (!pod_validate(p, error))
        return false;
    // node == kNullNode is a valid "empty" patch descriptor.
    if (p.field_offset > 1'000'000u) {
        if (error)
            *error = "Patch.field_offset too large";
        return false;
    }
    return true;
}

inline bool validate_match_clause(const MatchClauseInfo& m, std::string* error = nullptr) {
    if (!pod_validate(m, error))
        return false;
    if (m.used_constructors.size() > 1'000'000u || m.candidate_constructors.size() > 1'000'000u) {
        if (error)
            *error = "MatchClauseInfo constructor list too large";
        return false;
    }
    return true;
}

static_assert(aura::reflect::member_count<SourceLocation>() == 3);
static_assert(aura::reflect::member_count<Patch>() == 3);
static_assert(aura::reflect::member_count<MatchClauseInfo>() == 5);
static_assert(aura::reflect::member_count<NodeLifecycleStats>() == 4);
static_assert(aura::reflect::member_count<PostRestoreReport>() == 4);

} // namespace aura::ast_pod

#endif // AURA_REFLECT_AST_POD_REFLECT_HH
