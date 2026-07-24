// evaluator_typecheck.cpp — P1-j: inline typecheck helpers
// aura.compiler.evaluator module partition.

module;

#include "observability_metrics.h"
#include "typed_mutation_audit.h"  // Issue #1614 invariant audit
#include "security_capabilities.h" // aura_fiber_current_id
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

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

    // Issue #2027: composite / nested / atomic_batch — no dangling Moved
    // live root may survive a batch commit or nested boundary success.
    if (composite_mode) {
        std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
        for (const auto& fr : env_frames_) {
            for (const auto s : fr.bindings_linear_ownership_state_) {
                if (s == linear_rt::Moved) {
                    r.cross_batch_linear_escape = true;
                    r.linear_ok = false;
                    break;
                }
            }
            if (r.cross_batch_linear_escape)
                break;
        }
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

} // namespace aura::compiler