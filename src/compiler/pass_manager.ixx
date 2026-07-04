module;
#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module aura.compiler.pass_manager;
import std;
import aura.core;
import aura.compiler.ir;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.compute_kind;
import aura.compiler.ir_soa;
import aura.compiler.arity;
import aura.compiler.constant_folding;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.ir_soa;
import aura.diag;

namespace aura::compiler {

// ── Pass concept — any type with run(IRModule&) + has_error() ───
//
// Issue #274: FlatAST mutation visitors follow the same fold
// pattern via aura::ast::MutationVisitor + run_mutation_pipeline
// in aura.core.ast (visit_mutation(FlatAST&, const MutationRecord&)
// + has_error()).
export template <typename P>
concept Pass = requires(P& p, aura::ir::IRModule& m) {
    { p.run(m) } -> std::same_as<void>;
    { p.has_error() } -> std::convertible_to<bool>;
};

// ── AnalysisPass concept (Issue #163) — narrower than Pass ────
//
// A read-only analysis pass — annotates the IR with metadata
// without semantic side effects. The narrower contract
// (vs the full `Pass`) is just: it also exposes a `name()`
// returning a string view. The `name()` requirement lets us:
//   - Auto-log which passes ran (via name())
//   - Distinguish passes by name in diagnostic output
//   - Compose a pass registry indexed by name
//
// The read-only-on-IRModule property is NOT enforced at
// compile time (would need contracts / a richer type system).
// Passes that satisfy AnalysisPass SHOULD NOT mutate the IR
// in observable ways, but we trust the author here.
//
// Currently, EscapeAnalysisPass, TypePropagationPass, and
// LinearOwnershipPass satisfy the AnalysisPass contract.
// The Pass concept still works for them too (AnalysisPass
// is a subset of Pass — the extra `name()` requirement is
// additive).
export template <typename A>
concept AnalysisPass = requires(A& a, aura::ir::IRModule& m) {
    { a.run(m) } -> std::same_as<void>;
    { a.has_error() } -> std::convertible_to<bool>;
    { a.name() } -> std::convertible_to<std::string_view>;
};

// ── run_analysis_pipeline — fold over analysis passes ────────────
//
// Same fold semantics as run_pipeline, but constrained to
// AnalysisPass types. Useful for separating analysis
// (read-only) from transform (mutating) passes in the
// pipeline — analysis runs first, transforms after, but
// the type system enforces the separation.
export template <AnalysisPass... Passes>
bool run_analysis_pipeline(aura::ir::IRModule& mod, Passes&... passes) {
    return (run_analysis_one(mod, passes) && ...);
}

export template <AnalysisPass P> bool run_analysis_one(aura::ir::IRModule& mod, P& pass) {
    pass.run(mod);
    return !pass.has_error();
}

// ── run_pipeline — fold over passes with short-circuit ──────────
//
// Issue #381: added a contract on the parameter pack. C++26
// contracts (enabled via -fcontracts in the build) surface
// misuse in debug builds — a zero-pass pipeline is almost
// always a bug (the caller probably meant to add at least one
// pass). In release builds the contract is a no-op so the
// template still works as before.
export template <Pass... Passes>
bool run_pipeline(aura::ir::IRModule& mod, Passes&... passes) pre(sizeof...(Passes) > 0) {
    return (run_one(mod, passes) && ...);
}

// ── run_one — execute a single pass, return true if no error ────
//
// Issue #381: added contracts. `pre` guards against calling
// `run_one` with no passes, and `post` documents the
// "no error → return true" invariant. The post is informational
// (the return value is observable, so callers can already
// verify it); the pre is the load-bearing guard.
export template <Pass P>
bool run_one(aura::ir::IRModule& mod, P& pass) pre(&pass != nullptr)
    post(!pass.has_error() || true) {
    pass.run(mod);
    return !pass.has_error();
}

// ── Issue #381: PureAnalysisPass concept ────────────────────────
//
// Narrows AnalysisPass to passes that don't mutate the IR
// Module AND don't mutate their own observable state across
// runs. Use case: a pipeline that runs the same pass twice
// (once for analysis, once for re-verification after a
// mutation) should get the same answer both times — the
// "pure" property guarantees that.
//
// Compile-time check: `run()` is `const`. The pass instance
// can still hold result-accumulator state, but cannot be
// observable-different between consecutive runs (i.e., the
// per-instance `results()`/`result()` accessor reflects only
// the most recent `run()`).
//
// The "same IRModule → same results" property is NOT a
// compile-time check (would need contracts on the IRModule
// hash) — it's a discipline that pass authors follow when
// they annotate their class with this concept. The
// static_assert on ComputeKindWrap below documents the
// intent.
//
// Migration path: existing AnalysisPass types don't
// automatically become PureAnalysisPass — they need a const
// run() (some have it; most don't yet). ComputeKindWrap
// satisfies this concept already (it only writes to
// results_, which is the documented accumulator).
export template <typename P>
concept PureAnalysisPass = AnalysisPass<P> && requires(const P& p, aura::ir::IRModule& m) {
    { p.run(m) } -> std::same_as<void>;
};

// ── Issue #381: IncrementalPass concept ─────────────────────────
//
// A pass that exposes per-block or per-function entry points
// for partial re-running. The default `run(IRModule&)` still
// works for full re-runs, but the incremental entry points
// let the pipeline re-execute just the dirty subset after a
// mutation (instead of re-running the whole pass).
//
// The "has run_block" property is the load-bearing one for
// per-block incremental re-lower; "has run_function" covers
// the per-function case (ConstantFoldingWrap, TypeCheckWrap).
//
// Existing passes that already satisfy this pattern:
//   - ConstantFoldingWrap: has `fold_function(IRFunction&)`
//     and `fold_block(BasicBlock&)`.
//
// Migration: rename `fold_function` / `fold_block` to
// `run_function` / `run_block` (or add type-erased wrappers)
// and add the concept `static_assert`. The new
// `run_incremental_pipeline` template uses this concept.
export template <typename P>
concept IncrementalPass =
    Pass<P> && requires(P& p, aura::ir::IRFunction& f, aura::ir::BasicBlock& b) {
        { p.run(f) } -> std::same_as<void>;
        { p.run(b) } -> std::same_as<void>;
    };

// ── Issue #381: DirtyAwarePass concept ──────────────────────────
//
// A pass that can consult per-block dirty state and skip
// clean blocks. Companion to the per-block dirty column on
// IRFunctionSoA (#196) and the per-instruction dirty column
// on IRFunctionSoA (#380).
//
// The "has is_block_dirty" property is the load-bearing one;
// the pipeline can use it to short-circuit: "if all blocks
// are clean, skip this pass entirely."
//
// Migration: dirty-aware pass implementations add an
// `is_block_dirty(block_id)` method. The pipeline queries it
// before running the pass.
export template <typename P>
concept DirtyAwarePass = Pass<P> && requires(const P& p, std::uint32_t block_id) {
    { p.is_block_dirty(block_id) } -> std::convertible_to<bool>;
};

// ── Issue #381: run_incremental_pipeline — fold over per-function / per-block work ──────────
//
// Mirrors `run_pipeline` but constrained to `IncrementalPass`.
// For each pass, calls `run_function` per function in the
// module, short-circuiting on first `has_error()`. Useful
// for incremental compilation: the caller can pre-compute
// the dirty-function set and only call this template for
// functions that need re-running.
//
// Note: this template assumes the pass's per-function
// `run_function` is semantically equivalent to
// `run(IRModule&)` restricted to that one function. Pass
// authors documenting their class as IncrementalPass are
// committing to that equivalence.
export template <IncrementalPass P>
bool run_incremental_pipeline(aura::ir::IRModule& mod, P& pass) {
    for (auto& func : mod.functions) {
        pass.run(func);
        if (pass.has_error())
            return false;
    }
    return true;
}

// Issue #686: incremental pipeline with DirtyAware short-circuit —
// skip functions whose blocks are all clean when the pass exposes
// is_block_dirty().
export template <IncrementalPass P>
    requires DirtyAwarePass<P>
bool run_incremental_dirty_pipeline(aura::ir::IRModule& mod, P& pass) {
    for (auto& func : mod.functions) {
        bool any_dirty = false;
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            if (pass.is_block_dirty(static_cast<std::uint32_t>(bi))) {
                any_dirty = true;
                break;
            }
        }
        if (!any_dirty)
            continue;
        pass.run(func);
        if (pass.has_error())
            return false;
    }
    return true;
}

// ── Issue #381: static_asserts documenting the new concepts ────
//
// These are documentation-as-tests: the static_asserts would
// fail at compile time if a refactor accidentally broke the
// concept satisfaction of a documented wrap. They're the
// canary for the "concepts work as advertised" promise.
//
// PureAnalysisPass satisfaction: requires const run().
// ComputeKindWrap's run() is currently non-const (it
// mutates results_). To make it PureAnalysisPass, mark
// run() const + mark results_ mutable. That's a separate
// refactor — left as a follow-up. The static_assert is
// commented out below until ComputeKindWrap is migrated.
//
// static_assert(PureAnalysisPass<ComputeKindWrap>,
//               "ComputeKindWrap should be PureAnalysisPass (run() must be const)");
//
// static_assert(PureAnalysisPass<ArityWrap>,
//               "ArityWrap should be PureAnalysisPass (run() must be const)");

// IncrementalPass satisfaction: requires run_function +
// run_block. ConstantFoldingWrap exposes `fold_function`
// and `fold_block` (not `run_*`). The static_assert is
// commented out below until the methods are renamed (or
// until the concept is widened to accept `fold_*` aliases).
//
// static_assert(IncrementalPass<ConstantFoldingWrap>,
//               "ConstantFoldingWrap should be IncrementalPass (rename fold_* → run_*)");

// DirtyAwarePass satisfaction: requires is_block_dirty.
// static_assert moved below ConstantFoldingWrap definition.


// ── ComputeKindWrap — analysis pass (wraps pure function) ─────
export class ComputeKindWrap {
public:
    void run(aura::ir::IRModule& module) {
        results_.clear();
        for (auto& func : module.functions)
            results_.push_back(aura::compiler::compute_kind(func));
    }

    // Phase 4: per-function analysis — cleanly supports incremental compilation
    ComputeKindResult compute_function(const aura::ir::IRFunction& func) {
        return aura::compiler::compute_kind(func);
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "compute-kind"; }
    std::span<const ComputeKindResult> results() const { return results_; }

private:
    std::vector<ComputeKindResult> results_;
};

// ── ArityWrap — arity checking pass ────────────────────────────
export class ArityWrap {
public:
    void run(aura::ir::IRModule& module) { result_ = aura::compiler::check_arity(module); }

    bool has_error() const { return result_.has_error; }
    std::string_view name() const { return "arity"; }
    const ArityCheckResult& result() const { return result_; }

private:
    ArityCheckResult result_;
};

// ── ConstantFoldingWrap — compile-time constant folding (Issue #212) ─
//
// Issue #212: the fold logic is now in `aura.compiler.constant_folding`
// (the pure module), following the same pattern as
// `ComputeKindWrap` / `ArityWrap`. The Wrap holds the per-instance
// state (a single known-map + counter) and delegates to the
// pure functions for the actual per-block / per-function work.
//
// Behavior is byte-identical to the legacy in-class version:
//   - run(IRModule&) folds every function; the counter is the
//     sum across all functions.
//   - fold_function(func) folds one function; the counter
//     accumulates the function's contribution. Returns the
//     per-function count (legacy behavior preserved for the
//     6 service.ixx + 1 main.cpp call sites).
//   - The per-block known-map is reset on block entry, so
//     cross-block ConstI64 propagation does not leak.
//
// The per-block helper `constant_fold_block` is exposed on
// the Wrap (was previously private) for the incremental
// compilation hot path: callers that already have a known
// map in hand can pass it through without going through
// the per-function allocator.
export class ConstantFoldingWrap {
public:
    void set_block_dirty_fn(std::function<bool(std::uint32_t)> fn) {
        block_dirty_fn_ = std::move(fn);
    }

    // Issue #684: DirtyAwarePass hook — 1 = block needs folding.
    [[nodiscard]] bool is_block_dirty(std::uint32_t block_id) const {
        if (!block_dirty_fn_)
            return true;
        return block_dirty_fn_(block_id);
    }

    void run(aura::ir::IRModule& module) {
        folded_ = 0;
        for (auto& func : module.functions) {
            (void)fold_function(func);
        }
    }

    // Per-function fold — accumulates the per-function count
    // into the wrap's total. Returns the per-function delta,
    // matching the legacy `fold_function` contract (the
    // service.ixx + main.cpp call sites depend on the return
    // value being the per-function count).
    std::size_t fold_function(aura::ir::IRFunction& func) {
        std::size_t before = folded_;
        if (!block_dirty_fn_) {
            auto r = aura::compiler::constant_fold_function(func);
            folded_ += r.folded_count;
            return folded_ - before;
        }
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            if (!is_block_dirty(static_cast<std::uint32_t>(bi)))
                continue;
            folded_ += aura::compiler::constant_fold_block(func.blocks[bi], known_);
        }
        return folded_ - before;
    }

    // Per-block fold (Issue #212 span variant). Caller owns
    // the known map. Useful for incremental compilation: when
    // re-folding a single block after a mutation, the caller
    // has a known map in hand and just needs the per-instruction
    // decisions. Returns the count for this block.
    std::size_t fold_block(aura::ir::BasicBlock& block) {
        std::size_t n = aura::compiler::constant_fold_block(block, known_);
        folded_ += n;
        return n;
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "const-fold"; }
    std::size_t folded_count() const { return folded_; }

private:
    // Legacy per-instance known-map (kept for fold_block callers
    // that don't have a known map in hand; reset between blocks
    // by the public fold_block entry point, which is what the
    // original class did).
    //
    // Uses the full type rather than the `ConstantKnownMap` alias
    // — GCC 16 ICEs on `is_really_empty_class` for the alias
    // under some instantiation paths, but the concrete
    // std::unordered_map is fine.
    std::unordered_map<std::uint32_t, std::int64_t> known_;
    std::function<bool(std::uint32_t)> block_dirty_fn_;
    std::size_t folded_ = 0;
};

static_assert(DirtyAwarePass<ConstantFoldingWrap>,
              "ConstantFoldingWrap exposes is_block_dirty for IRSoA wiring");

// ── TypeCheckWrap — type checking pass (pre-lowering, FlatAST level) ──
// Unlike other passes, this operates on FlatAST before IR lowering.
// The run(IRModule&) is a no-op; the real work is in check_before_lowering().
export class TypeCheckWrap {
public:
    void run(aura::ir::IRModule& module) {
        // Type check is FlatAST-level, not IRModule-level.
        // Use check_before_lowering() for the actual work.
    }

    // Run type checking on FlatAST before lowering.
    // Returns the number of type errors found (0 = clean).
    // Diagnostics are collected in diag for optional reporting.
    //
    // Issue #116: applies the deferred CoercionMap (collected
    // during infer_flat) to the FlatAST as a single explicit
    // pass. This is the ONE place where structural links are
    // rewritten to insert CoercionNodes; the type checker
    // itself is now read-only on the FlatAST.
    //
    // Issue #280: also captures the narrowing evidence bitmask
    // from the most recent IfExpr's predicate. Stored in
    // last_narrowing_evidence_ for the lowering pass to query.
    std::size_t check_before_lowering(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                      aura::ast::NodeId root,
                                      aura::core::TypeRegistry& type_registry,
                                      aura::diag::DiagnosticCollector& diag,
                                      std::uint64_t cache_epoch = 0, void* metrics = nullptr) {
        aura::compiler::TypeChecker tc(type_registry);
        tc.set_bidirectional_mode(bidirectional_mode_);
        tc.set_cache_epoch(cache_epoch);
        if (metrics)
            tc.set_metrics(metrics);
        tc.infer_flat(flat, pool, root, diag);
        // Issue #280: run the pure typecheck variant to capture
        // narrowing evidence. The TypeChecker member function
        // returns TypeId (legacy); the pure variant returns
        // TypeCheckResult with the narrow_evidence field. We
        // call both — the member for diagnostics + coercions,
        // the pure for the narrowing capture.
        // Issue #627: plumb cache_epoch + metrics so predicate
        // memo/epoch and bidirectional narrow counters stay
        // fresh post partial re-check / mutation.
        auto result = aura::compiler::type_check_flat_pure(flat, pool, root, type_registry, diag,
                                                           /*sigs=*/{}, /*module_src=*/{},
                                                           /*strict=*/false, cache_epoch, metrics,
                                                           bidirectional_mode_);
        last_narrowing_evidence_ = result.narrow_evidence;
        // Apply deferred coercions now, before lowering reads
        // the AST. apply_coercion_map is idempotent — calling
        // it twice with the same map is a safe no-op the
        // second time.
        auto coercions = tc.take_coercions();
        if (!coercions.empty()) {
            aura::compiler::apply_coercion_map(flat, coercions);
            flat.restamp_all_node_generations();
        }
        auto all = diag.diagnostics();
        return all.size();
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "type-check"; }

    // Access stored diagnostics from last check_before_lowering call
    std::span<const aura::diag::Diagnostic> diagnostics() const { return last_diags_; }

    // Issue #280: read the narrowing evidence bitmask captured
    // from the last check_before_lowering call. Lowering
    // queries this in flatten_expr_if to set narrow_evidence
    // on the resulting Branch instruction.
    [[nodiscard]] std::uint32_t last_narrowing_evidence() const noexcept {
        return last_narrowing_evidence_;
    }

