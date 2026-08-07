// flatast_domains.hh — FlatAST decomposition map (architecture review).
//
// FlatAST is intentionally still one class (~8.5k lines) for ABI / lock-
// order cohesion, but its *logical* domains are fixed. Agents and humans
// assess blast radius against these domains, not the whole class.
//
// Decomposition steps (do not skip order without a design note):
//
//   Step 1 (done): freestanding SSOT extracted from ast.ixx
//     - flatast_restamp.hh     RestampPolicy + env resolvers
//     - owned_shared_mutex.hh  OwnedSharedMutex (SoA / structural / meta)
//     - this file              domain inventory + remaining plan
//
//   Step 2: mutation *pipeline* (visitors / fold) out of ast.ixx
//     - aura.core.ast_mutation_pipeline  (imports ast; no reverse edge)
//     - MutationVisitor, run_mutation_pipeline, example visitors
//
//   Step 3 (done): dirty / generation method bodies → ast_impl.cpp
//     - mark_dirty*, mark_dirty_upward*, restamp_*, bump_generation*
//     - private columns remain on FlatAST (no layout break)
//
//   Step 4: storage / index friends only if step 3 proves stable
//     - Optional FlatASTStorage / FlatASTIndex *views* (not ownership)
//     - Never split locks across types without lock-order rewrite
//
// Domain inventory (what may depend on what):
//
//   Storage     SoA columns, PCV children, free-list, StringPool link
//   Concurrency flatast_mutex_ | structural_mtx_ | metadata_mtx_ |
//               dirty_column_mtx_  (#2418 order: structural before meta;
//               flatast exclusive before dirty_column)
//   Dirty       ppa/verify/macro dirty bits, generation_, wrap, restamp
//   Mutation    mutation log, rollback, StableNodeRef, atomic batch
//   Indexes     tag/arity inverted index, multi-parent edges
//   Meta        marker_, provenance_, type_id_, error kind
//
// Cross-domain rules:
//   - Mutation may mark Dirty; Dirty never owns Mutation log layout.
//   - Storage grows only under flatast_mutex_ exclusive.
//   - Concurrency docs live on FlatAST (Issue #2413 banner) — single truth.
//
// Issue stamp for dashboards / agents tracking the split program.

#ifndef AURA_CORE_FLATAST_DOMAINS_HH
#define AURA_CORE_FLATAST_DOMAINS_HH

#include <cstdint>

namespace aura::ast::domains {

inline constexpr int kFlatAstDecomposeIssue = 0; // program; no single GH issue
inline constexpr int kFlatAstDecomposeStep = 3;  // current completed step
inline constexpr int kFlatAstDecomposeStepCount = 4;

enum class FlatAstDomain : std::uint8_t {
    Storage = 0,
    Concurrency = 1,
    Dirty = 2,
    Mutation = 3,
    Indexes = 4,
    Meta = 5,
};

inline constexpr const char* domain_name(FlatAstDomain d) noexcept {
    switch (d) {
        case FlatAstDomain::Storage:
            return "storage";
        case FlatAstDomain::Concurrency:
            return "concurrency";
        case FlatAstDomain::Dirty:
            return "dirty";
        case FlatAstDomain::Mutation:
            return "mutation";
        case FlatAstDomain::Indexes:
            return "indexes";
        case FlatAstDomain::Meta:
            return "meta";
    }
    return "unknown";
}

} // namespace aura::ast::domains

#endif // AURA_CORE_FLATAST_DOMAINS_HH
