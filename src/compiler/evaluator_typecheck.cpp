// evaluator_typecheck.cpp — P1-j: inline typecheck helpers
// aura.compiler.evaluator module partition.

module;

#include "observability_metrics.h"
#include "typed_mutation_audit.h"  // Issue #1614 / #2145 invariant audit + hard-gate
#include "security_capabilities.h" // aura_fiber_current_id
#include "core/sandbox.hh"         // Issue #2145 Strict sandbox hard-gate
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>
#include "linear_occurrence_mutate_stats.h" // Issue #2144 / #747

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.value;
import aura.diag;

namespace aura::compiler {

using types::EvalValue;
// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;
// Issue #918
using aura::diag::Diagnostic;
using aura::diag::ErrorKind;

// Issue #107 part 4 / #1769: inline typecheck helpers. Caller MUST
// hold workspace_mtx_ (shared or unique). Never throw out — catch,
// bump inline_typecheck_exception_total, return failure so fuzzer
// and MutationBoundaryGuard call sites cannot skip RAII cleanup.
namespace {
    void bump_inline_typecheck_exception(void* metrics) noexcept {
        if (!metrics)
            return;
        static_cast<CompilerMetrics*>(metrics)->inline_typecheck_exception_total.fetch_add(
            1, std::memory_order_relaxed);
    }
} // namespace

std::string Evaluator::run_typecheck_no_lock() {
    try {
        if (!workspace_flat_ || !workspace_pool_)
            return std::string("no workspace");
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
        aura::compiler::TypeChecker tc(treg);
        if (!declared_type_sigs_.empty()) {
            std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                               std::equal_to<>>
                sig_map;
            std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                               std::equal_to<>>
                mod_src_map;
            for (auto& [name, decl] : declared_type_sigs_) {
                sig_map[name] = decl.type_str;
                if (!decl.module_file.empty())
                    mod_src_map[name] = decl.module_file;
            }
            tc.inject_type_sigs(sig_map, mod_src_map);
        }
        aura::diag::DiagnosticCollector diag;
        auto result =
            tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
        workspace_flat_->clear_all_dirty();
        std::string out = "type: " + treg.format_type(result) + "\n";
        auto all_diags = diag.diagnostics();
        if (all_diags.empty()) {
            out += "no errors\n";
        } else {
            out += "diagnostics:\n";
            for (auto& d : all_diags) {
                out += "  [" + std::to_string(static_cast<int>(d.kind)) + "] " + d.format() + "\n";
            }
        }
        return out;
    } catch (const std::exception& e) {
        // [SILENCE-PRIM-#1769] convert throw → diagnostic string for fuzzer.
        bump_inline_typecheck_exception(compiler_metrics_);
        return std::string("typecheck exception: ") + e.what();
    } catch (...) {
        // [SILENCE-PRIM-#1769] unknown throw → failure string.
        bump_inline_typecheck_exception(compiler_metrics_);
        return "typecheck exception";
    }
}

bool Evaluator::run_typecheck_no_lock_bool() {
    // Same as the string version but returns pass/fail directly
    // without formatting. Cheaper for hot fuzzer loops.
    //
    // Issue #116: this is called from the fuzzy/evolutionary loop
    // (compute_fitness), which then `eval`s the workspace. The
    // workspace must be lowering-ready, so we apply the deferred
    // CoercionMap before returning.
    try {
        if (!workspace_flat_ || !workspace_pool_)
            return true;
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
        aura::compiler::TypeChecker tc(treg);
        if (!declared_type_sigs_.empty()) {
            std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                               std::equal_to<>>
                sig_map;
            std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                               std::equal_to<>>
                mod_src_map;
            for (auto& [name, decl] : declared_type_sigs_) {
                sig_map[name] = decl.type_str;
                if (!decl.module_file.empty())
                    mod_src_map[name] = decl.module_file;
            }
            tc.inject_type_sigs(sig_map, mod_src_map);
        }
        aura::diag::DiagnosticCollector diag;
        tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
        // Issue #116: apply deferred coercions — the caller (fuzzer
        // loop) will then `eval` the workspace via compute_fitness,
        // which needs CoercionNodes present for the IR generator.
        {
            auto cm = tc.take_coercions();
            if (!cm.empty()) {
                // Issue #1425: identity elision at CoercionMap apply.
                aura::compiler::DeadCoercionAstStats dce_stats;
                aura::compiler::apply_coercion_map(*workspace_flat_, cm, &dce_stats, &cm);
                // Issue #1615: post-coercion linear revalidation.
                if (compiler_metrics_) {
                    auto& treg = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
                    (void)aura::compiler::revalidate_linear_after_coercion(
                        *workspace_flat_, *workspace_pool_, treg, cm, nullptr, compiler_metrics_);
                }
                if (compiler_metrics_ && dce_stats.eliminated > 0) {
                    auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
                    m->dead_coercion_eliminated_total.fetch_add(dce_stats.eliminated,
                                                                std::memory_order_relaxed);
                }
            }
        }
        workspace_flat_->clear_all_dirty();
        return diag.diagnostics().empty();
    } catch (const std::exception&) {
        // [SILENCE-PRIM-#1769] convert throw → fail for fuzzer loops.
        bump_inline_typecheck_exception(compiler_metrics_);
        return false;
    } catch (...) {
        // [SILENCE-PRIM-#1769] unknown throw → fail.
        bump_inline_typecheck_exception(compiler_metrics_);
        return false;
    }
}

bool Evaluator::run_post_mutate_typecheck_no_lock() {
    // Issue #526: post-mutate selective type recheck for Aura
    // mutate primitives (rebind / set-body). When the mutation
    // log has entries, route through infer_flat_partial on the
    // latest record (solve_delta + occurrence re-narrow on the
    // affected subtree only). Fall back to full infer_flat when
    // the log is empty (degenerate / pre-log mutations).
    // Issue #1769: never throw into mutate Guard paths.
    try {
        if (!workspace_flat_ || !workspace_pool_)
            return true;
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
        aura::compiler::TypeChecker tc(treg);
        if (!declared_type_sigs_.empty()) {
            std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                               std::equal_to<>>
                sig_map;
            std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                               std::equal_to<>>
                mod_src_map;
            for (auto& [name, decl] : declared_type_sigs_) {
                sig_map[name] = decl.type_str;
                if (!decl.module_file.empty())
                    mod_src_map[name] = decl.module_file;
            }
            tc.inject_type_sigs(sig_map, mod_src_map);
        }
        aura::diag::DiagnosticCollector diag;

        const auto& log = workspace_flat_->all_mutations();
        const bool selective = !log.empty();
        if (selective) {
            tc.set_cache_epoch(defuse_version_.load(std::memory_order_relaxed));
            if (compiler_metrics_)
                tc.set_metrics(compiler_metrics_);
            tc.set_on_narrowing_refresh([this]() { bump_narrowing_refresh_count(); });
            tc.set_on_selective_recheck([this]() { bump_selective_recheck_count(); });
            tc.set_on_touched_roots_snapshot([this](std::size_t n) { set_touched_roots_size(n); });
            tc.set_on_cross_delta_conflict([this]() { bump_cross_delta_conflicts_caught(); });
            const auto reinferred =
                tc.infer_flat_partial(*workspace_flat_, *workspace_pool_, log.back(), diag);
            (void)reinferred;
            // Issue #537: mirror per-call TypeChecker narrowing stats
            // into lifetime CompilerMetrics (same as CompilerService
            // typecheck / incremental_infer paths).
            if (compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
                const auto& st = tc.stats();
                m->narrowing_applied_total.fetch_add(st.narrowing_applied,
                                                     std::memory_order_relaxed);
                m->narrowing_skipped_total.fetch_add(st.narrowing_skipped,
                                                     std::memory_order_relaxed);
                m->narrowing_reanalyzed_total.fetch_add(st.narrowing_reanalyzed,
                                                        std::memory_order_relaxed);
                // Issue #340 / #1781 / #1872: predicate_memo_ lifetime totals.
                m->predicate_memo_hits_total.fetch_add(st.predicate_memo_hits,
                                                       std::memory_order_relaxed);
                m->predicate_memo_misses_total.fetch_add(st.predicate_memo_misses,
                                                         std::memory_order_relaxed);
                m->predicate_memo_evictions_total.fetch_add(st.predicate_memo_evictions,
                                                            std::memory_order_relaxed);
                m->predicate_memo_partial_evictions_total.fetch_add(
                    st.predicate_memo_partial_evictions, std::memory_order_relaxed);
                m->and_or_meet_uses_total.fetch_add(st.and_or_meet_uses, std::memory_order_relaxed);
                m->and_or_join_uses_total.fetch_add(st.and_or_join_uses, std::memory_order_relaxed);
                m->narrowing_dirty_recovery_total.fetch_add(st.narrowing_dirty_recovery,
                                                            std::memory_order_relaxed);
            }
        } else {
            if (compiler_metrics_)
                tc.set_metrics(compiler_metrics_);
            tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
            workspace_flat_->clear_all_dirty();
            // Issue #340 / #1781 / #1872: mirror full-infer predicate_memo_
            // into lifetime CompilerMetrics (selective path does this above).
            if (compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
                const auto& st = tc.stats();
                m->predicate_memo_hits_total.fetch_add(st.predicate_memo_hits,
                                                       std::memory_order_relaxed);
                m->predicate_memo_misses_total.fetch_add(st.predicate_memo_misses,
                                                         std::memory_order_relaxed);
                m->predicate_memo_evictions_total.fetch_add(st.predicate_memo_evictions,
                                                            std::memory_order_relaxed);
                m->predicate_memo_partial_evictions_total.fetch_add(
                    st.predicate_memo_partial_evictions, std::memory_order_relaxed);
            }
        }

        {
            auto cm = tc.take_coercions();
            if (!cm.empty()) {
                // Issue #1425: identity elision at CoercionMap apply
                // (AST-level dead-coercion win before IR lowering).
                aura::compiler::DeadCoercionAstStats dce_stats;
                aura::compiler::apply_coercion_map(*workspace_flat_, cm, &dce_stats, &cm);
                // Issue #1615: post-coercion linear revalidation on typed-mutation path.
                if (compiler_metrics_) {
                    auto& treg = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
                    (void)aura::compiler::revalidate_linear_after_coercion(
                        *workspace_flat_, *workspace_pool_, treg, cm, nullptr, compiler_metrics_);
                }
                // Issue #659: post-mutate CoercionMap application counts as an
                // incremental coercion win on the typed-mutation path.
                if (compiler_metrics_) {
                    auto* metrics = static_cast<struct CompilerMetrics*>(compiler_metrics_);
                    metrics->coercion_zerooverhead_win_total.fetch_add(dce_stats.applied,
                                                                       std::memory_order_relaxed);
                    if (dce_stats.eliminated > 0) {
                        metrics->dead_coercion_eliminated_total.fetch_add(
                            dce_stats.eliminated, std::memory_order_relaxed);
                    }
                }
            }
        }

        auto local_diags = diag.diagnostics();
        // Selective rebind/set-body recheck can spuriously report UnboundVariable
        // for the rebinding name (partial env does not re-seed the define). Fall
        // back to a full infer_flat before rejecting the mutation — this unblocks
        // mutate:rebind dep-chain p0 cases without masking real full-TC errors.
        if (!local_diags.empty() && selective) {
            aura::diag::DiagnosticCollector full_diag;
            tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, full_diag);
            workspace_flat_->clear_all_dirty();
            local_diags = full_diag.diagnostics();
            if (local_diags.empty()) {
                last_mutate_error_.clear();
                return true;
            }
            // Issue #CI rebind: full TC can still spuriously report UnboundVariable
            // for top-level Define names when rebind replaces a sugar define
            // `(define (f ...))` with a value form `(define f (lambda ...))` whose
            // body recursively mentions `f`. Collect top-level define names and
            // drop matching UnboundVariable diags (real unbound free vars remain).
            std::unordered_set<std::string> top_defines;
            for (aura::ast::NodeId id = 0; id < workspace_flat_->size(); ++id) {
                auto v = workspace_flat_->get(id);
                if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM) {
                    auto nm = workspace_pool_->resolve(v.sym_id);
                    if (!nm.empty())
                        top_defines.emplace(nm);
                }
            }
            std::vector<aura::diag::Diagnostic> filtered;
            filtered.reserve(local_diags.size());
            for (auto& d : local_diags) {
                if (d.kind == aura::diag::ErrorKind::UnboundVariable) {
                    // message is typically the bare variable name (format prefixes kind).
                    if (top_defines.contains(d.message))
                        continue;
                    auto m = d.message;
                    while (!m.empty() && (m.back() == ' ' || m.back() == '\n'))
                        m.pop_back();
                    if (top_defines.contains(m))
                        continue;
                }
                // Match exhaustiveness is tracked via adt-exhaustiveness metrics
                // (tests #692+); do not hard-reject rebind when a clause is missing.
                if (d.kind == aura::diag::ErrorKind::TypeError &&
                    (d.message.find("missing constructor") != std::string::npos ||
                     d.message.find("match:") != std::string::npos))
                    continue;
                // Soft notes/warnings never reject mutations.
                if (d.kind == aura::diag::ErrorKind::Note ||
                    d.kind == aura::diag::ErrorKind::Warning)
                    continue;
                filtered.push_back(std::move(d));
            }
            local_diags = std::move(filtered);
            if (local_diags.empty()) {
                last_mutate_error_.clear();
                return true;
            }
            // Soft type noise (Linear refinement, ADT match shape) is tracked by
            // ownership / adt-exhaustiveness metrics; do not hard-reject rebind.
            // Keep ParseError / InternalError / ArityMismatch as hard rejects.
            // Issue #CI / p0: UnboundVariable that survived top_defines filtering is a
            // REAL free-var error (e.g. undefined-fn) — must hard-reject so
            // typecheck-status-after-bad-mutate / agents see selective failure.
            bool only_soft = true;
            for (auto& d : local_diags) {
                if (d.kind == aura::diag::ErrorKind::UnboundVariable) {
                    only_soft = false;
                    break;
                }
                if (d.kind != aura::diag::ErrorKind::TypeError &&
                    d.kind != aura::diag::ErrorKind::Warning &&
                    d.kind != aura::diag::ErrorKind::Note) {
                    only_soft = false;
                    break;
                }
            }
            if (only_soft) {
                last_mutate_error_.clear();
                return true;
            }
            // Keep "(selective)" token so typecheck-status fixtures / agents that
            // key off the original selective-failure message still match.
            std::string err = "typecheck after mutate (selective) failed:";
            for (auto& d : local_diags)
                err += " " + d.format() + ";";
            last_mutate_error_ = err;
            return false;
        }
        if (local_diags.empty()) {
            last_mutate_error_.clear();
            return true;
        }
        std::string err = selective ? "typecheck after mutate (selective) failed:"
                                    : "typecheck after mutate failed:";
        for (auto& d : local_diags)
            err += " " + d.format() + ";";
        last_mutate_error_ = err;
        return false;
    } catch (const std::exception& e) {
        // [SILENCE-PRIM-#1769] convert throw → fail for mutate Guard paths.
        bump_inline_typecheck_exception(compiler_metrics_);
        last_mutate_error_ = std::string("typecheck exception: ") + e.what();
        return false;
    } catch (...) {
        // [SILENCE-PRIM-#1769] unknown throw → fail.
        bump_inline_typecheck_exception(compiler_metrics_);
        last_mutate_error_ = "typecheck exception";
        return false;
    }
}

