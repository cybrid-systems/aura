// ast_mutation_pipeline.ixx — FlatAST mutation visitor pipeline (Issue #274).
//
// Extracted from ast.ixx as FlatAST decomposition step 2 of 4.
// Lives in a separate module so aura.core.ast shrinks and mutation-fold
// consumers (type_checker, service) can import the pipeline without
// depending on future storage-only partitions.
//
// Dependency: imports aura.core.ast (FlatAST + MutationRecord). Must NOT
// be re-exported from aura.core.ast (would cycle).

module;

#include <span>

export module aura.core.ast_mutation_pipeline;

import std;
import aura.core.ast;
import aura.core.mutation;

export namespace aura::ast {

// ── MutationVisitor concept (Issue #274) ─────────────────────
//
// Mirrors the Pass / AnalysisPass pattern in pass_manager.ixx,
// but for FlatAST mutation records instead of IRModule transforms.
// Visitors observe or react to committed mutations; the pipeline
// folds over the mutation log with short-circuit on has_error().
template <typename V>
concept MutationVisitor = requires(V& v, FlatAST& flat, const MutationRecord& rec) {
    { v.visit_mutation(flat, rec) } -> std::same_as<void>;
    { v.has_error() } -> std::convertible_to<bool>;
};

// Pure-function mutation callbacks (no persistent visitor state).
template <typename Fn>
concept PureMutationFn = requires(Fn& fn, FlatAST& flat, const MutationRecord& rec) {
    { fn(flat, rec) } -> std::same_as<void>;
};

template <PureMutationFn Fn> class MutationFnWrap {
public:
    explicit MutationFnWrap(Fn& fn)
        : fn_(&fn) {}

    void visit_mutation(FlatAST& flat, const MutationRecord& rec) { (*fn_)(flat, rec); }
    [[nodiscard]] bool has_error() const noexcept { return false; }

private:
    Fn* fn_;
};

// ── run_mutation_pipeline — fold over mutation log ───────────
template <MutationVisitor V>
bool run_mutation_visitor_one(FlatAST& flat, const MutationRecord& rec, V& visitor) {
    visitor.visit_mutation(flat, rec);
    return !visitor.has_error();
}

template <MutationVisitor... Visitors>
bool run_mutation_one(FlatAST& flat, const MutationRecord& rec, Visitors&... visitors) {
    return (run_mutation_visitor_one(flat, rec, visitors) && ...);
}

template <MutationVisitor... Visitors>
bool run_mutation_pipeline(FlatAST& flat, Visitors&... visitors) {
    for (const auto& rec : flat.all_mutations()) {
        if (!run_mutation_one(flat, rec, visitors...))
            return false;
    }
    return true;
}

template <MutationVisitor... Visitors>
bool run_mutation_pipeline(FlatAST& flat, std::span<const MutationRecord> records,
                           Visitors&... visitors) {
    for (const auto& rec : records) {
        if (!run_mutation_one(flat, rec, visitors...))
            return false;
    }
    return true;
}

} // namespace aura::ast