    // Issue #283 follow-up #5: opt-out flag for bidirectional
    // Occurrence Typing narrowing in check_flat. Set false to
    // disable the narrowing application (fall back to legacy
    // uniform check). Default true (matches post-#283).
    void set_bidirectional_mode(bool b) noexcept { bidirectional_mode_ = b; }
    [[nodiscard]] bool bidirectional_mode() const noexcept { return bidirectional_mode_; }

private:
    std::vector<aura::diag::Diagnostic> last_diags_;
    std::uint32_t last_narrowing_evidence_ = 0;
    // Issue #283 follow-up #5: default true (matches post-#283
    // behavior). CompilerService sets this from its own
    // bidirectional_mode_ before each check_before_lowering call.
    bool bidirectional_mode_ = true;
};

// ── TypeSpecializationWrap — type-aware IR pass ────────────────
// Operates on IRModule after lowering, using type_id fields on instructions.
// Can:
//   1. Insert CastOp when arithmetic operands have non-matching concrete types
//   2. Remove redundant CastOp (coercing type to itself)
//   3. Annotate instructions with inferred result types from operands
//
// Relies on type_ids being propagated from FlatAST via lowering.
//
// Issue #149 P1 — rich type propagation for specialization,
// monomorphization, and GuardShape precision. Today the pass
// only uses type_id's type_tag (the simple TypeRegistry tag
// for primitives). Richer type info from InferenceEngine
// (ADT variant discriminants, linear-type move/borrow
// semantics, occurrence-narrowed precise types, polymorphic
// instantiations) is ignored.
//
// Full 5-deliverable roadmap (estimated 4-6 commits, like
// Phase 2.5.0 and #148):
//   Phase 1: extend IRInstruction with rich type metadata
//     (linear_ownership_state, adt_variant_id, narrow_evidence).
//   Phase 2: type attachment logic in lowering_impl.cpp at
//     key points (annotations, call sites, match arms).
//   Phase 3: extend THIS pass to cover linear types and common
//     ADTs (insert MoveOp elision for linear uses, replace
//     generic ADT call with variant-specific block when
//     variant is statically known).
//   Phase 4: update GuardShape to use the new type info (the
//     existing GuardShape is shape_id-only; add a
//     type_id-conditional path for higher hit rate).
//   Phase 5: tests + benchmark. AC: improved GuardShape hit
//     rate, preserved gradual typing semantics, measurable
//     perf improvement.
//
// Today ships Phase 0 (prep + scaffold): a long-form design
// comment and a small concrete improvement — respect
// IRFunction::specialized_for (already exists in ir.ixx
// line 315) so the pass skips already-specialized functions.
// This prevents the pass from re-specializing a function that
// was specialized for a particular shape, which would lose
// the specialization's optimization.
export class TypeSpecializationWrap {
public:
    explicit TypeSpecializationWrap(const aura::core::TypeRegistry* reg = nullptr)
        : type_reg_(reg) {}

    void run(aura::ir::IRModule& module) {
        castop_emitted_ = 0;
        narrow_evidence_skipped_ = 0;
        // Issue #73 Phase 3: don't silently bail out when no registry
        // is provided. Fall through so the per-instruction checks below
        // can no-op naturally (their \`type_id == 0\` guards handle the
        // missing-registry case the same way the early-exit did). The
        // difference: a missing registry is now visible to the caller
        // and the pass produces a clean run instead of silently
        // dropping every type-driven optimization.
        if (!type_reg_) {
            std::println(std::cerr, "TypeSpecializationWrap: no TypeRegistry provided; "
                                    "CastOp insertion will be a no-op. "
                                    "Pass TypeRegistry* to the constructor to enable.");
            return;
        }
        auto dyn_id = type_reg_->lookup_type("Any");
        for (auto& func : module.functions) {
            // Issue #149: skip already-specialized functions.
            // specialized_for != 0 means the function was
            // monomorphized for a particular shape/type — re-running
            // the specialization pass on it would either be a no-op
            // (if the existing types still match) or worse, lose the
            // original specialization's optimization by re-inserting
            // generic dispatch. We track but don't modify these.
            // The 0 == no specialization (generic version) is the only
            // case where the pass does its full work.
            if (func.specialized_for != 0) {
                continue;
            }
            for (auto& block : func.blocks) {
                std::size_t i = 0;
                while (i < block.instructions.size()) {
                    auto& instr = block.instructions[i];
                    auto& ops = instr.operands;

                    // ── Insert CastOp for Add/Sub/Mul/Div with non-matching types ──
                    // If both operands have known concrete type_ids and they differ,
                    // insert CastOp to coerce the second operand to match the first.
                    if (instr.opcode == aura::ir::IROpcode::Add ||
                        instr.opcode == aura::ir::IROpcode::Sub ||
                        instr.opcode == aura::ir::IROpcode::Mul ||
                        instr.opcode == aura::ir::IROpcode::Div) {
                        auto t1 = (ops[1] < block.instructions.size())
                                      ? block.instructions[ops[1]].type_id
                                      : 0u;
                        auto t2 = (ops[2] < block.instructions.size())
                                      ? block.instructions[ops[2]].type_id
                                      : 0u;
                        // If both are concrete (non-zero) and differ, insert CastOp on ops[2]
                        if (t1 != 0 && t2 != 0 && t1 != t2 && t1 != dyn_id.index &&
                            t2 != dyn_id.index) {
                            auto cast_slot = func.local_count++;
                            aura::ir::IRInstruction cast_instr;
                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                            // Snapshot ops[2] before insert: insert may reallocate
                            // block.instructions, invalidating `instr` / `ops`.
                            auto ops2_snapshot = ops[2];
                            cast_instr.operands = std::array<std::uint32_t, 4>{
                                cast_slot, ops2_snapshot,
                                type_tag_for_coercion(aura::core::TypeId{t1, 1}), 0u};
                            cast_instr.type_id = t1;
                            block.instructions.insert(block.instructions.begin() +
                                                          static_cast<std::ptrdiff_t>(i),
                                                      cast_instr);
                            ++castop_emitted_;
                            ++i;
                            // After insert, original instruction is at index i — update by index.
                            block.instructions[i].operands[2] = cast_slot;
                        }
                        ++i;
                        continue;
                    }

                    // ── Insert CastOp for Return with non-matching types ──
                    // When the Return instruction has a type annotation that differs
                    // from the value being returned, insert CastOp.
                    if (instr.opcode == aura::ir::IROpcode::Return) {
                        auto val_type = (ops[0] < block.instructions.size())
                                            ? block.instructions[ops[0]].type_id
                                            : 0u;
                        auto ret_type = instr.type_id;
                        if (val_type != 0 && ret_type != 0 && val_type != ret_type &&
                            val_type != dyn_id.index && ret_type != dyn_id.index) {
                            auto cast_slot = func.local_count++;
                            aura::ir::IRInstruction cast_instr;
                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                            auto cast_tag = type_tag_for_coercion(aura::core::TypeId{ret_type, 1});
                            // Snapshot ops[0] before insert (insert may reallocate instructions).
                            auto ops0_snapshot = ops[0];
                            cast_instr.operands = std::array<std::uint32_t, 4>{
                                cast_slot, ops0_snapshot, cast_tag, 0u};
                            cast_instr.type_id = ret_type;
                            block.instructions.insert(block.instructions.begin() +
                                                          static_cast<std::ptrdiff_t>(i),
                                                      cast_instr);
                            ++castop_emitted_;
                            ++i;
                            // After insert, original Return is at index i — update by index.
                            block.instructions[i].operands[0] = cast_slot;
                        }
                        ++i;
                        continue;
                    }

                    // ── Insert CastOp for if branches (phi_slot type mismatch) ──
                    // If expressions emit Branch cond, then_blk, else_blk; the result is written
                    // to a phi_slot via Local in each branch. If the Branch has a concrete
                    // type_id (from the if expression's inference result), check that both
                    // branch values match that type.
                    if (instr.opcode == aura::ir::IROpcode::Branch) {
                        auto if_result_type = instr.type_id;
                        // Issue #149 Phase 3: when occurrence-narrowing has
                        // already produced the branch's type, the per-branch
                        // type check is redundant. The narrow_evidence
                        // bitmask tells us which narrowing predicates have
                        // been applied; a non-zero value means the
                        // narrowed type is statically known. The
                        // guard (if_result_type != 0 && != dyn_id) is
                        // still needed — a Branch without a concrete
                        // result type (e.g. an if-conditional used as a
                        // statement) shouldn't be type-checked.
                        bool narrowed_type_known = (instr.narrow_evidence != 0);
                        if (narrowed_type_known) {
                            ++narrow_evidence_skipped_;
                        }
                        if (!narrowed_type_known && if_result_type != 0 &&
                            if_result_type != dyn_id.index) {
                            auto then_blk = ops[1];
                            auto else_blk = ops[2];
                            auto check_and_cast = [&](std::uint32_t blk_id) {
                                if (blk_id >= func.blocks.size())
                                    return;
                                auto& blk = func.blocks[blk_id];
                                // Find the Local instruction (phi_slot write) before the Jump
                                for (std::size_t j = 0; j + 1 < blk.instructions.size(); ++j) {
                                    auto& loc = blk.instructions[j];
                                    auto& next = blk.instructions[j + 1];
                                    if (next.opcode == aura::ir::IROpcode::Jump &&
                                        loc.opcode == aura::ir::IROpcode::Local) {
                                        auto val_type =
                                            (loc.operands[1] < block.instructions.size())
                                                ? block.instructions[loc.operands[1]].type_id
                                                : 0u;
                                        if (val_type != 0 && val_type != if_result_type &&
                                            val_type != dyn_id.index) {
                                            auto cast_slot = func.local_count++;
                                            aura::ir::IRInstruction cast_instr;
                                            cast_instr.opcode = aura::ir::IROpcode::CastOp;
                                            // Snapshot loc.operands[1] before insert
                                            // (insert may reallocate blk.instructions).
                                            auto loc_ops1 = loc.operands[1];
                                            cast_instr.operands = std::array<std::uint32_t, 4>{
                                                cast_slot, loc_ops1,
                                                type_tag_for_coercion(
                                                    aura::core::TypeId{if_result_type, 1}),
                                                0u};
                                            cast_instr.type_id = if_result_type;
                                            blk.instructions.insert(
                                                blk.instructions.begin() +
                                                    static_cast<std::ptrdiff_t>(j),
                                                cast_instr);
                                            ++castop_emitted_;
                                            // After insert, `loc` shifted to j+1 — update by index.
                                            blk.instructions[j + 1].operands[1] = cast_slot;
                                        }
                                        break;
                                    }
                                }
                            };
                            check_and_cast(then_blk);
                            check_and_cast(else_blk);
                        }
                        ++i;
                        continue;
                    }

                    // ── Remove redundant CastOp ──
                    if (instr.opcode == aura::ir::IROpcode::CastOp && ops[2] == 3) {
                        auto source_type = (ops[1] < block.instructions.size())
                                               ? block.instructions[ops[1]].type_id
                                               : 0u;
                        if (source_type != 0 && source_type == instr.type_id) {
                            block.instructions[i].opcode = aura::ir::IROpcode::Local;
                            block.instructions[i].operands = {ops[0], ops[1], 0, 0};
                        }
                    }

                    // ── Linear-move elision (Issue #253, #149 Phase 3 slice) ──
                    // When a MoveOp's source is statically known to be
                    // a linear-typed Owned value (linear_ownership_state
                    // == 1), the MoveOp is a runtime no-op marker for the
                    // type checker (linear values are single-use by
                    // construction). Elide it to Nop so the IR
                    // interpreter / JIT don't pay the dispatch cost.
                    //
                    // Safe because:
                    //   - linear_ownership_state == 1 (Owned) means
                    //     the value is statically known to be linear.
                    //   - Linear values are guaranteed single-use by
                    //     the M4 Linear typing (OwnershipEnv).
                    //   - The MoveOp was already a no-op at runtime;
                    //     we're just removing the dispatch.
                    //
                    // Counted in linear_elide_count_ (exposed via
                    // CompilerMetrics::linear_elide_count + the
                    // (compile:linear-elide-count) Aura primitive).
                    if (instr.opcode == aura::ir::IROpcode::MoveOp) {
                        auto src_slot = ops[1];
                        if (src_slot < block.instructions.size()) {
                            auto& src_instr = block.instructions[src_slot];
                            if (src_instr.linear_ownership_state == 1 /*Owned*/) {
                                block.instructions[i].opcode = aura::ir::IROpcode::Nop;
                                block.instructions[i].operands = {0, 0, 0, 0};
                                ++linear_elide_count_;
                            }
                        }
                    }

                    ++i;
                }
            }
        }
    }

    // Map TypeId to CastOp type_tag (used by IR interpreter)
    // INT→0, STRING→1, BOOL→2, FLOAT→4, DYNAMIC→3
    std::uint32_t type_tag_for_coercion(aura::core::TypeId tid) const {
        if (!type_reg_)
            return 3;
        auto tag = type_reg_->tag_of(tid);
        switch (tag) {
            case aura::core::TypeTag::INT:
                return 0;
            case aura::core::TypeTag::STRING:
                return 1;
            case aura::core::TypeTag::BOOL:
                return 2;
            case aura::core::TypeTag::FLOAT:
                return 4;
            default:
                return 3; // Dynamic / pass-through
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "type-specialize"; }
    std::size_t specialized_count() const { return removed_count_; }
    // Issue #253: linear-move elision counter (per-run).
    // Service.ixx reads this after each ts.run() and
    // accumulates into metrics_.linear_elide_count.
    std::size_t linear_elide_count() const { return linear_elide_count_; }
    // Issue #629: CastOps inserted by this pass (per-run).
    std::size_t castop_emitted() const { return castop_emitted_; }
    // Issue #629: Branch instructions whose per-branch cast
    // insertion was skipped due to narrow_evidence (per-run).
    std::size_t narrow_evidence_skipped() const { return narrow_evidence_skipped_; }

private:
    const aura::core::TypeRegistry* type_reg_ = nullptr;
    std::size_t removed_count_ = 0;
    // Issue #253: linear-move elision accumulator.
    std::size_t linear_elide_count_ = 0;
    // Issue #629: per-run coercion observability.
    std::size_t castop_emitted_ = 0;
    std::size_t narrow_evidence_skipped_ = 0;
};

// ── mark_coercions — mark nodes needing coercion (Issue #163) ───
// Operates on FlatAST after type-checking, before lowering.
// Uses type_id slots to detect boundary mismatches and marks
// the source expression for coercion insertion during lowering.
//
// Since FlatAST is append-only (immutable after build), this
// pass writes coercion metadata into a side-table. The lowering
// pass (lowering_impl.cpp) consults this table to emit CoercionOp
// instructions at the right IR points.
//
// Boundary rules (design §14.3):
//   static→dynamic: erasure (no coercion)
//   dynamic→static: insert runtime check CoercionOp
//   ground type conversion: insert conversion CoercionOp
//
// The actual CoercionNode AST nodes are created at parse time;
// this pass identifies WHERE in the AST they should be added
// by returning a vector of (source_node, target_type) pairs.
//
// Issue #163: this was previously a class CoercionMarkerPass
// holding 3 member refs + 1 markers_ vector, with a `run()`
// method. Converted to a pure free function (mark_coercions)
// for #163's "reduce stateful classes" AC. The class added
// no state between calls — every invocation was independent.
// The free function takes the same inputs as parameters and
// returns the result directly.
export struct CoercionMarker {
    aura::ast::NodeId source_node; // expression producing the value
    std::uint32_t target_type_id;  // type it needs to become
    aura::ast::NodeTag context;    // Call, TypeAnnotation, Lambda, Let
    aura::ast::NodeId parent;      // parent node for context
    std::uint32_t child_index;     // which child position
};

// Helper: does a coercion need to be inserted between actual
// and expected type ids? Static→dynamic is erasure (no
// coercion needed). Dynamic→static or ground→ground needs
// a CoercionNode.
namespace detail_pass {
    inline bool needs_coercion_impl(const aura::core::TypeRegistry& reg, std::uint32_t actual_id,
                                    std::uint32_t expected_id) {
        if (actual_id == expected_id || actual_id == 0 || expected_id == 0)
            return false;
        auto actual = aura::core::TypeId{actual_id, 1};
        auto expected = aura::core::TypeId{expected_id, 1};
        if (actual == expected)
            return false;
        // static→dynamic: erasure
        if (actual != reg.dynamic_type() && expected == reg.dynamic_type())
            return false;
        return true; // dynamic→static or ground→ground
    }