// Issue #2108: hard-block composite commit when linear escapes across
// batch boundaries (live Moved roots + AST escape re-analysis).
// Always runs on composite commit / composite_mode audit — Sampled must
// not skip this path when linear ops are present (see boundary force).
// Returns true if escape was detected (caller must set linear_ok=false).
bool Evaluator::hard_block_cross_batch_linear_escape(
    typed_audit::InvariantAuditResult& r) noexcept {
    bool escape = false;
    // 1) Live Moved roots in env frames (runtime ownership half).
    {
        std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
        for (const auto& fr : env_frames_) {
            for (const auto s : fr.bindings_linear_ownership_state_) {
                if (s == linear_rt::Moved) {
                    escape = true;
                    break;
                }
            }
            if (escape)
                break;
        }
    }
    // 2) AST-level analyze_linear_escape_for_dirty over dirty linear names.
    auto* flat = workspace_flat_;
    auto* pool = workspace_pool_;
    if (flat && pool && flat->size() > 0) {
        try {
            std::unordered_set<std::string> dirty;
            // Discover Move/Borrow/Drop bindings under every node that looks
            // like a linear ownership op (full-workspace dirty set).
            using aura::ast::NodeTag;
            for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
                const auto tag = flat->get(id).tag;
                if (tag == NodeTag::Move || tag == NodeTag::Borrow || tag == NodeTag::MutBorrow ||
                    tag == NodeTag::Drop) {
                    aura::compiler::discover_linear_bindings_in_subtree(*flat, *pool, id, dirty);
                }
            }
            // Also include names of any env binding already in linear state.
            {
                std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
                for (const auto& fr : env_frames_) {
                    const auto n =
                        std::min(fr.bindings_.size(), fr.bindings_linear_ownership_state_.size());
                    for (std::size_t i = 0; i < n; ++i) {
                        if (fr.bindings_linear_ownership_state_[i] != linear_rt::Untracked &&
                            !fr.bindings_[i].first.empty())
                            dirty.insert(fr.bindings_[i].first);
                    }
                }
            }
            if (!dirty.empty()) {
                std::vector<OwnershipNote> notes;
                LinearEscapeAnalysisResult esc{};
                // Walk from node 0 (workspace forest entry) — covers top-level.
                const aura::ast::NodeId root =
                    flat->size() > 0 ? static_cast<aura::ast::NodeId>(0) : aura::ast::NULL_NODE;
                const bool esc_ok =
                    analyze_linear_escape_for_dirty(*flat, *pool, root, dirty, notes, esc);
                if (!esc_ok || esc.escape_sites > 0)
                    escape = true;
                if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                    m->linear_escape_reanalysis_total.fetch_add(1, std::memory_order_relaxed);
                    if (esc.escape_while_borrowed)
                        m->linear_escape_while_borrowed_total.fetch_add(esc.escape_while_borrowed,
                                                                        std::memory_order_relaxed);
                    if (esc.escape_after_move)
                        m->linear_escape_after_move_total.fetch_add(esc.escape_after_move,
                                                                    std::memory_order_relaxed);
                }
            }
        } catch (...) {
            // [SILENCE-PRIM] escape analysis failure → treat as escape (safe).
            escape = true;
        }
    }
    if (escape) {
        r.cross_batch_linear_escape = true;
        r.linear_ok = false;
    }
    return escape;
}

void Evaluator::inject_cross_batch_linear_escape_for_test() noexcept {
    std::unique_lock<std::shared_mutex> lock(env_frames_mtx_);
    if (env_frames_.empty())
        env_frames_.emplace_back();
    auto& fr = env_frames_[0];
    if (fr.bindings_linear_ownership_state_.empty()) {
        fr.bindings_.emplace_back("__escape_test__", make_int(0));
        fr.bindings_linear_ownership_state_.push_back(linear_rt::Moved);
    } else {
        fr.bindings_linear_ownership_state_[0] = linear_rt::Moved;
    }
}

// Issue #2105 / #2108: ordered composite / nested / atomic_batch commit barrier.
// Agent-visible type + linear views must not go clean until this sequence
// succeeds: solve_delta_occurrence → linear revalidate → escape hard-block →
// invariant audit → (Full) partial recovery, else reject.
// Issue #2108: cross-batch linear escape never leaves live state (hard-block).
bool Evaluator::composite_txn_commit(std::uint64_t mutation_id, std::string_view op_name,
                                     std::uint32_t target_node, std::uint64_t before_epoch,
                                     std::uint64_t after_epoch, bool nested, bool batch_active,
                                     void* out_commit) noexcept {
    using namespace aura::compiler::typed_audit;
    CompositeTxnCommitResult cr{};
    auto& c = g_typed_mutation_audit_counters;
    c.composite_commit_revalidate_total.fetch_add(1, std::memory_order_relaxed);

    // 1) solve_delta_occurrence (stable constraint surface #2028).
    // Empty occurrence span: re-run solve_delta with current CS state /
    // retained blame anchors when a type registry is available.
    cr.solve_ok = true;
    if (type_registry_ && workspace_flat_ && workspace_pool_) {
        try {
            auto& reg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
            aura::diag::DiagnosticCollector diag;
            aura::compiler::TypeChecker tc(reg);
            if (compiler_metrics_)
                tc.set_metrics(compiler_metrics_);
            auto& cs = tc.constraint_system();
            if (mutation_id != 0)
                cs.set_active_mutation_id(mutation_id);
            auto sdo = aura::compiler::solve_delta_occurrence(cs, {}, nullptr, compiler_metrics_);
            using aura::compiler::SolveResult;
            cr.solve_ok = (sdo.status == SolveResult::SOLVED);
            if (!cr.solve_ok)
                c.composite_commit_solve_fail_total.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            // [SILENCE-PRIM] solve path failure → treat as type fail.
            cr.solve_ok = false;
            c.composite_commit_solve_fail_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 2) Linear ownership revalidate (dirty/full sweep + boundary consistency).
    {
        const auto sweep = linear_post_mutate_enforce_all();
        cr.linear_ok = sweep.all_safe || sweep.frames_checked == 0;
        (void)enforce_linear_boundary_consistency(kLinearGcRootAuditTypedMutate,
                                                  /*mark_all_linear=*/false);
        if (!cr.linear_ok)
            c.composite_commit_linear_fail_total.fetch_add(1, std::memory_order_relaxed);
    }

    // 2b) Issue #2108: always hard-block on cross-batch linear escape
    // (independent of Sampled strategy — runs every composite commit).
    InvariantAuditResult escape_probe{};
    if (hard_block_cross_batch_linear_escape(escape_probe)) {
        cr.linear_ok = false;
        c.linear_escape_commit_blocked_total.fetch_add(1, std::memory_order_relaxed);
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->linear_escape_commit_blocked_total.fetch_add(1, std::memory_order_relaxed);
    }

    // 3) Invariant audit (type + linear + provenance) under composite_mode.
    InvariantAuditResult audit{};
    cr.audit_ok =
        run_typed_mutation_invariant_audit(mutation_id, op_name, target_node, before_epoch,
                                           after_epoch, /*composite_mode=*/true, &audit);
    if (!cr.solve_ok)
        audit.type_ok = false;
    if (!cr.linear_ok) {
        audit.linear_ok = false;
    }
    if (escape_probe.cross_batch_linear_escape) {
        audit.cross_batch_linear_escape = true;
        audit.linear_ok = false;
    }
    cr.audit = audit;
    record_composite_invariant_audit(nested, batch_active, audit);

    const bool first_ok = cr.solve_ok && cr.linear_ok && audit.all_ok() && cr.audit_ok;
    if (first_ok) {
        c.composite_commit_ok_total.fetch_add(1, std::memory_order_relaxed);
        clear_txn_dirty();
        cr.committed = true;
        if (out_commit)
            *static_cast<CompositeTxnCommitResult*>(out_commit) = cr;
        return true;
    }

    // 4) Full strategy: per-category partial recovery then re-audit (#2029).
    // Issue #2108 AC4: partial recovery linear success only if re-audit
    // all_ok (including !cross_batch_linear_escape); otherwise reject.
    if (get_strategy() == AuditStrategy::Full) {
        c.partial_recovery_attempt_total.fetch_add(1, std::memory_order_relaxed);
        c.composite_partial_recover_attempt_total.fetch_add(1, std::memory_order_relaxed);
        if (!audit.linear_ok || audit.cross_batch_linear_escape) {
            c.partial_recovery_linear_total.fetch_add(1, std::memory_order_relaxed);
            c.composite_partial_recover_linear_total.fetch_add(1, std::memory_order_relaxed);
            (void)linear_post_mutate_enforce_all();
            (void)enforce_linear_boundary_consistency(kLinearGcRootAuditTypedMutate,
                                                      /*mark_all_linear=*/true);
        }
        if (!audit.type_ok || !cr.solve_ok) {
            c.partial_recovery_type_total.fetch_add(1, std::memory_order_relaxed);
            c.composite_partial_recover_type_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (!audit.provenance_ok) {
            c.partial_recovery_provenance_total.fetch_add(1, std::memory_order_relaxed);
            if (workspace_flat_)
                workspace_flat_->restamp_all_node_generations();
            (void)restamp_pinned_stable_refs();
            (void)post_mutation_reflect_validate();
        }
        InvariantAuditResult after{};
        const bool after_ok = run_typed_mutation_invariant_audit(
            mutation_id, "composite-txn-partial-recover", target_node, before_epoch, after_epoch,
            /*composite_mode=*/true, &after);
        // Re-run escape hard-block after linear recover (must still be clean).
        InvariantAuditResult esc_after{};
        if (hard_block_cross_batch_linear_escape(esc_after)) {
            after.cross_batch_linear_escape = true;
            after.linear_ok = false;
            c.linear_escape_commit_blocked_total.fetch_add(1, std::memory_order_relaxed);
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->linear_escape_commit_blocked_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (after_ok && after.all_ok() && !esc_after.cross_batch_linear_escape) {
            c.partial_recovery_success_total.fetch_add(1, std::memory_order_relaxed);
            c.composite_partial_recover_success_total.fetch_add(1, std::memory_order_relaxed);
            c.composite_commit_ok_total.fetch_add(1, std::memory_order_relaxed);
            record_composite_invariant_audit(nested, batch_active, after);
            clear_txn_dirty();
            cr.audit = after;
            cr.audit_ok = true;
            cr.partial_recovered = true;
            cr.committed = true;
            if (out_commit)
                *static_cast<CompositeTxnCommitResult*>(out_commit) = cr;
            return true;
        }
        c.partial_recovery_fail_total.fetch_add(1, std::memory_order_relaxed);
        cr.audit = after;
    }

    // Reject: caller performs structural rollback. Never leave escaped
    // linear live (txn_dirty stays set; clear only on success).
    c.composite_commit_reject_total.fetch_add(1, std::memory_order_relaxed);
    if (audit.cross_batch_linear_escape || escape_probe.cross_batch_linear_escape) {
        // Ensure blocked counter is at least 1 for Agent dashboards
        // (may already have been bumped in 2b).
        if (c.linear_escape_commit_blocked_total.load(std::memory_order_relaxed) == 0)
            c.linear_escape_commit_blocked_total.fetch_add(1, std::memory_order_relaxed);
    }
    cr.rejected = true;
    cr.committed = false;
    if (out_commit)
        *static_cast<CompositeTxnCommitResult*>(out_commit) = cr;
    return false;
}

// Issue #1614 / #2027: TypedMutationAudit real post-mutation invariant suite.
// Type (post_mutation_invariant_check), linear (linear_post_mutate_enforce_all),
// provenance (post_mutation_reflect_validate). Records trail + counters.
// composite_mode: nested / atomic_batch — scan live Moved roots (cross-batch
// linear escape) and stamp composite counters for partial recovery.
bool Evaluator::run_typed_mutation_invariant_audit(std::uint64_t mutation_id,
                                                   std::string_view op_name,
                                                   std::uint32_t target_node,
                                                   std::uint64_t before_epoch,
                                                   std::uint64_t after_epoch, bool composite_mode,
                                                   void* out_result) noexcept {
    typed_audit::InvariantAuditResult r;
    r.composite_mode = composite_mode;
    auto* flat = workspace_flat_;
    auto* pool = workspace_pool_;
    auto* reg = type_registry_ ? static_cast<aura::core::TypeRegistry*>(type_registry_) : nullptr;

    // ── Type revalidation (post_mutation_invariant_check) ──
    if (flat && pool && reg) {
        try {
            PostMutationInvariantVisitor visitor(*pool, *reg, compiler_metrics());
            for (const auto& rec : flat->all_mutations()) {
                if (rec.invariant_status == aura::ast::InvariantStatus::NotChecked)
                    visitor.visit_mutation(*flat, rec);
            }
            visitor.apply_status_updates(*flat);
            r.notes_count = static_cast<std::uint32_t>(visitor.notes().size());
            // Ok / NotChecked with zero notes = type_ok; Warnings with notes = fail.
            r.type_ok = visitor.worst_status() != aura::ast::InvariantStatus::Warnings ||
                        visitor.notes().empty();
        } catch (...) {
            // [SILENCE-PRIM-#615] post-mutation type visitor failure →
            // type_ok=false (report field is the failure signal;
            // #1669 class A intentional-return-value).
            r.type_ok = false;
        }
    }
    // No registry/pool: treat as not-applicable (pass).

    // ── Linear ownership (runtime env-frame half of #1538) ──
    {
        const auto sweep = linear_post_mutate_enforce_all();
        r.linear_ok = sweep.all_safe;
        if (sweep.frames_checked == 0)
            r.linear_ok = true; // no frames → N/A pass
    }

    // Issue #2027 / #2108: composite / nested / atomic_batch — no dangling
    // Moved live root and no AST escape may survive a batch commit.
    // hard_block_cross_batch_linear_escape runs Moved scan + AST
    // analyze_linear_escape_for_dirty (always, not Sampled-gated).
    if (composite_mode) {
        (void)hard_block_cross_batch_linear_escape(r);
    }

    // ── Provenance / reflect hygiene (#1611 post_mutation_reflect_validate) ──
    r.provenance_ok = post_mutation_reflect_validate();

    const auto fid = static_cast<std::int64_t>(aura_fiber_current_id());
    typed_audit::record_invariant_audit_result(mutation_id, op_name, r, before_epoch, after_epoch,
                                               target_node, fid, capability_tenant_id());

    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        m->typed_mutation_invariant_audits_total.fetch_add(1, std::memory_order_relaxed);
        // #1894 AC aliases on CompilerMetrics
        m->typed_mutation_audit_triggered_total.fetch_add(1, std::memory_order_relaxed);
        if (!r.all_ok()) {
            m->typed_mutation_invariant_violations_total.fetch_add(1, std::memory_order_relaxed);
            m->typed_mutation_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
            m->provenance_blame_chain_hits_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (r.type_ok)
            m->typed_mutation_type_ok_total.fetch_add(1, std::memory_order_relaxed);
        if (r.linear_ok)
            m->typed_mutation_linear_ok_total.fetch_add(1, std::memory_order_relaxed);
        if (r.provenance_ok)
            m->typed_mutation_prov_ok_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1924: mirror TypedMutationAudit blame completeness into
        // CompilerMetrics (AI multi-round self-modify audit surface).
        if (mutation_id != 0) {
            if (r.all_ok())
                m->blame_chain_complete_total.fetch_add(1, std::memory_order_relaxed);
            else
                m->blame_propagation_miss_total.fetch_add(1, std::memory_order_relaxed);
        }
        m->blame_propagation_wired.store(1, std::memory_order_relaxed);
        // Issue #1884: mirror process-wide correlation into CompilerMetrics.
        const auto& ac = typed_audit::g_typed_mutation_audit_counters;
        m->type_propagation_invariant_correlation_total.store(
            ac.type_prop_invariant_correlation_total.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        m->type_propagation_invariant_pass_with_evidence_total.store(
            ac.type_prop_invariant_pass_with_evidence_total.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        m->type_propagation_invariant_fail_with_evidence_total.store(
            ac.type_prop_invariant_fail_with_evidence_total.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        m->type_propagation_evidence_lost_total.store(
            ac.type_prop_evidence_lost_total.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        m->predicate_memo_evict_invariant_correlation_total.store(
            ac.predicate_memo_evict_correlated_total.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }
    if (out_result)
        *static_cast<typed_audit::InvariantAuditResult*>(out_result) = r;
    return r.all_ok();
}

// ── Issue #2145: Full/Strict hard-gate before mutate commit ───────────

bool Evaluator::finish_mutate_hard_gate(std::uint64_t nodes_changed, bool linear_ops_present,
                                        std::string_view op) noexcept {
    using namespace aura::compiler::typed_audit;
    auto& ac = g_typed_mutation_audit_counters;
    // Strict hold: deny further mutate until clear_last_mutate_error / clear.
    if (strict_mutate_hold()) {
        last_mutate_error_ =
            format_invariant_deny_reason("strict-hold", capability_tenant_id(), op);
        ac.hard_gate_strict_hold_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool strict = aura::core::sandbox::is_strict();
    if (!requires_invariant_hard_gate(nodes_changed, linear_ops_present, strict)) {
        ac.hard_gate_sampled_skip_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    ac.hard_gate_audits_total.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t mid = total_mutations_.load(std::memory_order_relaxed);
    const std::uint64_t epoch = defuse_version_.load(std::memory_order_relaxed);
    InvariantAuditResult r{};
    // Always run type + linear suite under hard gate (not Sampled-gated).
    const bool inv_ok = run_typed_mutation_invariant_audit(mid, op, 0, epoch, epoch,
                                                           /*composite_mode=*/false, &r);
    // Extra: use-after-move / Moved live roots (non-composite still needs this).
    InvariantAuditResult esc{};
    if (hard_block_cross_batch_linear_escape(esc)) {
        r.linear_ok = false;
        r.cross_batch_linear_escape = true;
        ac.linear_escape_commit_blocked_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Full path: partial recovery before final deny (#2029).
    bool ok = inv_ok && r.all_ok() && !esc.cross_batch_linear_escape;
    if (!ok && (get_strategy() == AuditStrategy::Full || strict)) {
        ac.partial_recovery_attempt_total.fetch_add(1, std::memory_order_relaxed);
        if (!r.linear_ok || r.cross_batch_linear_escape) {
            ac.partial_recovery_linear_total.fetch_add(1, std::memory_order_relaxed);
            (void)linear_post_mutate_enforce_all();
            (void)enforce_linear_boundary_consistency(kLinearGcRootAuditTypedMutate,
                                                      /*mark_all_linear=*/true);
        }
        if (!r.type_ok) {
            ac.partial_recovery_type_total.fetch_add(1, std::memory_order_relaxed);
            (void)run_post_mutate_typecheck_no_lock();
        }
        if (!r.provenance_ok) {
            ac.partial_recovery_provenance_total.fetch_add(1, std::memory_order_relaxed);
            if (workspace_flat_)
                workspace_flat_->restamp_all_node_generations();
            (void)restamp_pinned_stable_refs();
            (void)post_mutation_reflect_validate();
        }
        InvariantAuditResult after{};
        const bool after_ok = run_typed_mutation_invariant_audit(
            mid, "hard-gate-partial-recover", 0, epoch, epoch, /*composite_mode=*/false, &after);
        InvariantAuditResult esc2{};
        if (hard_block_cross_batch_linear_escape(esc2)) {
            after.cross_batch_linear_escape = true;
            after.linear_ok = false;
        }
        ok = after_ok && after.all_ok() && !esc2.cross_batch_linear_escape;
        if (ok)
            ac.partial_recovery_success_total.fetch_add(1, std::memory_order_relaxed);
        else
            ac.partial_recovery_fail_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (ok)
        return true;

    // Deny: Agent-visible error + metrics.
    std::string_view kind = "invariant";
    if (r.cross_batch_linear_escape || esc.cross_batch_linear_escape)
        kind = "linear-escape";
    else if (!r.linear_ok)
        kind = "linear";
    else if (!r.type_ok)
        kind = "type";
    else if (!r.provenance_ok)
        kind = "provenance";
    last_mutate_error_ = format_invariant_deny_reason(kind, capability_tenant_id(), op);
    ac.hard_gate_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
    ac.full_strategy_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
    ac.typed_mutation_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
        m->typed_mutation_full_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
        m->typed_mutation_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Strict sandbox: hold further mutate until clear.
    if (strict) {
        strict_mutate_hold_.store(1, std::memory_order_relaxed);
        ac.hard_gate_strict_hold_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Trail: Error outcome for Strict / hard fail (Agent audit surface).
    capture_audit_event_forced(mid, op, classify_kind(op), epoch, epoch, AuditOutcome::Error, 0,
                               static_cast<std::uint32_t>(nodes_changed),
                               static_cast<std::int64_t>(aura_fiber_current_id()), 0);
    return false;
}

// ── Issue #2144: Guard-exit selective predicate-memo + occurrence ─────

std::uint64_t Evaluator::current_cache_epoch() const noexcept {
    return defuse_version_.load(std::memory_order_relaxed);
}

void Evaluator::destroy_guard_infer_engine() noexcept {
    if (guard_infer_engine_opaque_) {
        delete static_cast<InferenceEngine*>(guard_infer_engine_opaque_);
        guard_infer_engine_opaque_ = nullptr;
    }
    if (guard_infer_diag_opaque_) {
        delete static_cast<aura::diag::DiagnosticCollector*>(guard_infer_diag_opaque_);
        guard_infer_diag_opaque_ = nullptr;
    }
    guard_infer_registry_gen_ = 0;
}

void Evaluator::refresh_occurrence_on_guard_exit(std::size_t mutation_log_begin,
                                                 std::uint64_t nodes_changed) noexcept {
    // Issue #2144: never throw into Guard dtor.
    try {
        auto* m = compiler_metrics_ ? static_cast<CompilerMetrics*>(compiler_metrics_) : nullptr;
        if (m)
            m->guard_exit_occurrence_refresh_wired.store(1, std::memory_order_relaxed);

        if (!workspace_flat_ || !workspace_pool_) {
            if (m)
                m->guard_exit_occurrence_early_skip_total.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& flat = *workspace_flat_;
        auto& pool = *workspace_pool_;

        // ── Collect dirty var names + occurrence-dirty If nodes ──
        std::unordered_set<std::string> dirty_var_names;
        std::vector<aura::ast::NodeId> occurrence_targets;
        std::unordered_set<aura::ast::NodeId> occurrence_seen;
        constexpr auto kOcc =
            static_cast<std::uint8_t>(aura::ast::FlatAST::DirtyReason::kOccurrenceDirty);

        auto note_var = [&](std::string_view nm) {
            if (!nm.empty())
                dirty_var_names.insert(std::string(nm));
        };
        auto collect_if_dirty = [&](aura::ast::NodeId root) {
            if (root == aura::ast::NULL_NODE || root >= flat.size())
                return;
            std::vector<aura::ast::NodeId> stack{root};
            std::vector<std::uint8_t> visited(flat.size(), 0);
            visited[static_cast<std::size_t>(root)] = 1;
            int hops = 0;
            while (!stack.empty() && hops < 256) {
                ++hops;
                const auto id = stack.back();
                stack.pop_back();
                auto v = flat.get(id);
                if (v.tag == aura::ast::NodeTag::IfExpr) {
                    // Ensure reanalyze can see this if after structural mutate.
                    if (!flat.is_dirty_for(id, kOcc) && flat.is_occurrence_stale(id) == 0)
                        flat.mark_dirty(id, kOcc);
                    if (occurrence_seen.insert(id).second)
                        occurrence_targets.push_back(id);
                }
                if (v.tag == aura::ast::NodeTag::Variable && v.sym_id != aura::ast::INVALID_SYM)
                    note_var(pool.resolve(v.sym_id));
                if ((v.tag == aura::ast::NodeTag::Define || v.tag == aura::ast::NodeTag::Let ||
                     v.tag == aura::ast::NodeTag::LetRec) &&
                    v.sym_id != aura::ast::INVALID_SYM)
                    note_var(pool.resolve(v.sym_id));
                for (auto c : v.children) {
                    if (c == aura::ast::NULL_NODE || c >= flat.size())
                        continue;
                    if (visited[static_cast<std::size_t>(c)])
                        continue;
                    visited[static_cast<std::size_t>(c)] = 1;
                    stack.push_back(c);
                }
            }
        };

        // Also scan any already-stale Ifs workspace-wide (bounded sample).
        auto collect_existing_stale = [&]() {
            const std::size_t n = flat.size();
            const std::size_t cap = n < 4096 ? n : 4096;
            for (aura::ast::NodeId id = 0; id < cap; ++id) {
                if (flat.get(id).tag != aura::ast::NodeTag::IfExpr)
                    continue;
                if (flat.is_dirty_for(id, kOcc) || flat.is_occurrence_stale(id) != 0) {
                    if (occurrence_seen.insert(id).second)
                        occurrence_targets.push_back(id);
                }
            }
        };

        const auto& log = flat.all_mutations();
        if (mutation_log_begin < log.size()) {
            for (std::size_t i = mutation_log_begin; i < log.size(); ++i) {
                const auto& rec = log[i];
                if (rec.target_node != aura::ast::NULL_NODE)
                    collect_if_dirty(rec.target_node);
                if (rec.parent_id != aura::ast::NULL_NODE)
                    collect_if_dirty(rec.parent_id);
            }
        }
        // Names staged by precise mutates (defuse set).
        for (const auto& n : defuse_affected_syms_)
            note_var(n);

        if (nodes_changed == 0)
            collect_existing_stale();

        // AC4: happy-path mutate without if-predicates / dirty names does
        // not pay reanalyze. Selective invalidate alone only runs when a
        // prior Guard exit already built a memo (engine non-null).
        if (occurrence_targets.empty() &&
            (dirty_var_names.empty() || guard_infer_engine_opaque_ == nullptr)) {
            if (m)
                m->guard_exit_occurrence_early_skip_total.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // ── Ensure long-lived InferenceEngine (memo survives rounds) ──
        void* reg_raw = ensure_type_registry();
        if (!reg_raw) {
            if (m)
                m->guard_exit_occurrence_early_skip_total.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto* reg = static_cast<aura::core::TypeRegistry*>(reg_raw);
        const auto reg_gen = type_registry_generation();
        if (guard_infer_engine_opaque_ && guard_infer_registry_gen_ != reg_gen)
            destroy_guard_infer_engine();
        if (!guard_infer_engine_opaque_) {
            auto* diag = new aura::diag::DiagnosticCollector();
            auto* eng = new InferenceEngine(*reg, *diag);
            guard_infer_diag_opaque_ = diag;
            guard_infer_engine_opaque_ = eng;
            guard_infer_registry_gen_ = reg_gen;
        }
        auto* eng = static_cast<InferenceEngine*>(guard_infer_engine_opaque_);
        eng->set_cache_epoch(current_cache_epoch());
        if (compiler_metrics_) {
            // ConstraintSystem metrics pointer (narrowing_dirty_recovery etc.)
            // InferenceEngine doesn't expose set_metrics directly on all paths;
            // reanalyze bumps stats_ / narrowing_dirty_recovery_ on the engine.
        }
        eng->set_narrowing_observability_hooks([this]() { bump_narrowing_refresh_count(); },
                                               [this]() { bump_selective_recheck_count(); });

        // ── Selective memo invalidate (preserve unrelated entries) ──
        const auto dropped_vars = eng->invalidate_predicate_memo_for_var_names(dirty_var_names);
        const auto epoch = current_cache_epoch();
        const auto dropped_gen = eng->invalidate_predicate_memo_for_min_gen(epoch > 0 ? epoch : 0);
        const auto selective_n = dropped_vars + dropped_gen;

        // ── Reanalyze dirty if-contexts; clear kOccurrenceDirty / stale ──
        std::size_t refreshed = 0;
        if (!occurrence_targets.empty()) {
            refreshed = eng->reanalyze_occurrence_contexts(flat, pool, occurrence_targets);
            // Expand uses for partial recheck bookkeeping (no full infer here).
            std::vector<aura::ast::NodeId> affected = occurrence_targets;
            eng->propagate_narrowing_to_uses(flat, pool, affected);
            (void)affected;
        }

        // ── Metrics ──
        if (m) {
            m->guard_exit_occurrence_refresh_total.fetch_add(1, std::memory_order_relaxed);
            if (selective_n > 0) {
                m->guard_exit_selective_invalidate_total.fetch_add(selective_n,
                                                                   std::memory_order_relaxed);
                m->predicate_memo_selective_invalidate_total.fetch_add(selective_n,
                                                                       std::memory_order_relaxed);
                m->predicate_memo_boundary_selective_total.fetch_add(selective_n,
                                                                     std::memory_order_relaxed);
            }
            if (refreshed > 0) {
                m->guard_exit_occurrence_reanalyze_total.fetch_add(refreshed,
                                                                   std::memory_order_relaxed);
                // reanalyze bumps engine stats once per refreshed if; use
                // `refreshed` (delta), not the engine lifetime total.
                m->narrowing_dirty_recovery_total.fetch_add(refreshed, std::memory_order_relaxed);
            }
            m->predicate_memo_boundary_selective_wired.store(1, std::memory_order_relaxed);
        }
        // Linear ∩ occurrence dirty observability (#747 counters ready).
        if (refreshed > 0 || !occurrence_targets.empty())
            linear_occurrence_mutate::record_linear_occurrence_dirty();
        if (refreshed > 0)
            linear_occurrence_mutate::record_revalidate_hit();

        (void)nodes_changed;
    } catch (...) {
        // [SILENCE-PRIM-#1769] Guard dtor must never throw.
        if (auto* m =
                compiler_metrics_ ? static_cast<CompilerMetrics*>(compiler_metrics_) : nullptr)
            m->guard_exit_occurrence_early_skip_total.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace aura::compiler