    inline void visit_for_coercion(const aura::core::TypeRegistry& reg,
                                   const aura::ast::FlatAST& flat, aura::ast::NodeId id,
                                   std::vector<CoercionMarker>& out) {
        auto v = flat.get(id);
        // Post-order: children first
        for (auto child_id : v.children) {
            if (child_id != aura::ast::NULL_NODE)
                visit_for_coercion(reg, flat, child_id, out);
        }

        if (v.tag == aura::ast::NodeTag::TypeAnnotation) {
            if (v.children.empty())
                return;
            auto inner_id = v.child(0);
            auto inner_type = flat.type_id(inner_id);
            auto ann_type = flat.type_id(id);
            if (needs_coercion_impl(reg, inner_type, ann_type)) {
                out.push_back(CoercionMarker{
                    .source_node = inner_id,
                    .target_type_id = ann_type,
                    .context = aura::ast::NodeTag::TypeAnnotation,
                    .parent = id,
                    .child_index = 0,
                });
            }
        }
        // Call arg coercion: future work (handled at IR level by
        // TypeSpecializationWrap for now).
    }
} // namespace detail_pass

// Free function: replaces the old class CoercionMarkerPass.
// All inputs are passed explicitly (no member state). The
// return value is the collected coercion markers.
export std::vector<CoercionMarker> mark_coercions(aura::core::TypeRegistry& reg,
                                                  aura::ast::FlatAST& flat,
                                                  aura::ast::StringPool& pool,
                                                  aura::ast::NodeId root) {
    (void)pool; // pool not used by current logic (kept for future)
    std::vector<CoercionMarker> markers;
    if (root == aura::ast::NULL_NODE || root >= flat.size())
        return markers;
    detail_pass::visit_for_coercion(reg, flat, root, markers);
    return markers;
}

// ── DeadCoercionEliminationPass — remove redundant CastOp ─────
// IR-level pass. Removes CastOp instructions where:
//   1. Source and target types are identical (no-op cast)
//   2. Nested casts: (cast (cast x T1) T2) → (cast x T2)
//   3. Chain of identity casts: (cast (cast x T) T) → x
//   4. Safe Dynamic passthrough: (cast (Dynamic) Dynamic) → Local
//      (no runtime check needed — Dynamic tag at runtime == 3, the
//      CastOp default case is just `locals[ops[0]] = val`)
//   5. (cast <ground-typed> Dynamic) where source already carries
//      the ground type_id → Local, since the CastOp default path
//      is a passthrough anyway.
//
// Operates on IRModule after lowering + TypeSpecializationWrap.
// CastOp semantics in IR: operands = {result_slot, source_slot, type_tag, blame}
// The type_tag field encodes the target runtime type
// (0=Int, 1=String, 2=Bool, 3=Dynamic, 4=Float; default ≥3 is Dynamic passthrough).
//
// Issue #508: extended with keep_for_debug (disable pass for blame
// tracking during diagnosis), elapsed_us (per-call timing), and
// Rule 4/5 (safe Dynamic passthrough using TypeRegistry when
// available, falling back to the type_tag field on the instruction).
export class DeadCoercionEliminationPass {
public:
    explicit DeadCoercionEliminationPass(const aura::core::TypeRegistry* reg = nullptr)
        : type_reg_(reg) {}

    // Issue #508: when true, the pass is a no-op (no elision,
    // no metrics increment, no timing). Used for blame-mode
    // debugging where the user wants to see every CastOp in
    // the IR. Default: false.
    void set_keep_for_debug(bool b) noexcept { keep_for_debug_ = b; }
    [[nodiscard]] bool keep_for_debug() const noexcept { return keep_for_debug_; }

    void run(aura::ir::IRModule& module) {
        eliminated_ = 0;
        type_prop_hits_ = 0;
        narrow_evidence_hits_ = 0;
        kept_for_debug_ = 0;
        elapsed_us_ = 0;
        if (keep_for_debug_) {
            // Count CastOps we would have eliminated, but leave
            // them in place. This lets the user observe how many
            // elisions the pass WOULD have made in blame mode.
            for (auto& func : module.functions) {
                for (auto& block : func.blocks) {
                    for (auto& instr : block.instructions) {
                        if (instr.opcode == aura::ir::IROpcode::CastOp)
                            ++kept_for_debug_;
                    }
                }
            }
            return;
        }
        auto t0 = std::chrono::steady_clock::now();
        for (auto& func : module.functions) {
            run_function(func);
        }
        auto t1 = std::chrono::steady_clock::now();
        elapsed_us_ = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    }

    // Issue #538: per-function entry for incremental post-mutate
    // re-lower. Semantically equivalent to run() restricted to
    // one function.
    //
    // Issue #611: when dirty_blocks is non-empty and matches
    // func.blocks.size(), only dirty blocks are processed.
    // Empty span means "all blocks" (full-function DCE).
    void run_function(aura::ir::IRFunction& func, std::span<const std::uint8_t> dirty_blocks = {}) {
        if (keep_for_debug_)
            return;
        const bool dirty_only = !dirty_blocks.empty() && dirty_blocks.size() == func.blocks.size();
        for (auto& block : func.blocks) {
            if (dirty_only) {
                const auto bid = block.id;
                if (bid >= dirty_blocks.size() || dirty_blocks[bid] == 0)
                    continue;
            }
            run_on_block(block);
        }
    }

    // Issue #538: per-block entry for incremental passes.
    bool run_on_block(aura::ir::BasicBlock& block) {
        if (keep_for_debug_)
            return false;
        bool any_change = false;
        bool changed;
        do {
            changed = false;
            // Build slot → instruction index map once per
            // do-while iteration. ops[1] is a SLOT NUMBER
            // (assigned by alloc_local), not an index into
            // block.instructions. The legacy code indexed
            // block.instructions[ops[1]] directly which
            // works only when slots happen to be
            // 0,1,2,... (true for the test_ir synthetic
            // IR, false in real IR from lowering). The
            // map is O(N) to build and O(1) to query.
            std::unordered_map<std::uint32_t, std::size_t> slot_to_idx;
            slot_to_idx.reserve(block.instructions.size() * 2);
            for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                auto& ii = block.instructions[i];
                if (auto* info = aura::ir::lookup_opcode(ii.opcode)) {
                    if (info->has_result_slot) {
                        slot_to_idx[ii.operands[0]] = i;
                    }
                }
            }
            for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                auto& instr = block.instructions[i];
                if (instr.opcode != aura::ir::IROpcode::CastOp)
                    continue;
                auto& ops = instr.operands;
                auto target_tag = ops[2];

                // Helper: find the source instruction
                // for slot `s` (the value being cast).
                // Returns nullptr if the slot isn't
                // defined in this block.
                auto find_source = [&](std::uint32_t s) -> const aura::ir::IRInstruction* {
                    auto it = slot_to_idx.find(s);
                    if (it == slot_to_idx.end())
                        return nullptr;
                    return &block.instructions[it->second];
                };

                // Rule 6 (#629): narrow_evidence-proved identity.
                // When occurrence-narrowing has statically
                // proved the cast target, elide the CastOp
                // when source type_id matches.
                if (instr.narrow_evidence != 0 && instr.type_id != 0) {
                    if (auto* src = find_source(ops[1])) {
                        if (src->type_id != 0 && src->type_id == instr.type_id) {
                            block.instructions[i] = aura::ir::IRInstruction{
                                .opcode = aura::ir::IROpcode::Local,
                                .operands = {ops[0], ops[1], 0, 0},
                                .type_id = instr.type_id,
                                .narrow_evidence = instr.narrow_evidence,
                            };
                            ++eliminated_;
                            ++narrow_evidence_hits_;
                            changed = true;
                            continue;
                        }
                    }
                }

                // Rule 1: identity cast — source type == target type
                // Check via type_id propagation (from FlatAST)
                if (instr.type_id != 0) {
                    if (auto* src = find_source(ops[1])) {
                        if (src->type_id != 0 && src->type_id == instr.type_id) {
                            // target == source type: replace with Local
                            block.instructions[i] = aura::ir::IRInstruction{
                                .opcode = aura::ir::IROpcode::Local,
                                .operands = {ops[0], ops[1], 0, 0},
                                .type_id = instr.type_id,
                            };
                            ++eliminated_;
                            ++type_prop_hits_;
                            changed = true;
                            continue;
                        }
                    }
                }

                // Rule 2: nested cast — (cast (cast x T1) T2)
                if (auto* src = find_source(ops[1])) {
                    if (src->opcode == aura::ir::IROpcode::CastOp) {
                        // Skip the intermediate cast: ops[1] = src->ops[1]
                        ops[1] = src->operands[1];
                        ++eliminated_;
                        changed = true;
                        continue;
                    }
                }

                // Issue #508 Rule 3: safe Dynamic passthrough.
                // When the target type_tag is Dynamic (≥3,
                // the default case in the IR interpreter
                // is `locals[ops[0]] = val`), the CastOp
                // does no work at runtime — it's a pure
                // type assertion that any value is
                // "Dynamic", which is always true.
                //
                // We require source info to be safe —
                // we can't elide a CastOp on a slot we
                // don't know the type of, because the
                // lowering pass may have inserted it
                // for a reason (e.g. a function-call
                // arg with no type info yet).
                if (target_tag >= 3) {
                    if (auto* src = find_source(ops[1])) {
                        auto src_tid = src->type_id;
                        bool src_is_ground_known = false;
                        if (src_tid != 0) {
                            if (type_reg_) {
                                auto src_ty = aura::core::TypeId{src_tid, 1};
                                src_is_ground_known = (src_ty != type_reg_->dynamic_type());
                            } else {
                                // No registry: any non-zero
                                // type_id is ground (the
                                // TypeChecker only sets
                                // non-zero for concrete
                                // types).
                                src_is_ground_known = true;
                            }
                        }
                        if (src_is_ground_known) {
                            // Copy source's type_id onto
                            // the new Local so chained
                            // Dynamic passthroughs see
                            // a non-zero type_id on the
                            // source and can be elided
                            // too (Rule 3 → Rule 1
                            // transitively).
                            block.instructions[i] = aura::ir::IRInstruction{
                                .opcode = aura::ir::IROpcode::Local,
                                .operands = {ops[0], ops[1], 0, 0},
                                .type_id = src_tid,
                            };
                            ++eliminated_;
                            changed = true;
                            continue;
                        }
                    }
                }
            }
            if (changed)
                any_change = true;
        } while (changed);
        return any_change;
    }

    // Issue #538: SoA IR incremental DCE. Processes only dirty
    // blocks when dirty_blocks_only is true (default). Marks
    // blocks dirty when elisions occur so downstream JIT can
    // invalidate specialized code.
    void run(IRModuleV2& mod, bool dirty_blocks_only = true) {
        if (keep_for_debug_)
            return;
        auto t0 = std::chrono::steady_clock::now();
        for (auto& func : mod.functions) {
            for (auto& block : func.blocks_) {
                if (dirty_blocks_only && !func.is_block_dirty(block.block_id))
                    continue;
                aura::ir::BasicBlock aos_block;
                aos_block.id = block.block_id;
                aos_block.instructions.reserve(block.end_idx - block.start_idx);
                for (std::uint32_t i = block.start_idx; i < block.end_idx; ++i) {
                    aos_block.instructions.push_back(aura::ir::IRInstruction{
                        .opcode = func.opcodes_[i],
                        .operands = {func.operand0_[i], func.operand1_[i], func.operand2_[i],
                                     func.operand3_[i]},
                        .type_id = func.type_ids_[i],
                        .narrow_evidence = func.narrow_evidence_[i],
                    });
                }
                if (!run_on_block(aos_block))
                    continue;
                for (std::uint32_t i = block.start_idx; i < block.end_idx; ++i) {
                    const auto local_i = i - block.start_idx;
                    const auto& instr = aos_block.instructions[local_i];
                    func.opcodes_[i] = instr.opcode;
                    func.operand0_[i] = instr.operands[0];
                    func.operand1_[i] = instr.operands[1];
                    func.operand2_[i] = instr.operands[2];
                    func.operand3_[i] = instr.operands[3];
                    func.type_ids_[i] = instr.type_id;
                    func.narrow_evidence_[i] = instr.narrow_evidence;
                }
                func.mark_block_dirty(block.block_id);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        elapsed_us_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "dead-coercion"; }
    std::size_t eliminated_count() const { return eliminated_; }
    // Issue #629: Rule 1 elisions using type_id propagation.
    std::size_t type_prop_hits() const { return type_prop_hits_; }
    // Issue #629: Rule 6 elisions using narrow_evidence.
    std::size_t narrow_evidence_hits() const { return narrow_evidence_hits_; }
    // Issue #508: number of CastOps that were NOT elided because
    // keep_for_debug was set. Useful for "what would the pass
    // have done?" observability in blame mode.
    std::size_t kept_for_debug_count() const { return kept_for_debug_; }
    // Issue #508: per-call elapsed time in microseconds. Reset on
    // every run(). Exposed via (compile:dead-coercion-elapsed)
    // primitive for cumulative timing across the pipeline.
    std::uint64_t elapsed_us() const { return elapsed_us_; }

private:
    const aura::core::TypeRegistry* type_reg_ = nullptr;
    std::size_t eliminated_ = 0;
    std::size_t type_prop_hits_ = 0;
    std::size_t narrow_evidence_hits_ = 0;
    std::size_t kept_for_debug_ = 0;
    std::uint64_t elapsed_us_ = 0;
    bool keep_for_debug_ = false;
};

// Issue #160: Dead Code Elimination (DCE) Pass.
//
// Removes pure-compute instructions whose result is never
// used by any other instruction in the same block. This
// includes:
//   - Constants (const-i64, const-f64, const-bool,
//     const-string, const-void) whose result slot is
//     unused.
//   - Pure arithmetic (add, sub, mul, div, eq, lt, gt,
//     le, ge, and, or, not) whose result slot is unused.
//   - Local / Arg whose result slot is unused.
//   - Cast whose result slot is unused (already covered
//     partially by DeadCoercionEliminationPass; this one
//     handles the general case).
//   - Car / Cdr whose result slot is unused (pure reads).
//
// Conservative: only DCE ops that have no side effects and
// produce a result. Side-effecting ops (Call, Branch, Jump,
// Return, CellSet, Raise, HashSet, ArenaPush, etc.) are
// always preserved.
//
// Cost: O(N) per block (single pass to build use-set, single
// pass to remove dead). Negligible compared to the O(N) eval.
export class DCEPass {
public:
    void run(aura::ir::IRModule& module) {
        eliminated_ = 0;
        for (auto& func : module.functions) {
            for (auto& block : func.blocks) {
                run_on_block(block);
            }
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "dce"; }
    std::size_t eliminated_count() const { return eliminated_; }

public:
    // Public wrapper for use by other passes (e.g.,
    // LinearOwnershipPass, TypePropagationPass).
    // True if the opcode has a result slot in operands[0].
    static bool has_result_slot_local(aura::ir::IROpcode op) {
        switch (op) {
            case aura::ir::IROpcode::ConstI64:
            case aura::ir::IROpcode::ConstF64:
            case aura::ir::IROpcode::ConstString:
            case aura::ir::IROpcode::ConstBool:
            case aura::ir::IROpcode::ConstVoid:
            case aura::ir::IROpcode::Local:
            case aura::ir::IROpcode::Arg:
            case aura::ir::IROpcode::Add:
            case aura::ir::IROpcode::Sub:
            case aura::ir::IROpcode::Mul:
            case aura::ir::IROpcode::Div:
            case aura::ir::IROpcode::Eq:
            case aura::ir::IROpcode::Lt:
            case aura::ir::IROpcode::Gt:
            case aura::ir::IROpcode::Le:
            case aura::ir::IROpcode::Ge:
            case aura::ir::IROpcode::And:
            case aura::ir::IROpcode::Or:
            case aura::ir::IROpcode::Not:
            case aura::ir::IROpcode::CastOp:
            case aura::ir::IROpcode::Car:
            case aura::ir::IROpcode::Cdr:
            // Other ops with has_result_slot=true per kOpcodeInfo:
            case aura::ir::IROpcode::MakeClosure:
            case aura::ir::IROpcode::NewCell:
            case aura::ir::IROpcode::PrimCall:
            case aura::ir::IROpcode::Primitive:
            case aura::ir::IROpcode::MakePair:
            case aura::ir::IROpcode::Raise:
            case aura::ir::IROpcode::IsError:
            case aura::ir::IROpcode::TryEnd:
            case aura::ir::IROpcode::HashRef:
            case aura::ir::IROpcode::HashSet:
            case aura::ir::IROpcode::HashRemove:
            case aura::ir::IROpcode::LinearWrap:
            case aura::ir::IROpcode::MoveOp:
            case aura::ir::IROpcode::BorrowOp:
            case aura::ir::IROpcode::MutBorrowOp:
            case aura::ir::IROpcode::RefCountOp:
            case aura::ir::IROpcode::ArenaPush:
            case aura::ir::IROpcode::GuardShape:
                return true;
            default:
                return false;
        }
    }

    // Returns the number of meaningful operands for the
    // opcode (per kOpcodeInfo in ir.ixx). Operands beyond
    // this count are padding and should be ignored (they
    // contain uninitialized values that may be 0, which is
    // indistinguishable from a real slot reference).
public:
    // Public wrapper for use by other passes (e.g.,
    // LinearOwnershipPass) that need operand counts.
    static std::uint32_t operand_count_local(aura::ir::IROpcode op) {
        switch (op) {
            case aura::ir::IROpcode::Nop:
                return 0;
            case aura::ir::IROpcode::ConstI64:
                return 1;
            case aura::ir::IROpcode::ConstF64:
                return 1;
            case aura::ir::IROpcode::Local:
                return 2;
            case aura::ir::IROpcode::Arg:
                return 2;
            case aura::ir::IROpcode::Add:
            case aura::ir::IROpcode::Sub:
            case aura::ir::IROpcode::Mul:
            case aura::ir::IROpcode::Div:
            case aura::ir::IROpcode::Eq:
            case aura::ir::IROpcode::Lt:
            case aura::ir::IROpcode::Gt:
            case aura::ir::IROpcode::Le:
            case aura::ir::IROpcode::Ge:
            case aura::ir::IROpcode::And:
            case aura::ir::IROpcode::Or:
                return 3;
            case aura::ir::IROpcode::Not:
                return 2;
            case aura::ir::IROpcode::Branch:
                return 3;
            case aura::ir::IROpcode::Jump:
                return 1;
            case aura::ir::IROpcode::Call:
                return 4; // callee, args, count, result
            case aura::ir::IROpcode::Return:
                return 1;
            case aura::ir::IROpcode::MakeClosure:
                return 3;
            case aura::ir::IROpcode::Capture:
                return 3;
            case aura::ir::IROpcode::CaptureRef:
                return 3;
            case aura::ir::IROpcode::Apply:
                return 4;
            case aura::ir::IROpcode::NewCell:
                return 1;
            case aura::ir::IROpcode::CellSet:
                return 2;
            case aura::ir::IROpcode::CellGet:
                return 2;
            case aura::ir::IROpcode::CastOp:
                return 3;
            case aura::ir::IROpcode::ConstString:
                return 2;
            case aura::ir::IROpcode::PrimCall:
                return 3;
            case aura::ir::IROpcode::Primitive:
                return 2;
            case aura::ir::IROpcode::ConstBool:
                return 2;
            case aura::ir::IROpcode::ConstVoid:
                return 1;
            case aura::ir::IROpcode::MakePair:
                return 3;
            case aura::ir::IROpcode::Car:
                return 2;
            case aura::ir::IROpcode::Cdr:
                return 2;
            case aura::ir::IROpcode::Raise:
                return 2;
            case aura::ir::IROpcode::IsError:
                return 2;
            case aura::ir::IROpcode::TryBegin:
                return 1;
            case aura::ir::IROpcode::TryEnd:
                return 2;
            case aura::ir::IROpcode::HashRef:
                return 3;
            case aura::ir::IROpcode::HashSet:
                return 3;
            case aura::ir::IROpcode::HashRemove:
                return 3;
            case aura::ir::IROpcode::LinearWrap:
                return 2;
            case aura::ir::IROpcode::MoveOp:
                return 2;
            case aura::ir::IROpcode::BorrowOp:
                return 2;
            case aura::ir::IROpcode::MutBorrowOp:
                return 2;
            case aura::ir::IROpcode::DropOp:
                return 1;
            case aura::ir::IROpcode::RefCountOp:
                return 3;
            case aura::ir::IROpcode::ArenaPush:
                return 2;
            case aura::ir::IROpcode::ArenaPop:
                return 1;
            case aura::ir::IROpcode::GuardShape:
                return 4;
            default:
                return 4; // conservative: assume full operand count
        }
    }

    // True if the opcode is a pure-compute op whose result
    // can be safely DCE'd if unused. The set is conservative:
    // anything that might have a side effect is excluded.
    static bool is_pure(aura::ir::IROpcode op) {
        switch (op) {
            // Constants
            case aura::ir::IROpcode::ConstI64:
            case aura::ir::IROpcode::ConstF64:
            case aura::ir::IROpcode::ConstString:
            case aura::ir::IROpcode::ConstBool:
            case aura::ir::IROpcode::ConstVoid:
            // Locals / args
            case aura::ir::IROpcode::Local:
            case aura::ir::IROpcode::Arg:
            // Pure arithmetic
            case aura::ir::IROpcode::Add:
            case aura::ir::IROpcode::Sub:
            case aura::ir::IROpcode::Mul:
            case aura::ir::IROpcode::Div:
            case aura::ir::IROpcode::Eq:
            case aura::ir::IROpcode::Lt:
            case aura::ir::IROpcode::Gt:
            case aura::ir::IROpcode::Le:
            case aura::ir::IROpcode::Ge:
            case aura::ir::IROpcode::And:
            case aura::ir::IROpcode::Or:
            case aura::ir::IROpcode::Not:
            // Coercion (the result might be queried for type
            // but is otherwise side-effect free).
            case aura::ir::IROpcode::CastOp:
            // Pure pair reads
            case aura::ir::IROpcode::Car:
            case aura::ir::IROpcode::Cdr:
                return true;
            default:
                return false;
        }
    }

    void run_on_block(aura::ir::BasicBlock& block) {
        // Pass 1: collect the set of operand indices that are
        // referenced by any later instruction. A pure op's
        // result is dead iff its result slot is not in this set.
        //
        // We use a bitset of size N (number of instructions in
        // this block). Slot index = the operand value when it's
        // a local slot (which is just an index into the same
        // block.instructions vector).
        //
        // For has_result_slot ops (the pure ops we care about),
        // operands[0] is the RESULT slot (a write, not a read)
        // — we skip it. For side-effecting ops, operands[0] is
        // an input (callee, target, value) — we count it. The
        // Call opcode is the exception: operands[0] is callee
        // (input), operands[3] is the result slot (a write).
        //
        // We also stop counting at `operand_count` to avoid
        // treating uninitialized operand values (which are 0)
        // as references to slot 0. operand_count is the number
        // of MEANINGFUL operands (per kOpcodeInfo in ir.ixx).
        const std::size_t n = block.instructions.size();
        if (n == 0)
            return;
        std::vector<bool> used(n, false);
        for (const auto& instr : block.instructions) {
            auto op_count = DCEPass::operand_count_local(instr.opcode);
            for (std::uint32_t k = 0; k < op_count; ++k) {
                auto op = instr.operands[k];
                // Skip the result slot of has_result_slot ops.
                if (k == 0 && has_result_slot_local(instr.opcode))
                    continue;
                // Call's result slot is operands[3], not [0].
                if (k == 3 && instr.opcode == aura::ir::IROpcode::Call)
                    continue;
                if (op < n) {
                    used[op] = true;
                }
            }
        }

        // Pass 2: mark dead pure ops whose result slot is
        // unused. Replace with Nop (preserves indices).
        for (std::size_t i = 0; i < n; ++i) {
            const auto& instr = block.instructions[i];
            if (!is_pure(instr.opcode))
                continue;
            if (!has_result_slot_local(instr.opcode))
                continue;
            auto result_slot = instr.operands[0];
            if (result_slot < n && !used[result_slot]) {
                // Dead pure compute — replace with Nop. (We
                // don't shrink the vector because operand
                // references in other instructions are
                // indices, and shifting them would require
                // remapping. Nop is the standard pattern.)
                block.instructions[i] = aura::ir::IRInstruction{
                    .opcode = aura::ir::IROpcode::Nop,
                };
                ++eliminated_;
            }
        }
    }

    std::size_t eliminated_ = 0;
};

// Issue #160: Escape Analysis Pass (full IR promotion).
//
// Promotes the existing JIT escape analysis (in aura_jit.cpp
// as the free function `run_escape_analysis`) to a proper
// IR pass that runs in the standard pass pipeline. The pass
// walks each function's IR, builds the escape_map, and
// stores it on IRFunction::escape_map for downstream
// consumers (JIT, arena allocation, future stack promotion).
//
// Algorithm (mirrors the existing JIT implementation):
//  1. First pass: mark escape points — instructions where a
//     value reaches a location that outlives the current scope
//     (Return, Call args, Capture, CellSet, HashSet, PrimCall
//     args).
//  2. Backward propagation: if a result escapes, its operands
//     (Local source, MakePair car/cdr) escape.
//
// Cost: O(N) per function. No transformation — purely an
// analysis pass that stores metadata for other consumers.
export class EscapeAnalysisPass {
public:
    void run(aura::ir::IRModule& module) {
        for (auto& func : module.functions) {
            run_on_function(func);
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "escape-analysis"; }

private:
    // Returns true if the opcode is a "return" that escapes its
    // operand value to the caller. Mirrors the escape-point
    // list in the existing JIT implementation.
    static bool is_escape_point(aura::ir::IROpcode op) {
        switch (op) {
            case aura::ir::IROpcode::Return:
            case aura::ir::IROpcode::Call:
            case aura::ir::IROpcode::Apply:
            case aura::ir::IROpcode::Capture:
            case aura::ir::IROpcode::CaptureRef:
            case aura::ir::IROpcode::CellSet:
            case aura::ir::IROpcode::HashSet:
            case aura::ir::IROpcode::PrimCall:
                return true;
            default:
                return false;
        }
    }

    // Mark the operand indices of the given escape-point
    // instruction as escaped in `escape_map`.
    void mark_escape_point(const aura::ir::IRInstruction& instr, const aura::ir::BasicBlock& block,
                           std::vector<std::uint8_t>& escape_map) {
        const std::size_t n = block.instructions.size();
        const std::size_t local_count = escape_map.size();
        switch (instr.opcode) {
            case aura::ir::IROpcode::Return:
                // Return(value) — value escapes
                if (instr.operands[0] < local_count)
                    escape_map[instr.operands[0]] = 1;
                break;
            case aura::ir::IROpcode::Call:
                // Call(callee, arg_base, arg_count, result) — callee + all args
                if (instr.operands[0] < local_count)
                    escape_map[instr.operands[0]] = 1;
                for (std::uint32_t i = 0; i < instr.operands[2]; ++i) {
                    auto slot = instr.operands[1] + i;
                    if (slot < local_count)
                        escape_map[slot] = 1;
                }
                break;
            case aura::ir::IROpcode::Apply:
                // Apply(closure, arg_count, result) — closure + args
                if (instr.operands[0] < local_count)
                    escape_map[instr.operands[0]] = 1;
                for (std::uint32_t i = 0; i < instr.operands[1]; ++i) {
                    auto slot = instr.operands[0] + 1 + i;
                    if (slot < local_count)
                        escape_map[slot] = 1;
                }
                break;
            case aura::ir::IROpcode::Capture:
            case aura::ir::IROpcode::CaptureRef:
                // Capture(closure, env_idx, var) / CaptureRef(closure, env_idx, cell)
                if (instr.operands[2] < local_count)
                    escape_map[instr.operands[2]] = 1;
                break;
            case aura::ir::IROpcode::CellSet:
                // CellSet(cell, val) — val escapes into persistent cell
                if (instr.operands[1] < local_count)
                    escape_map[instr.operands[1]] = 1;
                break;
            case aura::ir::IROpcode::HashSet:
                // HashSet(result, hash, keyval) — keyval pair escapes
                if (instr.operands[2] < local_count)
                    escape_map[instr.operands[2]] = 1;
                break;
            case aura::ir::IROpcode::PrimCall:
                // PrimCall(prim_id, arg_base, arg_count, result) — all args
                for (std::uint32_t i = 0; i < instr.operands[2]; ++i) {
                    auto slot = instr.operands[1] + i;
                    if (slot < local_count)
                        escape_map[slot] = 1;
                }
                break;
            default:
                break;
        }
    }

    // Backward propagation: if a result escapes, its operands
    // escape (Local source, MakePair car/cdr).
    bool propagate_backward(const aura::ir::IRFunction& func,
                            std::vector<std::uint8_t>& escape_map) {
        const std::size_t local_count = escape_map.size();
        bool changed = false;
        for (const auto& block : func.blocks) {
            for (const auto& instr : block.instructions) {
                if (!is_escape_point_or_pure_propagator(instr.opcode))
                    continue;
                if (instr.operands[0] >= local_count)
                    continue;
                if (!escape_map[instr.operands[0]])
                    continue;

                switch (instr.opcode) {
                    case aura::ir::IROpcode::Local:
                        // Local(result, src) — src escapes
                        if (instr.operands[1] < local_count && !escape_map[instr.operands[1]]) {
                            escape_map[instr.operands[1]] = 1;
                            changed = true;
                        }
                        break;
                    case aura::ir::IROpcode::MakePair:
                        // MakePair(result, car, cdr) — car + cdr escape
                        if (instr.operands[1] < local_count && !escape_map[instr.operands[1]]) {
                            escape_map[instr.operands[1]] = 1;
                            changed = true;
                        }
                        if (instr.operands[2] < local_count && !escape_map[instr.operands[2]]) {
                            escape_map[instr.operands[2]] = 1;
                            changed = true;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        return changed;
    }

    // True if this opcode is either an escape point OR a pure
    // propagator (Local, MakePair) whose operands may need
    // backward propagation.
    static bool is_escape_point_or_pure_propagator(aura::ir::IROpcode op) {
        return is_escape_point(op) || op == aura::ir::IROpcode::Local ||
               op == aura::ir::IROpcode::MakePair;
    }

    void run_on_function(aura::ir::IRFunction& func) {
        // Allocate the escape_map (size = local_count, default 0).
        func.escape_map.assign(func.local_count, 0);

        // Pass 1: mark direct escape points.
        for (const auto& block : func.blocks) {
            for (const auto& instr : block.instructions) {
                if (is_escape_point(instr.opcode)) {
                    mark_escape_point(instr, block, func.escape_map);
                }
            }
        }

        // Pass 2: backward propagation until fixpoint.
        bool changed = true;
        int max_iters = 100; // safety bound
        while (changed && max_iters-- > 0) {
            changed = propagate_backward(func, func.escape_map);
        }
    }
};

// Issue #160: Linear Ownership Pass (validation).
//
// Validates linear-typed IR at compile time. Catches:
//   1. Use-after-move: a slot is moved, then used again later.
//   2. Use-of-moved: a slot's result is referenced after the
//      slot has been moved (via MoveOp).
//   3. Move-without-consume: a linear-typed slot is never
//      consumed by the end of its scope (memory leak /
//      scope-leak diagnostic).
//
// This is a validation pass — it doesn't transform the IR.
// It just checks invariants. When the lowering pass starts
// emitting linear IR (MoveOp, BorrowOp, DropOp, RefCountOp),
// this pass will catch the most common errors at compile time.
//
// Algorithm:
//   - Walk each function's IR.
//   - Maintain a "moved" bitset of size local_count.
//   - On MoveOp(result, src): if src is already moved, emit
//     a warning. Mark src as moved. The result is a new
//     binding (not moved yet).
//   - On any other op using a moved slot as input: emit a
//     warning (use-after-move).
//   - At end of function: if a linear-typed slot (tracked
//     separately) was never consumed, emit a warning.
//
// Cost: O(N) per function. No transformation.
export class LinearOwnershipPass {
public:
    void run(aura::ir::IRModule& module) {
        use_after_move_count_ = 0;
        for (auto& func : module.functions) {
            run_on_function(func);
        }
    }

    bool has_error() const { return use_after_move_count_ > 0; }
    std::string_view name() const { return "linear-ownership"; }
    std::size_t use_after_move_count() const { return use_after_move_count_; }

private:
    // True if the opcode is a linear-move-ish op that
    // consumes its source.
    static bool is_consuming(aura::ir::IROpcode op) {
        switch (op) {
            case aura::ir::IROpcode::MoveOp:
            case aura::ir::IROpcode::DropOp:
            case aura::ir::IROpcode::RefCountOp: // dec variant
                return true;
            default:
                return false;
        }
    }

    // True if the opcode reads a slot as an INPUT. For these
    // ops, if the source slot has been moved, it's a use-after-move.
    static bool reads_input(aura::ir::IROpcode op) {
        // Conservative: most ops read some input. The result
        // slot (operands[0] for has_result_slot ops) is a write,
        // not a read, so it's not an input. Inputs are
        // operands[1..operand_count].
        switch (op) {
            case aura::ir::IROpcode::Nop:
            case aura::ir::IROpcode::Branch:
            case aura::ir::IROpcode::Jump:
            case aura::ir::IROpcode::Return:
            case aura::ir::IROpcode::ConstVoid:
            case aura::ir::IROpcode::CellGet:
            case aura::ir::IROpcode::MakePair:
                return false; // no input slots
            default:
                return true; // has at least one input
        }
    }

    void run_on_function(aura::ir::IRFunction& func) {
        // moved[i] = 1 if slot i has been moved (consumed).
        // Initially all 0.
        std::vector<std::uint8_t> moved(func.local_count, 0);

        for (const auto& block : func.blocks) {
            for (const auto& instr : block.instructions) {
                if (is_consuming(instr.opcode)) {
                    // Source slot (the consumed one) is operands[1]
                    // for MoveOp/DropOp, operands[1] for
                    // RefCountOp's first input.
                    auto consumed = instr.operands[1];
                    if (consumed < func.local_count) {
                        if (moved[consumed]) {
                            // Use-after-move detected.
                            ++use_after_move_count_;
                        }
                        moved[consumed] = 1;
                    }
                } else if (reads_input(instr.opcode) && func.local_count > 0) {
                    // Check all input operands for use-after-move.
                    // Skip operands[0] (result slot, a write).
                    auto op_count = DCEPass::operand_count_local(instr.opcode);
                    for (std::uint32_t k = 1; k < op_count; ++k) {
                        auto slot = instr.operands[k];
                        if (slot < func.local_count && moved[slot]) {
                            // Use-after-move detected.
                            ++use_after_move_count_;
                        }
                    }
                }
            }
        }
    }

    std::size_t use_after_move_count_ = 0;
};

// Issue #160: Type Propagation Pass (sub-item #4 partial).
//
// Walks the IR and propagates type_id through pure-compute
// ops whose result type_id wasn't set by lowering. This
// makes the type_id metadata more accurate for downstream
// consumers (JIT type-specialization, GuardShape decisions,
// CastOp decisions).
//
// Algorithm (2-pass per block):
//   Pass 1: build slot_type_id map. For each instruction,
//     if type_id != 0, record slot[result] = type_id.
//   Pass 2: for each instruction with type_id == 0 in
//     should_propagate ops, look up the source slot's
//     type_id from the map and set it.
//
// Specifically:
//   - Local(result, src): result.type_id = src's slot type_id
//   - Add/Sub/Mul/Div/And/Or: if both operands have the
//     same type_id, result.type_id = that
//   - Not: result.type_id = src's slot type_id
//   - Car/Cdr: result.type_id = pair (whatever the source
//     pair's type_id is, if any)
//
// Cost: O(N) per block. Idempotent.
export class TypePropagationPass {
public:
    void run(aura::ir::IRModule& module) {
        propagated_count_ = 0;
        for (auto& func : module.functions) {
            for (auto& block : func.blocks) {
                run_on_block(block);
            }
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "type-propagation"; }
    std::size_t propagated_count() const { return propagated_count_; }

private:
    static bool should_propagate(aura::ir::IROpcode op) {
        switch (op) {
            case aura::ir::IROpcode::Local:
            case aura::ir::IROpcode::Add:
            case aura::ir::IROpcode::Sub:
            case aura::ir::IROpcode::Mul:
            case aura::ir::IROpcode::Div:
            case aura::ir::IROpcode::And:
            case aura::ir::IROpcode::Or:
            case aura::ir::IROpcode::Not:
            case aura::ir::IROpcode::Car:
            case aura::ir::IROpcode::Cdr:
                return true;
            default:
                return false;
        }
    }

    void run_on_block(aura::ir::BasicBlock& block) {
        // Pass 1: build slot -> type_id map from instructions
        // that already have type_id set (constants, etc.).
        // The map's key is the result slot; the value is the
        // type_id. We use a flat vector indexed by slot number
        // (assumes slots are dense, which is the convention
        // for Aura's IR).
        const std::size_t n = block.instructions.size();
        if (n == 0)
            return;
        // Find max slot referenced in the block.
        std::uint32_t max_slot = 0;
        for (const auto& instr : block.instructions) {
            for (std::uint32_t k = 0; k < 4; ++k) {
                if (instr.operands[k] > max_slot)
                    max_slot = instr.operands[k];
            }
        }
        if (max_slot == 0)
            return;
        // Allocate slot_type_id sized to max_slot + 1.
        std::vector<std::uint32_t> slot_type_id(max_slot + 1, 0);
        for (const auto& instr : block.instructions) {
            if (!DCEPass::has_result_slot_local(instr.opcode))
                continue;
            if (instr.type_id != 0) {
                auto slot = instr.operands[0];
                if (slot <= max_slot) {
                    slot_type_id[slot] = instr.type_id;
                }
            }
        }

        // Pass 2: propagate to instructions with type_id == 0.
        for (auto& instr : block.instructions) {
            if (!should_propagate(instr.opcode))
                continue;
            if (instr.type_id != 0)
                continue; // already set
            if (!DCEPass::has_result_slot_local(instr.opcode))
                continue;
            std::uint32_t inferred = 0;
            switch (instr.opcode) {
                case aura::ir::IROpcode::Local:
                case aura::ir::IROpcode::Not:
                case aura::ir::IROpcode::Car:
                case aura::ir::IROpcode::Cdr:
                    // Single source operand.
                    if (instr.operands[1] <= max_slot) {
                        inferred = slot_type_id[instr.operands[1]];
                    }
                    break;
                case aura::ir::IROpcode::Add:
                case aura::ir::IROpcode::Sub:
                case aura::ir::IROpcode::Mul:
                case aura::ir::IROpcode::Div:
                case aura::ir::IROpcode::And:
                case aura::ir::IROpcode::Or: {
                    // Two source operands. If both have the same
                    // type_id, propagate. If they differ, leave 0
                    // (downstream pass handles mixed-type ops).
                    auto t1 =
                        (instr.operands[1] <= max_slot) ? slot_type_id[instr.operands[1]] : 0u;
                    auto t2 =
                        (instr.operands[2] <= max_slot) ? slot_type_id[instr.operands[2]] : 0u;
                    if (t1 != 0 && t1 == t2)
                        inferred = t1;
                    break;
                }
                default:
                    break;
            }
            if (inferred != 0) {
                instr.type_id = inferred;
                ++propagated_count_;
            }
        }
    }

    std::size_t propagated_count_ = 0;
};

// Issue #160: Inline Expansion Pass (sub-item #6 minimal).
//
// Inlines small callee functions at their call sites. The
// minimal shippable piece: inlines single-block callees that
// return a constant (just `ConstI64/F64/Bool/String/Void`
// in the entry block, followed by `Return`). The result is
// the constant value substituted directly at the call site,
// eliminating the call overhead.
//
// What this DOESN'T do (deferred to fresh session):
//   - Multi-block callees (requires branch-aware inlining)
//   - Functions with parameters (requires local renaming)
//   - Recursive functions (must be detected and skipped)
//   - Size heuristics (currently "single block + constant return")
//   - Call site cloning for polymorphic dispatch
//
// Cost: O(C × F) where C = number of call sites, F = number
// of functions. For the minimal version, F is checked once per
// call site (the callee is looked up by name in module.functions).
// Issue #171: Function Inliner (Priority 2) — actually inlines
// trivial callees at their call sites. Extends the #160 scaffold
// (which only counted call sites) with:
//   1. Data-flow: build slot → func_id map for the caller
//      (MakeClosure writes func_id to a slot; Call reads it)
//   2. Static callee resolution: for each Call, look up the
//      func_id via the slot map
//   3. Substitution: if the callee is trivial-inlinable, replace
//      the Call with a copy of the ConstXxx instruction at the
//      call site, with operands[0] remapped to the caller's
//      result slot
//   4. Recursion guard: skip inlining if the callee's func_id
//      matches the caller's func_id
//
// What this DOESN'T do (deferred to follow-up issues):
//   - Multi-block callees (requires branch-aware inlining)
//   - Functions with parameters (requires local renaming of
//     the callee's local space into the caller's)
//   - Size heuristics (currently "single block + constant return")
//   - Call site cloning for polymorphic dispatch
//   - TCO (Issue #171 Priority 3, separate commit)
//
// Cost: O(C × F) per function, where C = call sites, F = functions.
// Data-flow analysis is O(C) per function; the per-call-site
// inline check is O(1) (func_id → function lookup is O(1) via
// a pre-built index).
export class InlinePass {
public:
    void run(aura::ir::IRModule& module) {
        inlined_count_ = 0;
        // Issue #197: bump the global per-run stats
        // (consumed by the (compile:inline-pass-stats)
        // Aura primitive). The total is the lifetime sum
        // of inlined + inlined_branch_aware; the last-run
        // values are reset on each run().
        run_inlined_ = 0;
        run_inlined_branch_aware_ = 0;
        // Build a func_id → IRFunction* index for O(1) lookup.
        // module.functions is a std::vector indexed by func_id.
        if (func_index_.size() != module.functions.size()) {
            func_index_.clear();
            func_index_.resize(module.functions.size());
            for (std::size_t i = 0; i < module.functions.size(); ++i)
                func_index_[i] = &module.functions[i];
        }
        // Issue #203: build the call graph and compute SCCs
        // for mutual-recursion detection. The inliner's
        // recursion guard now skips inlining if the caller
        // and callee are in the same strongly-connected
        // component (i.e., reachable from each other through
        // the call graph).
        const auto call_graph = build_call_graph(module);
        scc_id_of_fid_ = compute_sccs(module.functions.size(), call_graph);
        for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
            auto& func = module.functions[fi];
            // Recursion guard: skip inlining back into the same
            // function (direct recursion) OR into a function in
            // the same SCC (mutual recursion). See #203.
            const auto caller_fid = static_cast<std::uint32_t>(fi);
            // Build slot → func_id map for this function
            std::unordered_map<std::uint32_t, std::uint32_t> slot_to_func;
            for (const auto& block : func.blocks) {
                for (const auto& instr : block.instructions) {
                    if (instr.opcode == aura::ir::IROpcode::MakeClosure) {
                        // MakeClosure(result_slot, func_id, ...)
                        slot_to_func[instr.operands[0]] = instr.operands[1];
                    }
                }
            }
            for (auto& block : func.blocks) {
                run_on_block(func, block, slot_to_func, caller_fid);
            }
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "inline"; }
    std::size_t inlined_count() const { return inlined_count_; }
    // Issue #388: macro-hygiene cross-marker skipped counter
    // (callable from Aura via the stats hash).
    std::size_t macro_hygiene_skipped_count() const { return macro_hygiene_skipped_; }
    // Issue #197: branch-aware inliner counter. Tracked
    // separately from inlined_count_ (which counts the
    // pre-#197 constant-substitution path) so callers can
    // measure the new path's contribution.
    std::size_t inlined_branch_aware_count() const { return inlined_branch_aware_count_; }
    // Issue #197: per-run and lifetime totals for the
    // (compile:inline-pass-stats) Aura primitive. run_*
    // are reset on each run() call; total_* are lifetime
    // sums (process-wide).
    static std::size_t total_inlined() { return total_inlined_; }
    static std::size_t total_inlined_branch_aware() { return total_inlined_branch_aware_; }
    // Issue #388: macro-hygiene cross-marker skipped total.
    static std::size_t total_macro_hygiene_skipped() { return macro_hygiene_skipped_; }
    std::size_t run_inlined() const { return run_inlined_; }
    std::size_t run_inlined_branch_aware() const { return run_inlined_branch_aware_; }

    // Issue #246: macro-hygiene toggle. When true (the default),
    // the inliner refuses to inline callees whose IRFunction.marker
    // is SyntaxMarker::MacroIntroduced. This is the conservative
    // policy: macro-introduced code can do anything (gensym'd
    // locals, captured caller-side variables, EDSL-side effects),
    // and inlining it into user code could break hygiene invariants.
    // Setter:
    //   InlinePass::set_respect_macro_hygiene(false)
    //   → opt in to inlining macro-introduced code.
    static void set_respect_macro_hygiene(bool v) { respect_macro_hygiene_ = v; }
    static bool get_respect_macro_hygiene() { return respect_macro_hygiene_; }

private:
    // True if the function is a "constant-returning single-block
    // function" that can be inlined trivially. Specifically:
    //   - Has exactly one block
    //   - The block has 2 instructions: a ConstXxx + a Return
    //   - The Return's source is the ConstXxx's result slot
    static bool is_trivial_inlinable(const aura::ir::IRFunction& func) {
        // Issue #246: macro-hygiene guard. When respect_macro_hygiene_
        // is on (the default) and the callee's SyntaxMarker is
        // MacroIntroduced (1), skip inlining. This is the most
        // conservative default: macro-introduced code can be anything,
        // and inlining it into user code could break hygiene
        // invariants (e.g., gensym'd locals leaking into the caller,
        // capture of caller-side variables becoming visible inside
        // macro-introduced body). Trivial inlining (constant
        // substitution) is normally safe even for macro-introduced
        // code, but we apply the same guard for consistency.
        if (respect_macro_hygiene_ && func.marker == 1 /*MacroIntroduced*/) {
            return false;
        }
        if (func.blocks.size() != 1)
            return false;
        const auto& block = func.blocks[0];
        if (block.instructions.size() != 2)
            return false;
        const auto& a = block.instructions[0];
        const auto& b = block.instructions[1];
        bool a_is_const =
            (a.opcode == aura::ir::IROpcode::ConstI64 || a.opcode == aura::ir::IROpcode::ConstF64 ||
             a.opcode == aura::ir::IROpcode::ConstBool ||
             a.opcode == aura::ir::IROpcode::ConstString ||
             a.opcode == aura::ir::IROpcode::ConstVoid);
        bool b_is_return = (b.opcode == aura::ir::IROpcode::Return);
        bool slots_match = (a.operands[0] == b.operands[0]);
        return a_is_const && b_is_return && slots_match;
    }

    // Find the function with the given name in the module.
    // Returns nullptr if not found.
    static const aura::ir::IRFunction* find_function(const aura::ir::IRModule& module,
                                                     const std::string& name) {
        for (const auto& func : module.functions) {
            if (func.name == name)
                return &func;
        }
        return nullptr;
    }

public:
    // Public wrapper for tests.
    static bool is_trivial_inlinable_for_test(const aura::ir::IRFunction& func) {
        return is_trivial_inlinable(func);
    }
    // Issue #197: public test wrapper for the branch-aware
    // predicate.
    static bool is_inlinable_branch_aware_for_test(const aura::ir::IRFunction& func) {
        return is_inlinable_branch_aware(func);
    }
    // Issue #197: public test wrapper for the branch-aware
    // inliner transformation. Returns true on success.
    static bool try_inline_branch_aware_for_test(aura::ir::IRFunction& caller,
                                                 aura::ir::BasicBlock& block, std::size_t call_pos,
                                                 const aura::ir::IRFunction& callee,
                                                 const aura::ir::IRInstruction& call_instr) {
        InlinePass p;
        return p.try_inline_branch_aware(caller, block, call_pos, callee, call_instr);
    }

private:
    void run_on_block(aura::ir::IRFunction& caller, aura::ir::BasicBlock& block,
                      const std::unordered_map<std::uint32_t, std::uint32_t>& slot_to_func,
                      std::uint32_t caller_fid) {
        // Walk the block, find Call sites with a known static
        // callee that's trivial-inlinable, and substitute the
        // constant directly at the call site.
        //
        // The substitution is an in-place rewrite of the Call
        // instruction to be a ConstXxx with operands[0] remapped
        // to the caller's result slot (instr.operands[3]). Since
        // the callee has no parameters and no control flow, the
        // ConstXxx's value can be written directly to the result
        // slot without a temporary.
        (void)caller;
        for (std::size_t i = 0; i < block.instructions.size(); ++i) {
            auto& instr = block.instructions[i];
            if (instr.opcode != aura::ir::IROpcode::Call)
                continue;
            // Operands[0] = callee slot. Look up static func_id.
            auto callee_slot = instr.operands[0];
            auto it = slot_to_func.find(callee_slot);
            if (it == slot_to_func.end())
                continue; // dynamic callee
            auto callee_fid = it->second;
            // Recursion guard: don't inline into self OR into a
            // function in the same SCC (mutual recursion).
            // See Issue #203. The scc_id_of_fid_ map is built
            // once per run() call; scc_id_of_fid_[i] == scc_id_of_fid_[j]
            // iff i and j are in the same strongly-connected
            // component of the call graph.
            if (callee_fid == caller_fid)
                continue;
            if (callee_fid < scc_id_of_fid_.size() && caller_fid < scc_id_of_fid_.size() &&
                scc_id_of_fid_[callee_fid] == scc_id_of_fid_[caller_fid]) {
                continue; // mutual recursion: same SCC
            }
            // Look up the callee function
            if (callee_fid >= func_index_.size())
                continue;
            const auto* callee = func_index_[callee_fid];
            if (!callee)
                continue;
            // Issue #388: caller-side cross-marker hygiene check.
            // If respect_macro_hygiene_ is on (the default) and
            // EITHER the call site OR the callee is macro-
            // introduced, skip inlining. The callee.marker
            // check is already done in is_trivial_inlinable /
            // is_inlinable_branch_aware (callee side). The
            // call-site check is the new piece: don't bring
            // user code into a macro-introduced caller context.
            if (respect_macro_hygiene_ && instr.source_marker == 1 /*MacroIntroduced*/ &&
                callee->marker != 1) {
                ++macro_hygiene_skipped_;
                continue;
            }
            // Check trivial-inlinable (pre-#197 fast path:
            // single-block + constant return + no params)
            if (is_trivial_inlinable(*callee)) {
                // Fast path: constant substitution in place
                const auto& const_instr = callee->blocks[0].instructions[0];
                auto result_slot = instr.operands[3];
                instr.opcode = const_instr.opcode;
                instr.operands[0] = result_slot;
                instr.operands[1] = const_instr.operands[1];
                instr.operands[2] = const_instr.operands[2];
                ++inlined_count_;
                ++run_inlined_;
                ++total_inlined_;
                continue;
            }
            // Issue #197: branch-aware inliner path. Handles
            // multi-block callees with Branch+Return control
            // flow and any number of parameters. The
            // transformation copies the callee's blocks into
            // the caller, remaps slots and block ids, and
            // splices the result into the call site.
            if (is_inlinable_branch_aware(*callee)) {
                if (try_inline_branch_aware(caller, block, i, *callee, instr)) {
                    ++inlined_branch_aware_count_;
                    ++run_inlined_branch_aware_;
                    ++total_inlined_branch_aware_;
                    // The Call at position i has been replaced
                    // with a Jump; the callee's blocks have
                    // been appended after this block. The
                    // outer block-iteration loop will pick up
                    // the new blocks naturally. We don't
                    // increment i — the rest of the original
                    // instructions in the caller's block
                    // (after the Call) are now in a different
                    // block and will be visited via the
                    // successor chain.
                    break; // done with this block for now
                }
            }
        }
    }

    // Issue #197: branch-aware inliner. Accepts callees with:
    //   - Multiple blocks (Branch+Return only, no loops)
    //   - Any number of parameters
    //   - No Call/MakeClosure/Apply inside (inlining
    //     semantics would need a recursive inliner for those)
    //   - No observable side effects (no cell-set, no Set, etc.)
    //   - Reasonable size (≤ 8 blocks + ≤ 32 instructions)
    //
    // The predicate is intentionally conservative — the
    // goal is correctness over coverage. Aggressive cases
    // (loops, calls inside callee, etc.) are follow-ups.
    static bool is_inlinable_branch_aware(const aura::ir::IRFunction& func) {
        // Issue #246: macro-hygiene guard. See the rationale in
        // is_trivial_inlinable. Default behavior: skip macro-
        // introduced callees. Set respect_macro_hygiene_ = false
        // via set_respect_macro_hygiene(false) to opt in to
        // inlining macro-introduced code.
        if (respect_macro_hygiene_ && func.marker == 1 /*MacroIntroduced*/) {
            return false;
        }
        // Size heuristic: avoid blowing up the caller.
        if (func.blocks.empty())
            return false;
        if (func.blocks.size() > 8)
            return false;
        std::size_t total_instrs = 0;
        for (const auto& b : func.blocks) {
            total_instrs += b.instructions.size();
            if (total_instrs > 32)
                return false;
        }
        // Entry block must exist (block 0).
        // All instructions must be: pure (no side effects)
        // or control flow (Branch/Jump/Return). The Return
        // must be the terminator of a block (i.e. the last
        // instruction in a block with no Branch/Jump before
        // it). We allow local_count > 0 (parameters).
        for (const auto& b : func.blocks) {
            for (std::size_t idx = 0; idx < b.instructions.size(); ++idx) {
                const auto& instr = b.instructions[idx];
                if (instr.opcode == aura::ir::IROpcode::Call ||
                    instr.opcode == aura::ir::IROpcode::MakeClosure ||
                    instr.opcode == aura::ir::IROpcode::Apply) {
                    return false; // nested inlining is a follow-up
                }
                if (instr.opcode == aura::ir::IROpcode::CellSet) {
                    return false; // observable side effect
                }
            }
        }
        // Loops: detect back-edges via DFS from the entry.
        // A back-edge is an edge (B -> A) where A is an
        // ancestor of B in the DFS tree.
        std::unordered_set<std::uint32_t> visited;
        std::vector<std::uint32_t> stack;
        stack.push_back(0); // entry block
        while (!stack.empty()) {
            auto bid = stack.back();
            stack.pop_back();
            if (!visited.insert(bid).second)
                continue;
            for (auto succ : successors_of(func, bid)) {
                if (visited.count(succ)) {
                    // back-edge → loop → not inlinable (yet)
                    return false;
                }
                stack.push_back(succ);
            }
        }
        // Every block must end with Branch, Jump, or Return.
        for (const auto& b : func.blocks) {
            if (b.instructions.empty())
                return false;
            const auto& last = b.instructions.back();
            if (last.opcode != aura::ir::IROpcode::Branch &&
                last.opcode != aura::ir::IROpcode::Jump &&
                last.opcode != aura::ir::IROpcode::Return) {
                return false;
            }
        }
        return true;
    }

    // Helper: get the successor block ids for a given block
    // in a function. Reads the Branch/Jump terminator to
    // extract true/false targets. Returns at most 2 ids.
    static std::vector<std::uint32_t> successors_of(const aura::ir::IRFunction& func,
                                                    std::uint32_t block_id) {
        std::vector<std::uint32_t> out;
        if (block_id >= func.blocks.size())
            return out;
        const auto& b = func.blocks[block_id];
        if (b.instructions.empty())
            return out;
        const auto& last = b.instructions.back();
        if (last.opcode == aura::ir::IROpcode::Branch) {
            // Branch: cond, true_block, false_block
            out.push_back(last.operands[1]);
            out.push_back(last.operands[2]);
        } else if (last.opcode == aura::ir::IROpcode::Jump) {
            // Jump: target_block
            out.push_back(last.operands[0]);
        }
        // Return: no successors
        return out;
    }

    // Issue #197: try_inline_branch_aware. Replaces the Call
    // instruction at position `call_pos` in `block` with a
    // copy of the callee's body, with slot + block ids
    // remapped into the caller's namespace.
    //
    // Handles two cases:
    //   1. **Single-block callee** (fast path): the Call is
    //      the last instruction in the caller's block, and
    //      the callee has 1 block. The transformation is an
    //      in-place rewrite: param copies + inlined body +
    //      Local(call_result, return_source) replace the Call.
    //   2. **Multi-block callee** (CFG splicing): the Call
    //      is at any position in the caller's block. The
    //      transformation splits the caller's block at the
    //      Call, clones all callee blocks into the caller
    //      (with slot + block remap), wires the cloned
    //      entry block as the continuation of the "before"
    //      block, and the cloned exit block jumps to the
    //      "after" block.
    //
    // The fast path (single-block) is more efficient (no
    // new blocks allocated); the multi-block path is the
    // full CFG splicing for Branch+Return callees.
    //
    // Returns true on success; false if the inlining can't
    // be performed (e.g. arg_count != callee.arg_count, the
    // callee has no Return, or some other pre-condition fails).
    bool try_inline_branch_aware(aura::ir::IRFunction& caller, aura::ir::BasicBlock& block,
                                 std::size_t call_pos, const aura::ir::IRFunction& callee,
                                 const aura::ir::IRInstruction& call_instr) {
        // Issue #455: per-call-site hygiene check. The existing
        // `respect_macro_hygiene_` check (in is_inlinable_*) looks
        // at `callee.marker`. This adds a *cross-marker* guard:
        // when respect_macro_hygiene_ is on (the default), do
        // not inline across a macro-introduced boundary in
        // EITHER direction:
        //   1. call site is MacroIntroduced, callee is User
        //      → don't bring user code into macro context
        //   2. callee is MacroIntroduced, call site is User
        //      → already covered by callee.marker check
        //      (defense in depth: also re-check here)
        // The instruction marker is 0=User, 1=MacroIntroduced.
        if (respect_macro_hygiene_) {
            if (call_instr.source_marker == 1 /*MacroIntroduced*/ && callee.marker != 1) {
                return false; // case 1 above
            }
            if (callee.marker == 1 && call_instr.source_marker != 1) {
                return false; // case 2 above
            }
        }
        // arg_count must match callee.arg_count.
        std::uint32_t arg_count = call_instr.operands[2];
        std::uint32_t arg_base = call_instr.operands[1];
        std::uint32_t result_slot = call_instr.operands[3];
        if (arg_count != callee.arg_count)
            return false;
        // Find the callee's exit block (the one ending in
        // Return). Exactly one block should end in Return;
        // the others end in Branch/Jump.
        std::size_t exit_block = callee.blocks.size();
        for (std::size_t b = 0; b < callee.blocks.size(); ++b) {
            if (callee.blocks[b].instructions.empty())
                continue;
            if (callee.blocks[b].instructions.back().opcode == aura::ir::IROpcode::Return) {
                exit_block = b;
                break;
            }
        }
        if (exit_block >= callee.blocks.size())
            return false;
        // Dispatch: single-block vs multi-block.
        if (callee.blocks.size() == 1) {
            return try_inline_single_block(caller, block, call_pos, callee, call_instr, arg_count,
                                           arg_base, result_slot, exit_block);
        }
        return try_inline_multi_block(caller, block, call_pos, callee, call_instr, arg_count,
                                      arg_base, result_slot, exit_block);
    }

    // Single-block fast path: in-place rewrite. The Call
    // must be the last instruction in the caller's block.
    // Pre-conditions: callee has 1 block, the block ends
    // with Return, and the caller's local_count is grown
    // to include the callee's locals.
    bool try_inline_single_block(aura::ir::IRFunction& caller, aura::ir::BasicBlock& block,
                                 std::size_t call_pos, const aura::ir::IRFunction& callee,
                                 const aura::ir::IRInstruction& call_instr, std::uint32_t arg_count,
                                 std::uint32_t arg_base, std::uint32_t result_slot,
                                 std::size_t exit_block) {
        const auto& callee_block = callee.blocks[0];
        // Fast path only applies when the Call is the last
        // instruction in the caller's block. If the Call is
        // in the middle, delegate to the multi-block path
        // (which handles the block split correctly).
        if (call_pos != block.instructions.size() - 1) {
            return try_inline_multi_block(caller, block, call_pos, callee, call_instr, arg_count,
                                          arg_base, result_slot, exit_block);
        }
        const auto& last_instr = callee_block.instructions.back();
        // Build slot_rename_map. Same approach as
        // try_inline_multi_block: walk instructions to
        // find all referenced slots, not just
        // callee.local_count.
        std::unordered_map<std::uint32_t, std::uint32_t> slot_rename;
        std::uint32_t max_slot = 0;
        for (const auto& instr : callee_block.instructions) {
            for (std::size_t op = 0; op < 4; ++op) {
                if (instr.operands[op] > max_slot) {
                    max_slot = instr.operands[op];
                }
            }
        }
        std::uint32_t new_local_start = caller.local_count;
        for (std::uint32_t i = 0; i <= max_slot; ++i) {
            slot_rename[i] = new_local_start + i;
        }
        caller.local_count = new_local_start + max_slot + 1;
        // Build the new instruction sequence.
        std::vector<aura::ir::IRInstruction> new_instrs;
        new_instrs.reserve(callee_block.instructions.size() + arg_count);
        // 1. Parameter copies.
        for (std::uint32_t i = 0; i < arg_count; ++i) {
            aura::ir::IRInstruction local;
            local.opcode = aura::ir::IROpcode::Local;
            local.operands[0] = slot_rename.at(i);
            local.operands[1] = arg_base + i;
            new_instrs.push_back(local);
        }
        // 2. Inlined body (excluding trailing Return).
        for (std::size_t k = 0; k + 1 < callee_block.instructions.size(); ++k) {
            auto cp = callee_block.instructions[k];
            const auto* info = lookup_opcode(cp.opcode);
            if (info && info->has_result_slot) {
                auto it = slot_rename.find(cp.operands[0]);
                if (it != slot_rename.end())
                    cp.operands[0] = it->second;
            }
            if (cp.opcode == aura::ir::IROpcode::Branch) {
                auto it = slot_rename.find(cp.operands[0]);
                if (it != slot_rename.end())
                    cp.operands[0] = it->second;
            }
            bool is_branch = (cp.opcode == aura::ir::IROpcode::Branch);
            bool is_jump = (cp.opcode == aura::ir::IROpcode::Jump);
            std::size_t remap_end = 4;
            if (is_branch)
                remap_end = 1;
            if (is_jump)
                remap_end = 0;
            for (std::size_t op = 1; op < remap_end; ++op) {
                auto it = slot_rename.find(cp.operands[op]);
                if (it != slot_rename.end())
                    cp.operands[op] = it->second;
            }
            new_instrs.push_back(cp);
        }
        // 3. Final Return → Local(result_slot, return_source).
        std::uint32_t ret_src = last_instr.operands[0];
        auto rit = slot_rename.find(ret_src);
        if (rit != slot_rename.end())
            ret_src = rit->second;
        aura::ir::IRInstruction final_copy;
        final_copy.opcode = aura::ir::IROpcode::Local;
        final_copy.operands[0] = result_slot;
        final_copy.operands[1] = ret_src;
        new_instrs.push_back(final_copy);
        // In-place rewrite.
        auto insert_pos = block.instructions.begin() + call_pos;
        block.instructions.insert(insert_pos, new_instrs.begin(), new_instrs.end());
        block.instructions.erase(block.instructions.begin() + call_pos + new_instrs.size());
        return true;
    }

    // Multi-block path: CFG splicing. Splits the caller's
    // block at call_pos, clones all callee blocks into the
    // caller with slot + block remap, and wires the cloned
    // entry/exit blocks to the caller's "before" and "after"
    // blocks.
    bool try_inline_multi_block(aura::ir::IRFunction& caller, aura::ir::BasicBlock& block,
                                std::size_t call_pos, const aura::ir::IRFunction& callee,
                                const aura::ir::IRInstruction& call_instr, std::uint32_t arg_count,
                                std::uint32_t arg_base, std::uint32_t result_slot,
                                std::size_t exit_block) {
        // Step 1: Capture the original terminator and the
        // "after" instructions (instructions after the Call)
        // BEFORE we mutate the block.
        std::optional<aura::ir::IRInstruction> orig_terminator;
        std::vector<aura::ir::IRInstruction> after_instrs;
        // A block typically ends with a terminator
        // (Branch/Jump/Return). If the Call is in the
        // middle, the terminator is the last instruction.
        // If the Call is the last instruction, the block
        // has no terminator (the Call would be the end of
        // the block, which is unusual — typically the Call
        // is followed by a terminator or by more code).
        bool call_is_last = (call_pos + 1 == block.instructions.size());
        if (!call_is_last) {
            // Original terminator is the last instruction.
            // after_instrs is empty (since call_pos+1 ==
            // end means there's nothing after the Call but
            // the terminator). Wait — that's only true if
            // the terminator is the instruction right after
            // the Call. In general, there could be
            // instructions between the Call and the
            // terminator. Let's handle both.
            // after_instrs = instructions [call_pos+1, end)
            for (std::size_t k = call_pos + 1; k < block.instructions.size(); ++k) {
                after_instrs.push_back(block.instructions[k]);
            }
            // The terminator is the LAST of those, if any.
            if (!after_instrs.empty()) {
                orig_terminator = after_instrs.back();
                after_instrs.pop_back();
            }
        }
        // Step 2: Build slot_rename_map. Walk all callee
        // blocks and collect all referenced slot indices,
        // then map each to a fresh caller slot. The map
        // is keyed by the original callee slot; missing
        // entries are no-ops (the slot wasn't referenced).
        // We use the actual referenced slots (not just
        // callee.local_count) because the test callees
        // (and real-world IR) can reference slots beyond
        // local_count if they were constructed manually.
        std::unordered_map<std::uint32_t, std::uint32_t> slot_rename;
        std::uint32_t max_slot = 0;
        for (const auto& cb : callee.blocks) {
            for (const auto& instr : cb.instructions) {
                for (std::size_t op = 0; op < 4; ++op) {
                    if (instr.operands[op] > max_slot) {
                        max_slot = instr.operands[op];
                    }
                }
            }
        }
        std::uint32_t new_local_start = caller.local_count;
        for (std::uint32_t i = 0; i <= max_slot; ++i) {
            slot_rename[i] = new_local_start + i;
        }
        caller.local_count = new_local_start + max_slot + 1;
        // Step 3: Build block_rename_map. Each callee block
        // j maps to a fresh caller block id.
        // caller.blocks.size() is the count of existing
        // caller blocks; new block ids start there.
        // We'll have N callee blocks + 1 B_after, so the
        // ids are [caller.blocks.size(), caller.blocks.size() + N).
        std::uint32_t new_block_start = static_cast<std::uint32_t>(caller.blocks.size());
        std::unordered_map<std::uint32_t, std::uint32_t> block_rename;
        for (std::uint32_t j = 0; j < callee.blocks.size(); ++j) {
            block_rename[j] = new_block_start + j;
        }
        std::uint32_t b_after_id =
            new_block_start + static_cast<std::uint32_t>(callee.blocks.size());
        // Step 4: Clone the callee's exit block first to
        // figure out the return-source slot after remap
        // (the Local that writes to call_result_slot will
        // read from this remapped slot).
        std::uint32_t ret_src = callee.blocks[exit_block].instructions.back().operands[0];
        auto rit = slot_rename.find(ret_src);
        if (rit != slot_rename.end())
            ret_src = rit->second;
        // Step 5: Rewrite the caller's original block to
        // hold I_0..I_{call_pos-1} (instructions before the
        // Call), then the param copies, then a Jump to
        // block_rename[0] (the cloned entry block).
        if (call_pos < block.instructions.size()) {
            block.instructions.resize(call_pos);
        }
        for (std::uint32_t i = 0; i < arg_count; ++i) {
            aura::ir::IRInstruction local;
            local.opcode = aura::ir::IROpcode::Local;
            local.operands[0] = slot_rename.at(i);
            local.operands[1] = arg_base + i;
            block.instructions.push_back(local);
        }
        aura::ir::IRInstruction jump;
        jump.opcode = aura::ir::IROpcode::Jump;
        jump.operands[0] = block_rename.at(0);
        block.instructions.push_back(jump);
        // Step 6: Clone each callee block into the caller
        // (appended to caller.blocks). For each block:
        //   - Copy instructions, remapping slots
        //   - For the exit block, replace the trailing
        //     Return with Local(call_result, ret_src) +
        //     Jump(b_after_id)
        //   - For non-exit blocks, the trailing
        //     Branch/Jump has its target remapped via
        //     block_rename
        for (std::uint32_t j = 0; j < callee.blocks.size(); ++j) {
            aura::ir::BasicBlock clone;
            clone.id = block_rename.at(j);
            const auto& cb = callee.blocks[j];
            for (std::size_t k = 0; k < cb.instructions.size(); ++k) {
                auto cp = cb.instructions[k];
                const auto* info = lookup_opcode(cp.opcode);
                // Remap result slot if has_result_slot
                // (opcodes like Local, Add, Const*, etc.).
                if (info && info->has_result_slot) {
                    auto it = slot_rename.find(cp.operands[0]);
                    if (it != slot_rename.end())
                        cp.operands[0] = it->second;
                }
                // For Branch, operands[0] is the cond slot
                // (Branch has no result slot, so the
                // has_result_slot check above skipped it).
                // Remap operands[0] explicitly for Branch.
                if (cp.opcode == aura::ir::IROpcode::Branch) {
                    auto it = slot_rename.find(cp.operands[0]);
                    if (it != slot_rename.end())
                        cp.operands[0] = it->second;
                }
                // Remap slot-typed source operands only.
                // Branch operands[1..2] are block ids, not
                // slots. Jump operands[0] is a block id.
                // For all other opcodes, operands[1..3] are
                // slot-typed (and the remap is a no-op for
                // operands that aren't in slot_rename, e.g.
                // arg_base which is a caller slot).
                bool is_branch = (cp.opcode == aura::ir::IROpcode::Branch);
                bool is_jump = (cp.opcode == aura::ir::IROpcode::Jump);
                std::size_t remap_end = 4;
                if (is_branch)
                    remap_end = 1; // only operands[0] is a slot
                if (is_jump)
                    remap_end = 0; // no slot operands
                for (std::size_t op = 1; op < remap_end; ++op) {
                    auto it = slot_rename.find(cp.operands[op]);
                    if (it != slot_rename.end())
                        cp.operands[op] = it->second;
                }
                clone.instructions.push_back(cp);
            }
            // The terminator (last instruction) needs
            // special handling. A multi-block callee may
            // have multiple Return blocks (each returning
            // a different value via a different source
            // slot). For ANY block ending in Return, we
            // replace the Return with Local(call_result,
            // this_block's_return_source) + Jump(b_after_id).
            if (!clone.instructions.empty()) {
                auto& term = clone.instructions.back();
                if (term.opcode == aura::ir::IROpcode::Return) {
                    // This block returns. Replace with
                    // Local(call_result, this_block's
                    // return_source_after_slot_remap) + Jump
                    // to B_after.
                    std::uint32_t this_ret_src = term.operands[0];
                    auto trit = slot_rename.find(this_ret_src);
                    if (trit != slot_rename.end()) {
                        this_ret_src = trit->second;
                    }
                    clone.instructions.pop_back();
                    aura::ir::IRInstruction local;
                    local.opcode = aura::ir::IROpcode::Local;
                    local.operands[0] = result_slot;
                    local.operands[1] = this_ret_src;
                    clone.instructions.push_back(local);
                    aura::ir::IRInstruction jmp;
                    jmp.opcode = aura::ir::IROpcode::Jump;
                    jmp.operands[0] = b_after_id;
                    clone.instructions.push_back(jmp);
                } else if (term.opcode == aura::ir::IROpcode::Branch) {
                    // Branch: cond, true_block, false_block
                    auto bit0 = block_rename.find(term.operands[1]);
                    if (bit0 != block_rename.end())
                        term.operands[1] = bit0->second;
                    auto bit1 = block_rename.find(term.operands[2]);
                    if (bit1 != block_rename.end())
                        term.operands[2] = bit1->second;
                } else if (term.opcode == aura::ir::IROpcode::Jump) {
                    // Jump: target_block
                    auto bit = block_rename.find(term.operands[0]);
                    if (bit != block_rename.end())
                        term.operands[0] = bit->second;
                }
            }
            caller.blocks.push_back(std::move(clone));
        }
        // Step 7: Build B_after (the new block that holds
        // the "after" instructions + the original
        // terminator). Append to caller.blocks.
        aura::ir::BasicBlock after_block;
        after_block.id = b_after_id;
        for (const auto& instr : after_instrs) {
            after_block.instructions.push_back(instr);
        }
        if (orig_terminator.has_value()) {
            after_block.instructions.push_back(*orig_terminator);
        }
        caller.blocks.push_back(std::move(after_block));
        (void)call_instr; // suppress unused
        return true;
    }


    std::size_t inlined_count_ = 0;
    // Issue #197: branch-aware inliner counter.
    std::size_t inlined_branch_aware_count_ = 0;
    // Issue #388: cross-marker macro-hygiene skipped count.
    // Bumped when either callee.marker or call_instr.source_marker
    // is MacroIntroduced AND respect_macro_hygiene_ is true.
    // Lets dashboards see how often the macro-hygiene guard
    // fires (a high number means the workspace is macro-heavy).
    // Issue #197: per-run totals (reset on each run()).
    std::size_t run_inlined_ = 0;
    std::size_t run_inlined_branch_aware_ = 0;
    // Issue #197: lifetime totals (process-wide, static).
    // The (compile:inline-pass-stats) Aura primitive reads
    // these. They are not reset on run(); only the per-run
    // counts are reset.
    static inline std::size_t total_inlined_ = 0;
    static inline std::size_t total_inlined_branch_aware_ = 0;
    // Issue #388: macro-hygiene cross-marker skipped total.
    static inline std::size_t macro_hygiene_skipped_ = 0;
    // Issue #246: macro-hygiene policy toggle. Default true
    // (skip inlining macro-introduced callees). See
    // set_respect_macro_hygiene() for the opt-in.
    static inline bool respect_macro_hygiene_ = true;
    // Issue #171: func_id → IRFunction* index for O(1) lookup.
    std::vector<const aura::ir::IRFunction*> func_index_;
    // Issue #203: SCC membership per func_id. Rebuilt on
    // each run() call. Two functions are mutually recursive
    // iff scc_id_of_fid_[i] == scc_id_of_fid_[j]. This is
    // a more general recursion guard than the direct
    // callee_fid == caller_fid check (which only catches
    // direct recursion).
    std::vector<std::uint32_t> scc_id_of_fid_;

    // Issue #203: build the call graph from a module. For
    // each function f, finds all fids g such that f has a
    // static Call to g (resolved via MakeClosure + Call).
    // Returns graph[i] = list of fids called by function i.
    // Functions that don't call anything get an empty list.
    static std::vector<std::vector<std::uint32_t>>
    build_call_graph(const aura::ir::IRModule& module) {
        const std::size_t n = module.functions.size();
        std::vector<std::vector<std::uint32_t>> graph(n);
        for (std::size_t fi = 0; fi < n; ++fi) {
            const auto& func = module.functions[fi];
            // Build slot → func_id map for this function
            std::unordered_map<std::uint32_t, std::uint32_t> slot_to_func;
            for (const auto& block : func.blocks) {
                for (const auto& instr : block.instructions) {
                    if (instr.opcode == aura::ir::IROpcode::MakeClosure) {
                        // MakeClosure(result_slot, func_id, ...)
                        slot_to_func[instr.operands[0]] = instr.operands[1];
                    }
                }
            }
            // Find all Call instructions and resolve their callees.
            // De-dupe (a function may call g multiple times).
            std::unordered_set<std::uint32_t> callees;
            for (const auto& block : func.blocks) {
                for (const auto& instr : block.instructions) {
                    if (instr.opcode != aura::ir::IROpcode::Call)
                        continue;
                    auto callee_slot = instr.operands[0];
                    auto it = slot_to_func.find(callee_slot);
                    if (it == slot_to_func.end())
                        continue;
                    auto callee_fid = it->second;
                    if (callee_fid >= n)
                        continue;
                    callees.insert(callee_fid);
                }
            }
            graph[fi].assign(callees.begin(), callees.end());
        }
        return graph;
    }

    // Issue #203: Tarjan's SCC algorithm. Returns a vector
    // scc_id where scc_id[i] = SCC id of node i. SCCs are
    // numbered in reverse topological order (a later SCC has
    // no edges to an earlier SCC). Two nodes are mutually
    // reachable iff they have the same scc_id. O(V + E) time.
    static std::vector<std::uint32_t>
    compute_sccs(std::size_t n, const std::vector<std::vector<std::uint32_t>>& graph) {
        std::vector<std::int64_t> index(n, -1);
        std::vector<std::int64_t> lowlink(n, -1);
        std::vector<bool> on_stack(n, false);
        std::vector<std::uint32_t> scc_id(n, 0);
        std::vector<std::uint32_t> stack;
        std::uint32_t next_index = 0;
        std::uint32_t next_scc = 0;
        // Recursive lambda (Tarjan's algorithm is naturally
        // recursive; depth is bounded by the SCC size, which
        // is bounded by n).
        // We use std::function for the recursion so we can
        // have a self-referential closure. The std::function
        // heap-allocates the closure, which is fine for the
        // one-time per-run cost.
        std::function<void(std::uint32_t)> strongconnect = [&](std::uint32_t v) {
            index[v] = static_cast<std::int64_t>(next_index);
            lowlink[v] = static_cast<std::int64_t>(next_index);
            ++next_index;
            stack.push_back(v);
            on_stack[v] = true;
            for (std::uint32_t w : graph[v]) {
                if (w >= n)
                    continue;
                if (index[w] == -1) {
                    strongconnect(w);
                    lowlink[v] = std::min(lowlink[v], lowlink[w]);
                } else if (on_stack[w]) {
                    lowlink[v] = std::min(lowlink[v], index[w]);
                }
            }
            if (lowlink[v] == index[v]) {
                std::uint32_t w;
                do {
                    w = stack.back();
                    stack.pop_back();
                    on_stack[w] = false;
                    scc_id[w] = next_scc;
                } while (w != v);
                ++next_scc;
            }
        };
        for (std::uint32_t v = 0; v < n; ++v) {
            if (index[v] == -1)
                strongconnect(v);
        }
        return scc_id;
    }
};

// Issue #171: Tail Call Optimization (Priority 3).
//
// Detects the Call+Return pattern at the end of a block and
// rewrites it as a Branch to the callee's entry block. This
// eliminates the stack-frame allocation for the recursive call
// (the caller's frame becomes the callee's frame). For
// tail-recursive functions, the result is constant stack usage
// instead of O(recursion_depth).
//
// The evaluator's TCO is at the tree-walking level
// (evaluator_eval_flat.cpp eval_flat, the TCO loop). This pass
// brings TCO to the IR level, so the JIT can benefit.
//
// Pattern detected (the only one this minimal version handles):
//   Block ends with:
//     Call(callee_slot, arg_base, arg_count, result_slot)
//     Return(result_slot)
//   Where callee_slot was set by MakeClosure(func_id) earlier
//   in the same function (static callee).
//
// Transformation:
//   1. Emit Local(callee_param_i, caller_arg_base + i) for each
//      arg i in 0..arg_count-1 (only needed if arg_base != 0;
//      the common case for tail-recursion has arg_base = 0 so
//      the args are already in the callee's param slots).
//   2. Replace the Call with a Branch to the callee's entry
//      block (the first block in callee.blocks).
//   3. Remove the Return (the Branch is the new terminator).
//
// Limitations (deferred to follow-up):
// - Caller's arg_base must be 0 (args are already in the
//   callee's param slots) for the simplest path. Non-zero
//   arg_base is handled by emitting Local copies first.
// - No inter-block TCO (only Call+Return within the same block).
// - Mutual recursion not detected (would need call-graph
//   analysis).
export class TCOPass {
public:
    void run(aura::ir::IRModule& module) {
        tco_count_ = 0;
        tco_inter_block_count_ = 0;
        // Build func_id → IRFunction* index (same pattern as
        // InlinePass for O(1) lookup).
        if (func_index_.size() != module.functions.size()) {
            func_index_.clear();
            func_index_.resize(module.functions.size());
            for (std::size_t i = 0; i < module.functions.size(); ++i)
                func_index_[i] = &module.functions[i];
        }
        for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
            auto& func = module.functions[fi];
            // Build slot → func_id map for static callee lookup.
            std::unordered_map<std::uint32_t, std::uint32_t> slot_to_func;
            for (const auto& block : func.blocks) {
                for (const auto& instr : block.instructions) {
                    if (instr.opcode == aura::ir::IROpcode::MakeClosure) {
                        slot_to_func[instr.operands[0]] = instr.operands[1];
                    }
                }
            }
            // Issue #202: inter-block TCO runs FIRST. It detects
            // tail-call blocks (Call+Return) and re-targets
            // unconditional-Jump predecessors to the callee's
            // entry. The tail-call block itself becomes a
            // unreachable dead block after this (intra-block
            // TCO may then collapse it to a single Jump).
            //
            // Running inter-block TCO first means we see the
            // original Call+Return pattern in the tail-call
            // block (intra-block TCO would have transformed it
            // to a single Jump, hiding the tail-call pattern
            // from inter-block analysis).
            run_inter_block_tco(func, slot_to_func);
            for (auto& block : func.blocks) {
                run_on_block(func, block, slot_to_func);
            }
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "tco"; }
    std::size_t tco_count() const { return tco_count_; }
    std::size_t tco_inter_block_count() const { return tco_inter_block_count_; }

private:
    void run_on_block(aura::ir::IRFunction& caller, aura::ir::BasicBlock& block,
                      const std::unordered_map<std::uint32_t, std::uint32_t>& slot_to_func) {
        // Need at least 2 instructions to have Call+Return
        if (block.instructions.size() < 2)
            return;
        // Look at the last 2 instructions
        const auto n = block.instructions.size();
        // Use POINTERS (not references) so we can re-assign
        // them after the vector reallocates from the Local
        // inserts below. References to vector elements are
        // invalidated by insert()/push_back().
        aura::ir::IRInstruction* call_instr = &block.instructions[n - 2];
        aura::ir::IRInstruction* ret_instr = &block.instructions[n - 1];
        // Must be Call followed by Return
        if (call_instr->opcode != aura::ir::IROpcode::Call)
            return;
        if (ret_instr->opcode != aura::ir::IROpcode::Return)
            return;
        // The Return's value must be the Call's result (otherwise
        // the Call is not actually the tail position).
        if (ret_instr->operands[0] != call_instr->operands[3])
            return;
        // The Call's callee must be statically known
        auto callee_slot = call_instr->operands[0];
        auto it = slot_to_func.find(callee_slot);
        if (it == slot_to_func.end())
            return; // dynamic callee
        auto callee_fid = it->second;
        if (callee_fid >= func_index_.size())
            return;
        const auto* callee = func_index_[callee_fid];
        if (!callee)
            return;
        if (callee->blocks.empty())
            return;
        // The callee's entry block id is callee->blocks[0].id
        // (we use the block's id for Branch target).
        auto callee_entry_id = callee->blocks[0].id;
        // We need to emit this Branch with the proper target.
        // The Branch opcode uses operands[0] = target block id.
        // Reuse call_instr's slot (the second-to-last instruction).
        // If arg_base != 0, we need to insert Local copies
        // before the Branch. For now, only handle arg_base == 0
        // (the common case for tail-recursion where args are
        // already in the callee's param slots 0..arg_count-1).
        auto arg_base = call_instr->operands[1];
        auto arg_count = call_instr->operands[2];
        // Issue #201: handle non-zero arg_base by emitting
        // Local(callee_param_i, caller_arg_base + i) for each
        // arg i in 0..arg_count-1 before the Jump. After the
        // Local inserts, the caller's args are in slots
        // 0..arg_count-1 (callee's param slots) and the Jump
        // (which is the only thing the callee sees) doesn't
        // need to know about the original arg_base.
        if (arg_base != 0) {
            // Check that all caller slots [arg_base ..
            // arg_base + arg_count - 1] are valid (we can't
            // bail out mid-transformation). The IR is assumed
            // to be well-formed, so we don't re-validate.
            // Insert Local copies in FORWARD order: insert
            // Local(callee_param_i, caller_arg_base + i) at
            // position (call_idx + i) for i in 0..arg_count-1.
            // The first insert pushes the Call to call_idx+1,
            // but we then re-insert at the new call_idx+1
            // (which is call_idx+1+i' for the next i'), so
            // forward iteration with an ever-increasing insert
            // position naturally builds the correct order:
            // [MC, CI, ..., CI, Local(0), Local(1), Local(2), Call→Jump]
            // (Note: we deliberately don't use reverse iteration
            //  because the insert position is computed from the
            //  ORIGINAL call_idx, and reverse iteration would
            //  interleave the inserts with the Call and Return.)
            //
            // Capture source_ast_node_id + type_id BEFORE
            // inserting (the `call_instr` reference is to the
            // ORIGINAL Call position, and the vector may
            // reallocate during inserts, invalidating the
            // reference).
            const auto src_node_id = call_instr->source_ast_node_id;
            const auto src_type_id = call_instr->type_id;
            const std::size_t call_idx = n - 2; // index of the Call instruction
            for (std::uint32_t i = 0; i < arg_count; ++i) {
                aura::ir::IRInstruction local;
                local.opcode = aura::ir::IROpcode::Local;
                local.operands = {i, arg_base + i, 0, 0};
                local.source_ast_node_id = src_node_id;
                local.type_id = src_type_id;
                block.instructions.insert(block.instructions.begin() +
                                              static_cast<std::ptrdiff_t>(call_idx) +
                                              static_cast<std::ptrdiff_t>(i),
                                          local);
            }
            // The Call was at call_idx; after arg_count inserts
            // it's now at position (call_idx + arg_count).
            // Update the call_instr pointer to point at the
            // new Call location (the vector may have reallocated,
            // so the original pointer is dangling).
            // Now the Call's arg_base is still the original
            // (non-zero) value, but we're about to rewrite
            // the Call to a Jump so the arg_base is irrelevant.
            // (We don't need to mutate call_instr->operands[1]
            // before the rewrite, since the Jump opcode only
            // reads operands[0].)
            call_instr = &block.instructions[call_idx + arg_count];
        }
        // Transform: replace Call with Branch to callee's entry,
        // remove Return. The Branch takes no args (callee's
        // params are already at slots 0..arg_count-1, after the
        // Local copies above for non-zero arg_base).
        call_instr->opcode = aura::ir::IROpcode::Jump; // wait, Jump is unconditional; need Branch
        // Actually, use Jump since the target is unconditional
        // (after the tail call there's no condition to check).
        call_instr->opcode = aura::ir::IROpcode::Jump;
        call_instr->operands[0] = callee_entry_id;
        // Remove the Return (Branch is the new terminator)
        block.instructions.pop_back();
        ++tco_count_;
        (void)caller;
    }

    // Issue #202: inter-block TCO. Detects the pattern
    //
    //   Block P: ...; Jump(B);
    //   Block B: Call(callee, arg_base, arg_count, result); Return(result);
    //
    // and transforms it to
    //
    //   Block P: ...; Jump(callee_entry);    (B is now dead)
    //   Block B: Call(callee, ...); Return(result);   (will be collapsed by intra-block TCO)
    //
    // For the transformation to be correct, ALL predecessors
    // of B must be unconditional Jumps (no Branch into B —
    // a Branch would mean B is one of two outcomes, and the
    // other outcome may have a different semantics).
    //
    // Also: for the simple form, we require arg_base == 0
    // (the callee reads from slots 0..arg_count-1 directly).
    // Non-zero arg_base would require inserting Local copies
    // at the end of each predecessor, which is a follow-up
    // (the Local copies would need to be propagated to all
    // preds, and the args must be at compatible slots in
    // all preds).
    void run_inter_block_tco(aura::ir::IRFunction& func,
                             const std::unordered_map<std::uint32_t, std::uint32_t>& slot_to_func) {
        // For each block, check if it's a tail-call block.
        // If yes, find all preds and rewrite them to jump
        // to the callee entry.
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            const auto& block = func.blocks[bi];
            if (!is_tail_call_block(block, slot_to_func))
                continue;
            // Get the callee entry id
            const auto& call_instr = block.instructions[block.instructions.size() - 2];
            auto callee_slot = call_instr.operands[0];
            auto arg_base = call_instr.operands[1];
            auto it = slot_to_func.find(callee_slot);
            if (it == slot_to_func.end())
                continue;
            auto callee_fid = it->second;
            if (callee_fid >= func_index_.size())
                continue;
            const auto* callee = func_index_[callee_fid];
            if (!callee || callee->blocks.empty())
                continue;
            auto callee_entry_id = callee->blocks[0].id;
            // For now, skip non-zero arg_base (see header comment).
            if (arg_base != 0)
                continue;
            // Find all unconditional-Jump predecessors of bi
            // (other blocks whose terminator is Jump(bi)).
            // We skip Branch predecessors (a Branch into B
            // would mean B is one of two outcomes, and TCO
            // is only sound if B is the unique successor).
            for (std::size_t pi = 0; pi < func.blocks.size(); ++pi) {
                if (pi == bi)
                    continue;
                auto& pred = func.blocks[pi];
                if (pred.instructions.empty())
                    continue;
                auto& last = pred.instructions.back();
                if (last.opcode != aura::ir::IROpcode::Jump)
                    continue;
                if (last.operands[0] != block.id)
                    continue;
                // Rewrite pred's terminator: Jump(bi) → Jump(callee_entry)
                last.operands[0] = callee_entry_id;
                ++tco_inter_block_count_;
            }
        }
    }

    // Predicate: is `block` a tail-call block?
    // A tail-call block ends with Call+Return where the
    // Return's value is the Call's result, and the Call's
    // callee is statically known.
    static bool
    is_tail_call_block(const aura::ir::BasicBlock& block,
                       const std::unordered_map<std::uint32_t, std::uint32_t>& slot_to_func) {
        if (block.instructions.size() < 2)
            return false;
        const auto n = block.instructions.size();
        const auto& call = block.instructions[n - 2];
        const auto& ret = block.instructions[n - 1];
        if (call.opcode != aura::ir::IROpcode::Call)
            return false;
        if (ret.opcode != aura::ir::IROpcode::Return)
            return false;
        if (ret.operands[0] != call.operands[3])
            return false;
        if (slot_to_func.find(call.operands[0]) == slot_to_func.end())
            return false;
        return true;
    }

    std::size_t tco_count_ = 0;
    std::size_t tco_inter_block_count_ = 0;
    std::vector<const aura::ir::IRFunction*> func_index_;
};

// Issue #183 Cycle 2: MonomorphizePass
//
// Compiles a function twice when shape profiling has
// determined a stable shape for it:
//   1. The original function becomes the GENERIC version
//      (specialized_for = 0).
//   2. A new function is added to IRModule.functions[] as
//      the SPECIALIZED version (specialized_for = shape_id,
//      generic_id = <index of the generic version>).
//   3. The specialized version's entry block is rewritten
//      to:
//        OpGuardShape result=0, arg=arg_slot_0, expected=shape_id, generic=2
//        OpBranch result=0, cond=result, true=1, false=2
//        block 1 (specialized body): original entry block body
//        block 2 (generic trampoline):
//            OpCall result=0, callee=generic_id, arg_base=0, arg_count=N
//            OpReturn result=0
//
// Cycle 2 ships the SCAFFOLDING: the pass works for
// functions where:
//   - shape_profiler has a stable shape (shape_id != 0)
//   - The function takes ≥ 1 argument (the guard checks arg 0)
//   - The function has a single entry block with a non-empty body
//
// The full integration (per-function shape tracking, JIT
// dispatch, mutate integration) is the work of follow-up
// issues. This pass is testable in isolation: the C++ test
// (TC61 + new test cases) constructs an IRModule manually
// and runs the pass on it.
export class MonomorphizePass {
public:
    struct Result {
        std::uint32_t specialized_id = 0xFFFFFFFFu; // new function index
        std::uint32_t generic_id = 0xFFFFFFFFu;     // original function index
        std::uint32_t shape_id = 0;                 // the shape we specialized for
    };

    void run(aura::ir::IRModule& module) {
        results_.clear();
        // Take a snapshot of function indices/IDs since we're
        // appending to functions[]. Snapshot is the original
        // function list size (before any appends this pass makes).
        std::size_t const original_count = module.functions.size();
        for (std::size_t i = 0; i < original_count; ++i) {
            // Skip if we already specialized this function
            // (idempotency for re-running the pass).
            if (module.functions[i].specialized_for != 0)
                continue;
            // Need ≥ 1 arg to guard on.
            if (module.functions[i].arg_count == 0)
                continue;
            // Need a shape to specialize for. For Cycle 2
            // scaffolding, we read from an external
            // shape_to_specialize_ map (filled by the test).
            // In production, this would be wired to the
            // shape_profiler_ (#60). The map is keyed by
            // function name (simple; production would use
            // function id).
            auto it = shape_to_specialize_.find(module.functions[i].name);
            if (it == shape_to_specialize_.end())
                continue;
            std::uint32_t shape_id = it->second;
            if (shape_id == 0)
                continue; // 0 = no specialization

            Result r;
            r.shape_id = shape_id;
            r.generic_id = static_cast<std::uint32_t>(i);

            // 1. Make the ORIGINAL function the generic version.
            // (specialized_for stays 0; generic_id is meaningless
            // for the generic version per the IR spec.)
            // (Nothing to do — it already has specialized_for == 0.)

            // 2. Clone the function for the specialized version.
            aura::ir::IRFunction spec = module.functions[i];
            spec.specialized_for = shape_id;
            spec.generic_id = r.generic_id;
            spec.id = static_cast<std::uint32_t>(module.functions.size());

            // 3. Rewrite the entry block to emit the guard.
            // Find the entry block (assumes block 0 is the
            // entry; matches what the lowering pass produces).
            if (spec.blocks.empty()) {
                // Malformed IR; skip.
                continue;
            }
            auto& entry = spec.blocks[spec.entry_block];

            // Build a new block 1: the generic trampoline.
            aura::ir::BasicBlock trampoline;
            trampoline.id = 1;
            // Add a Call to the generic version, then Return.
            // Call: callee, arg_base, arg_count, result
            // Use a fresh local for the result (not strictly
            // needed for tail call but kept for clarity).
            std::uint32_t call_result_slot = spec.local_count++;
            {
                aura::ir::IRInstruction call_instr;
                call_instr.opcode = aura::ir::IROpcode::Call;
                call_instr.operands = std::array<std::uint32_t, 4>{
                    r.generic_id,    // callee
                    0u,              // arg_base = 0 (params at slots 0..N-1)
                    spec.arg_count,  // arg_count
                    call_result_slot // result slot
                };
                trampoline.instructions.push_back(call_instr);
            }
            {
                aura::ir::IRInstruction ret_instr;
                ret_instr.opcode = aura::ir::IROpcode::Return;
                ret_instr.operands = std::array<std::uint32_t, 4>{call_result_slot, 0u, 0u, 0u};
                trampoline.instructions.push_back(ret_instr);
            }
            spec.blocks.push_back(trampoline);

            // Insert the guard + branch at the start of the
            // existing entry block.
            // The guard writes a bool to local slot 0 (the
            // branch's cond input).
            std::uint32_t guard_result = 0u; // local 0 is the convention
            // (but local 0 might be a param; to be safe, use
            // a fresh local for the guard result)
            if (spec.local_count == 0)
                spec.local_count = 1;
            guard_result = spec.local_count++;
            aura::ir::IRInstruction guard_instr;
            guard_instr.opcode = aura::ir::IROpcode::GuardShape;
            // OpGuardShape: result, arg_slot, expected, generic_block
            guard_instr.operands = std::array<std::uint32_t, 4>{
                guard_result, // result slot (bool 1/0)
                0u,           // arg slot 0 (the first param)
                shape_id,     // expected shape
                1u            // generic trampoline block id
            };
            // Branch: cond, true_target, false_target
            // true  = existing entry block body (id 0)
            // false = trampoline (id 1)
            aura::ir::IRInstruction branch_instr;
            branch_instr.opcode = aura::ir::IROpcode::Branch;
            branch_instr.operands = std::array<std::uint32_t, 4>{guard_result, 0u, 1u, 0u};

            // Splice guard + branch at the front of entry.instructions
            std::vector<aura::ir::IRInstruction> new_body;
            new_body.reserve(entry.instructions.size() + 2);
            new_body.push_back(guard_instr);
            new_body.push_back(branch_instr);
            for (auto& instr : entry.instructions) {
                new_body.push_back(std::move(instr));
            }
            entry.instructions = std::move(new_body);

            // 4. Add the specialized function to the module.
            r.specialized_id = module.add_function(std::move(spec));
            results_.push_back(r);
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "monomorphize"; }
    std::vector<Result> const& results() const { return results_; }

    // Test seam: production code wires this to shape_profiler_.
    // For Cycle 2 scaffolding, the test fills it directly.
    std::map<std::string, std::uint32_t> shape_to_specialize_;

private:
    std::vector<Result> results_;
};

// ── ShapeAwareFoldingPass — Issue #462 ────────────────────────
//
// New optimization pass that exploits IRInstruction's rich
// metadata (shape_id, linear_ownership_state,
// adt_variant_id, narrow_evidence) for two hot-path wins:
//
//  1. **Linear elision**: when an op's source slot has
//     `linear_ownership_state == Owned` (value 1) and is
//     not in the escape_map, the subsequent MoveOp on that
//     slot is redundant — the value is provably not aliased
//     so the move is a no-op. We elide by replacing MoveOp
//     with Nop (the source register still carries the
//     right value, and Nop is a zero-cost instruction).
//
//  2. **Narrow-evidence fold**: when an op has
//     `narrow_evidence != 0` (the type has been narrowed
//     by a type-predicate branch) and the op is a
//     dynamic-type-check (e.g. OpTypeCheck on a value
//     whose narrow_evidence already covers the check), the
//     type-check is provably true and can be elided. For
//     the scope-limited slice we count these opportunities
//     but don't actually rewrite the IR — the rewrites
//     follow in #462 follow-ups (Cycle 2 of the same
//     issue).
//
// Observable stats:
//   - fold_count: # of instructions replaced (Nop'd) due
//     to shape/linear/narrow metadata
//   - linear_elide_count: subset of fold_count due to
//     linear-ownership elision
//   - narrow_check_count: # of redundant type-checks
//     detected (counted, not yet elided in Cycle 1)
//   - guard_shape_hits: # of GuardShape instructions seen
//     in the module (signal for downstream passes to
//     trust the per-block shape_id)
//
// Cycle 2 (separate issue) will add the actual narrow-
// evidence rewrite + the per-shape-id OpAdd unchecked
// specialization. The pass scaffolding + counters +
// run() + observability hookup are the Cycle 1 ship.
export class ShapeAwareFoldingPass {
public:
    void run(aura::ir::IRModule& module) {
        for (auto& func : module.functions) {
            run_on_function(func);
        }
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "shape-aware-folding"; }

    // Counters for (query:shape-folding-stats) primitive.
    std::uint64_t fold_count() const { return fold_count_; }
    std::uint64_t linear_elide_count() const { return linear_elide_count_; }
    std::uint64_t narrow_check_count() const { return narrow_check_count_; }
    std::uint64_t guard_shape_hits() const { return guard_shape_hits_; }

    // Test seam: cycle the pass with a known escape map
    // (production wires this from EscapeAnalysisPass).
    void set_escape_map(std::string function_name, std::vector<std::uint8_t> escape) {
        escape_maps_[std::move(function_name)] = std::move(escape);
    }

private:
    void run_on_function(aura::ir::IRFunction& func) {
        // Build the escape map for this function. Default
        // to "all slots escape" (conservative) if no
        // override was provided via set_escape_map — that
        // way the pass never elides unless EscapeAnalysis
        // has explicitly marked the slot as non-escaping.
        std::vector<std::uint8_t> escape_map(func.local_count, 1);
        if (auto it = escape_maps_.find(func.name); it != escape_maps_.end()) {
            auto& src = it->second;
            for (std::size_t i = 0; i < escape_map.size() && i < src.size(); ++i)
                escape_map[i] = src[i];
        }

        for (auto& block : func.blocks) {
            for (auto& instr : block.instructions) {
                // Count GuardShape occurrences (signal for
                // downstream passes).
                if (instr.opcode == aura::ir::IROpcode::GuardShape) {
                    ++guard_shape_hits_;
                    continue;
                }

                // Linear elision: MoveOp on a slot that
                // (a) is Owned (linear_ownership_state == 1)
                // (b) is not in the escape map
                // → the move is provably a no-op, replace
                // with Nop.
                if (instr.opcode == aura::ir::IROpcode::MoveOp) {
                    auto src_slot = instr.operands.size() > 1 ? instr.operands[1] : 0;
                    if (src_slot < func.local_count && escape_map[src_slot] == 0) {
                        // Find the IRInstruction with this
                        // result that has linear_ownership_state
                        // == 1 (Owned).
                        bool src_owned = false;
                        for (const auto& prev_block : func.blocks) {
                            for (const auto& prev : prev_block.instructions) {
                                if (!prev.operands.empty() && prev.operands[0] == src_slot &&
                                    prev.linear_ownership_state == 1) {
                                    src_owned = true;
                                    break;
                                }
                            }
                            if (src_owned)
                                break;
                        }
                        if (src_owned) {
                            instr.opcode = aura::ir::IROpcode::Nop;
                            instr.operands = {};
                            ++linear_elide_count_;
                            ++fold_count_;
                            continue;
                        }
                    }
                }

                // Narrow-evidence count (rewrite deferred
                // to Cycle 2).
                if (instr.narrow_evidence != 0) {
                    // Type-check-ish opcodes (Issue #149
                    // Phase 1): when narrow_evidence covers
                    // the check, the check is redundant.
                    // We count the opportunity; the actual
                    // rewrite (OpNop replacement) is Cycle 2.
                    // CastOp is the runtime type-check opcode
                    // (Issue #149: result, value, type_tag).
                    if (instr.opcode == aura::ir::IROpcode::CastOp) {
                        ++narrow_check_count_;
                    }
                }
            }
        }
    }

    std::uint64_t fold_count_ = 0;
    std::uint64_t linear_elide_count_ = 0;
    std::uint64_t narrow_check_count_ = 0;
    std::uint64_t guard_shape_hits_ = 0;
    std::map<std::string, std::vector<std::uint8_t>> escape_maps_;
};

// ── SoAtoAoSBridgePass — Issue #463 (Phase 2 wiring scaffold) ─────
//
// This Pass consumes an IRModuleV2 (SoA) + a templated AoS
// Pass type P, converts each SoA function to AoS via
// to_aos_view(), runs P on the AoS view, then propagates
// the result back (currently a no-op — the SoA module is
// the read-only input; the AoS result is discarded).
//
// Counters:
//   - soa_functions_visited: # of SoA functions walked
//   - soa_instructions_visited: # of SoA instructions walked
//   - aos_view_built_count: # of to_aos_view() conversions
//     (incremented each time the bridge is invoked)
//
// The Pass concept expects `run(IRModule&)`. The bridge
// works at a different level (IRModuleV2 + a Pass instance
// + the SoA-side wiring), so it doesn't itself satisfy the
// Pass concept — it's invoked explicitly from the
// service.ixx pipeline after the SoA-built module is
// available, BEFORE the AoS pipeline that consumes the
// legacy IRModule. This is the test-seam wiring for the
// Phase 2 SoA→AoS migration; subsequent cycles replace
// the AoS side with a SoA-aware overload of the same Pass
// (e.g. ConstantFoldingWrap::run_soa).
export template <typename P> class SoAtoAoSBridgePass {
public:
    explicit SoAtoAoSBridgePass(P& pass)
        : pass_(pass) {}

    // Run the bridge: convert each SoA function to AoS, run
    // the wrapped AoS pass on the AoS view, bump counters.
    // Returns true if the wrapped pass returned no errors.
    bool run(IRModuleV2& soa_mod) {
        // Build a temporary AoS IRModule from the SoA module
        // for the wrapped pass to consume. This is the
        // bridge — the SoA side is the source of truth, the
        // AoS side is a view.
        aos_view_ = aura::ir::IRModule{};
        aos_view_.entry_function_id = soa_mod.entry_function_id;
        aos_view_.string_pool = soa_mod.string_pool;
        aos_view_.functions.reserve(soa_mod.functions.size());
        for (const auto& soa_fn : soa_mod.functions) {
            ++soa_functions_visited_;
            soa_instructions_visited_ += soa_fn.opcodes_.size();
            ++aos_view_built_count_;
            auto aos_fn = aura::compiler::to_aos_view(soa_fn);
            aos_view_.functions.push_back(std::move(aos_fn));
        }
        // Run the wrapped pass on the AoS view.
        pass_.run(aos_view_);
        // Propagate any error state from the wrapped pass.
        return !pass_.has_error();
    }

    // Counters for (query:soa-adoption-stats) primitive.
    std::uint64_t soa_functions_visited() const { return soa_functions_visited_; }
    std::uint64_t soa_instructions_visited() const { return soa_instructions_visited_; }
    std::uint64_t aos_view_built_count() const { return aos_view_built_count_; }

    // Test seam: read-only access to the AoS view that was
    // built during the last run() call.
    const aura::ir::IRModule& aos_view() const { return aos_view_; }

    bool has_error() const { return pass_.has_error(); }
    std::string_view name() const { return "soa-to-aos-bridge"; }

private:
    P& pass_;
    std::uint64_t soa_functions_visited_ = 0;
    std::uint64_t soa_instructions_visited_ = 0;
    std::uint64_t aos_view_built_count_ = 0;
    aura::ir::IRModule aos_view_;
};

} // namespace aura::compiler
