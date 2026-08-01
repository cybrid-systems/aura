// evaluator_typecheck.cpp — P1-j: inline typecheck helpers
// aura.compiler.evaluator module partition.

module;

#include "observability_metrics.h"
#include "typed_mutation_audit.h"        // Issue #1614 / #2145 invariant audit + hard-gate
#include "security_capabilities.h"       // aura_fiber_current_id
#include "mutate_type_gate.hh"           // Issue #2219 Soft/Hard post-mutate type gate
#include "coercion_provenance_policy.hh" // Issue #2221 require_blame_complete_on_commit
#include "core/sandbox.hh"               // Issue #2145 Strict sandbox hard-gate
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>
#include "linear_occurrence_mutate_stats.h" // Issue #2144 / #747
#include "ownership_escape_lowering_gate.h" // Issue #2309: aura_escape_move_gate_clear + rollback counter

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
        // Issue #2220: prefer long-lived TypeChecker.
        TypeChecker* tc_ptr = static_cast<TypeChecker*>(ensure_typechecker());
        std::unique_ptr<TypeChecker> stack_tc;
        if (!tc_ptr) {
            stack_tc = std::make_unique<TypeChecker>(treg);
            tc_ptr = stack_tc.get();
            if (compiler_metrics_)
                tc_ptr->set_metrics(compiler_metrics_);
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
                tc_ptr->inject_type_sigs(sig_map, mod_src_map);
            }
        }
        auto& tc = *tc_ptr;
        aura::diag::DiagnosticCollector diag;
        auto result =
            tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
        workspace_flat_->clear_all_dirty();
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typecheck_persistent_cs_cache_hits.store(tc.stats().cs_cache_hits,
                                                        std::memory_order_relaxed);
        }
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
        // Issue #2220: prefer long-lived TypeChecker.
        TypeChecker* tc_ptr = static_cast<TypeChecker*>(ensure_typechecker());
        std::unique_ptr<TypeChecker> stack_tc;
        if (!tc_ptr) {
            stack_tc = std::make_unique<TypeChecker>(treg);
            tc_ptr = stack_tc.get();
            if (compiler_metrics_)
                tc_ptr->set_metrics(compiler_metrics_);
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
                tc_ptr->inject_type_sigs(sig_map, mod_src_map);
            }
        }
        auto& tc = *tc_ptr;
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
                    auto& treg2 = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
                    (void)aura::compiler::revalidate_linear_after_coercion(
                        *workspace_flat_, *workspace_pool_, treg2, cm, nullptr, compiler_metrics_);
                }
                if (compiler_metrics_ && dce_stats.eliminated > 0) {
                    auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
                    m->dead_coercion_eliminated_total.fetch_add(dce_stats.eliminated,
                                                                std::memory_order_relaxed);
                }
            }
        }
        workspace_flat_->clear_all_dirty();
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->typecheck_persistent_cs_cache_hits.store(tc.stats().cs_cache_hits,
                                                        std::memory_order_relaxed);
        }
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
        // Issue #2219: every post-mutate typecheck entry (for query/tests).
        mutate_type_gate::g_gate_check_total.fetch_add(1, std::memory_order_relaxed);
        if (compiler_metrics_) {
            auto* gm = static_cast<struct CompilerMetrics*>(compiler_metrics_);
            gm->mutate_type_gate_check_total.fetch_add(1, std::memory_order_relaxed);
            gm->mutate_type_gate_mode.store(mutate_type_gate::is_hard() ? 1 : 0,
                                            std::memory_order_relaxed);
        }
        // Issue #2279: production lock check on every post-mutate TC entry.
        // Bumps soft_in_production_alarm_total when production_locked && Soft;
        // optionally aborts under AURA_HARD_TYPE_GATE_ABORT=1. Idempotent —
        // forensically the alarm counter is a cumulative count of mutates
        // under misconfiguration (more useful than a one-shot boolean).
        mutate_type_gate::check_soft_in_production_or_abort();
        if (!workspace_flat_ || !workspace_pool_)
            return true;
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
        // Issue #2220: long-lived TypeChecker — reuses solve_delta_cs_ / cs_cache_
        // across multi-round Agent mutate (not a cold start each call).
        TypeChecker* tc_ptr = static_cast<TypeChecker*>(ensure_typechecker());
        std::unique_ptr<TypeChecker> stack_tc;
        if (!tc_ptr) {
            stack_tc = std::make_unique<TypeChecker>(treg);
            tc_ptr = stack_tc.get();
            if (compiler_metrics_)
                tc_ptr->set_metrics(compiler_metrics_);
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
                tc_ptr->inject_type_sigs(sig_map, mod_src_map);
            }
        }
        auto& tc = *tc_ptr;
        // Issue #2219: Hard rejects match exhaustiveness diags (Warning or
        // TypeError). Do not force set_strict(true) at construction — apply
        // only on full recheck path below.
        const bool hard_gate = mutate_type_gate::is_hard();
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
            // Issue #2180/#2220: stash partial CS for composite_txn_commit; with
            // persistent TC the same solve_delta_cs_ is reused next call.
            stash_partial_constraint_state(static_cast<void*>(&tc));
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
                m->typecheck_persistent_cs_cache_hits.store(st.cs_cache_hits,
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
                m->typecheck_persistent_cs_cache_hits.store(st.cs_cache_hits,
                                                            std::memory_order_relaxed);
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

        std::vector<aura::diag::Diagnostic> local_diags;
        {
            auto span = diag.diagnostics();
            local_diags.assign(span.begin(), span.end());
        }
        // Selective rebind/set-body recheck can spuriously report UnboundVariable
        // for the rebinding name (partial env does not re-seed the define). Fall
        // back to a full infer_flat before rejecting the mutation — this unblocks
        // mutate:rebind dep-chain p0 cases without masking real full-TC errors.
        // Issue #2219 Hard: always full TC so match exhaustiveness / TypeError
        // are visible (selective path may report empty diags for incomplete match).
        if (selective && (hard_gate || !local_diags.empty())) {
            // Hard: promote exhaustiveness to TypeError (strict match reporting).
            if (hard_gate)
                tc.set_strict(true);
            aura::diag::DiagnosticCollector full_diag;
            tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, full_diag);
            workspace_flat_->clear_all_dirty();
            {
                auto span = full_diag.diagnostics();
                local_diags.assign(span.begin(), span.end());
            }
            // Issue #2219 Hard: explicit match exhaustiveness walk — selective
            // rebind can leave match sites without TypeError/Warning diags even
            // when a constructor clause was removed.
            if (hard_gate) {
                auto& treg_ex = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
                for (aura::ast::NodeId id = 0; id < workspace_flat_->size(); ++id) {
                    if (!workspace_flat_->has_match_info(id))
                        continue;
                    auto exh =
                        check_match_exhaustiveness(*workspace_flat_, *workspace_pool_, treg_ex, id);
                    if (!exh.checked || exh.exhaustive || exh.missing_constructors.empty())
                        continue;
                    auto msg = format_match_exhaustiveness_message(exh);
                    if (msg.empty())
                        msg = "match: missing constructor";
                    local_diags.push_back(
                        aura::diag::Diagnostic(aura::diag::ErrorKind::TypeError, msg));
                }
            }
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
            auto* gate_m = compiler_metrics_
                               ? static_cast<struct CompilerMetrics*>(compiler_metrics_)
                               : nullptr;
            mutate_type_gate::g_gate_check_total.fetch_add(1, std::memory_order_relaxed);
            if (gate_m) {
                gate_m->mutate_type_gate_check_total.fetch_add(1, std::memory_order_relaxed);
                gate_m->mutate_type_gate_mode.store(hard_gate ? 1 : 0, std::memory_order_relaxed);
            }
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
                // Issue #2219: match exhaustiveness — Soft skip (legacy), Hard reject.
                const bool match_exh = mutate_type_gate::is_match_exhaustiveness_msg(d.message);
                if (match_exh && (d.kind == aura::diag::ErrorKind::TypeError ||
                                  d.kind == aura::diag::ErrorKind::Warning)) {
                    if (hard_gate) {
                        mutate_type_gate::g_exhaustiveness_reject_total.fetch_add(
                            1, std::memory_order_relaxed);
                        if (gate_m)
                            gate_m->mutate_type_gate_exhaustiveness_reject_total.fetch_add(
                                1, std::memory_order_relaxed);
                        filtered.push_back(std::move(d));
                    } else {
                        mutate_type_gate::g_soft_type_skip_total.fetch_add(
                            1, std::memory_order_relaxed);
                        if (gate_m)
                            gate_m->mutate_soft_type_skip_total.fetch_add(
                                1, std::memory_order_relaxed);
                    }
                    continue;
                }
                // Soft notes/warnings never reject mutations (AC4: Hard keeps this).
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
            // ownership / adt-exhaustiveness metrics; Soft gate does not hard-reject.
            // Keep ParseError / InternalError / ArityMismatch as hard rejects.
            // Issue #CI / p0: UnboundVariable that survived top_defines filtering is a
            // REAL free-var error (e.g. undefined-fn) — must hard-reject so
            // typecheck-status-after-bad-mutate / agents see selective failure.
            // Issue #2219 Hard: any remaining TypeError rejects (only_soft=false).
            bool only_soft = true;
            for (auto& d : local_diags) {
                if (d.kind == aura::diag::ErrorKind::UnboundVariable) {
                    only_soft = false;
                    break;
                }
                if (hard_gate && d.kind == aura::diag::ErrorKind::TypeError) {
                    only_soft = false;
                    mutate_type_gate::g_hard_type_error_reject_total.fetch_add(
                        1, std::memory_order_relaxed);
                    if (gate_m)
                        gate_m->mutate_type_gate_hard_type_error_reject_total.fetch_add(
                            1, std::memory_order_relaxed);
                    break;
                }
                if (hard_gate && mutate_type_gate::is_match_exhaustiveness_msg(d.message)) {
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
                // Soft path: remaining pure TypeError / Warning / Note soft-pass.
                if (!hard_gate) {
                    mutate_type_gate::g_soft_type_skip_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                    if (gate_m)
                        gate_m->mutate_soft_type_skip_total.fetch_add(1, std::memory_order_relaxed);
                }
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

// Issue #2264: force non-exhaustive ADT result on next invariant audit.
void Evaluator::inject_adt_non_exhaustive_for_test() noexcept {
    inject_adt_non_exhaustive_.store(1, std::memory_order_relaxed);
}
void Evaluator::clear_adt_non_exhaustive_inject_for_test() noexcept {
    inject_adt_non_exhaustive_.store(0, std::memory_order_relaxed);
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

    // 1) solve_delta_occurrence (stable constraint surface #2028 / #2180).
    // Prefer stashed post-infer_flat_partial CS + occurrence vars — never
    // rely on a greenfield empty TypeChecker for production composite commit.
    cr.solve_ok = true;
    cr.blame_ok = true;
    bool sdo_provenance_continuity = true;
    bool sdo_blame_complete = true;
    bool sdo_blame_nonvacuous = false;
    bool partial_cs_hard_empty = false; // Issue #2262
    if (type_registry_ || commit_type_checker_opaque_) {
        try {
            ConstraintSystem* cs_ptr = nullptr;
            std::span<const aura::core::TypeId> occ_span{};
            TypeChecker* scratch_tc = nullptr;
            const bool reuse = commit_type_checker_opaque_ && commit_cs_live_;
            if (reuse) {
                auto* ctc = static_cast<TypeChecker*>(commit_type_checker_opaque_);
                cs_ptr = &ctc->constraint_system();
                if (commit_occurrence_vars_opaque_) {
                    auto* occ = static_cast<std::vector<aura::core::TypeId>*>(
                        commit_occurrence_vars_opaque_);
                    occ_span = *occ;
                }
                c.composite_commit_solve_reuse_hit_total.fetch_add(1, std::memory_order_relaxed);
                // Issue #2262: reuse with empty work is a hard miss under Full
                // (silent SOLVED on empty CS is forbidden when partial expected).
                if (!ctc->commit_cs_has_work() && txn_dirty()) {
                    partial_cs_hard_empty = true;
                    g_partial_cs_hard_empty_miss_total.fetch_add(1, std::memory_order_relaxed);
                    c.composite_commit_solve_empty_cs_total.fetch_add(1, std::memory_order_relaxed);
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                        m->partial_cs_hard_empty_miss_total.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (type_registry_) {
                // Greenfield only when no partial was stashed (tests / no typecheck).
                auto& reg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
                scratch_tc = new TypeChecker(reg);
                if (compiler_metrics_)
                    scratch_tc->set_metrics(compiler_metrics_);
                cs_ptr = &scratch_tc->constraint_system();
                c.composite_commit_solve_empty_cs_total.fetch_add(1, std::memory_order_relaxed);
                // Issue #2262: expected partial (txn dirty) but empty CS → hard miss.
                if (txn_dirty()) {
                    partial_cs_hard_empty = true;
                    g_partial_cs_hard_empty_miss_total.fetch_add(1, std::memory_order_relaxed);
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                        m->partial_cs_hard_empty_miss_total.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (cs_ptr) {
                if (mutation_id != 0)
                    cs_ptr->set_active_mutation_id(mutation_id);
                std::vector<Constraint> unresolved;
                auto sdo = aura::compiler::solve_delta_occurrence(*cs_ptr, occ_span, &unresolved,
                                                                  compiler_metrics_);
                using aura::compiler::SolveResult;
                // Issue #2260: SOLVED alone is insufficient when reverify was
                // truncated — partial type-proof must not commit under hard-gate.
                c.boundary_solve_hard_gate_total.fetch_add(1, std::memory_order_relaxed);
                if (sdo.truncated_reverify) {
                    c.boundary_solve_truncated_seen_total.fetch_add(1, std::memory_order_relaxed);
                }
                // Issue #2277 AC1: under production defaults a delta TIMEOUT is
                // escalated to a one-shot full fixpoint (Option A). If still not
                // SOLVED, solve_ok stays false here — never half-solved ship.
                auto post_escalate = cs_ptr->escalate_if_production(sdo.status, &unresolved);
                // Base: SOLVED after optional TIMEOUT escalate.
                // Soft (#2458 AC1): truncated may still commit (observe only).
                // Hard: require !truncated unless gate full-solve recovers.
                const bool trunc_hard = aura::compiler::typed_audit::truncate_commit_hard_enabled();
                cr.solve_ok = (post_escalate == SolveResult::SOLVED) &&
                              (!sdo.truncated_reverify || !trunc_hard);
                // Issue #2458: anti half-green — truncated reverify or incomplete
                // non-vacuous blame under production/Full/HARD → full solve or reject.
                // Soft Sampled: observe only (AC1); happy path skips full solve (AC4).
                {
                    SolveDeltaOccurrenceResult sdo_gate = sdo;
                    sdo_gate.status = post_escalate;
                    sdo_gate.truncated_reverify =
                        sdo.truncated_reverify || cs_ptr->last_reverify_truncated();
                    auto gate = aura::compiler::commit_ok_after_delta_snapshot(*cs_ptr, &sdo_gate,
                                                                               &unresolved);
                    if (gate.rejected) {
                        cr.solve_ok = false;
                    } else if (gate.recovered) {
                        cr.solve_ok = true;
                        sdo.truncated_reverify = false;
                    } else if (gate.observed && post_escalate == SolveResult::SOLVED) {
                        // Soft: allow SOLVED even when truncated (AC1).
                        cr.solve_ok = true;
                    }
                }
                // Issue #2262 / #2345: expected-partial + empty CS anti false-green.
                // Production defaults / Full / AURA_COMPOSITE_EMPTY_CS_HARD=1 →
                // hard-reject (solve_ok=false). Dev Sampled soft → observe only
                // (commit may succeed). True vacuous batches (no txn_dirty /
                // expected_partial=false) never set partial_cs_hard_empty.
                // Strict sandbox also hard-rejects (production-like soundness).
                if (partial_cs_hard_empty) {
                    const bool hard = composite_empty_cs_hard_reject_enabled() ||
                                      aura::core::sandbox::is_strict();
                    if (hard) {
                        cr.solve_ok = false;
                        c.composite_commit_empty_cs_hard_miss_total.fetch_add(
                            1, std::memory_order_relaxed);
                        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                            m->composite_commit_empty_cs_hard_miss_total.fetch_add(
                                1, std::memory_order_relaxed);
                    } else {
                        c.composite_commit_empty_cs_observe_total.fetch_add(
                            1, std::memory_order_relaxed);
                        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                            m->composite_commit_empty_cs_observe_total.fetch_add(
                                1, std::memory_order_relaxed);
                    }
                }
                // Issue #2284: compute blame_complete before the publish site
                // so we can capture it on the repair surface.
                const auto& blame = cs_ptr->last_blame_chain();
                const bool blame_complete = blame.is_complete();
                if (!cr.solve_ok) {
                    c.composite_commit_solve_fail_total.fetch_add(1, std::memory_order_relaxed);
                    c.boundary_solve_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
                    // Issue #2284: publish the timeout repair surface so Agents
                    // can self-repair without parsing free-form diagnostics.
                    // (Captures sdo.unresolved + unresolved_affected_nodes +
                    // truncated_reverify + blame_complete.) CompiledMetrics*
                    // fetched from this->compiler_metrics_ (member).
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                        m->type_repair_last_timeout_status.store(
                            static_cast<std::uint64_t>(sdo.status), std::memory_order_relaxed);
                        m->type_repair_last_unresolved_count.store(sdo.unresolved.size(),
                                                                   std::memory_order_relaxed);
                        const std::size_t cap = std::min(sdo.unresolved_affected_nodes.size(),
                                                         static_cast<std::size_t>(16));
                        m->type_repair_last_unresolved_aff_nodes_count.store(
                            cap, std::memory_order_relaxed);
                        for (std::size_t i = 0; i < cap; ++i) {
                            m->type_repair_last_unresolved_aff_nodes[i].store(
                                sdo.unresolved_affected_nodes[i], std::memory_order_relaxed);
                        }
                        m->type_repair_last_truncated_reverify.store(
                            sdo.truncated_reverify ? 1u : 0u, std::memory_order_relaxed);
                        m->type_repair_last_blame_complete.store(blame_complete ? 1u : 0u,
                                                                 std::memory_order_relaxed);
                        m->type_repair_publish_total.fetch_add(1, std::memory_order_relaxed);
                        // Issue #2343: re-mirror unresolved graph onto the durable
                        // repair surface at publish time (production escalate /
                        // soft TIMEOUT both land here when !cr.solve_ok).
                        {
                            using aura::compiler::kUnresolvedGraphEdgeQueryCap;
                            using aura::compiler::kUnresolvedGraphSuggestedRootsCap;
                            const std::size_t ec = sdo.unresolved_graph_edges.size();
                            const std::size_t rc = sdo.suggested_roots.size();
                            m->type_repair_unresolved_edge_count.store(ec,
                                                                       std::memory_order_relaxed);
                            m->type_repair_suggested_root_count.store(rc,
                                                                      std::memory_order_relaxed);
                            for (std::size_t i = 0; i < kUnresolvedGraphSuggestedRootsCap; ++i) {
                                m->type_repair_suggested_roots[i].store(
                                    i < rc ? sdo.suggested_roots[i] : 0u,
                                    std::memory_order_relaxed);
                            }
                            for (std::size_t i = 0; i < kUnresolvedGraphEdgeQueryCap; ++i) {
                                if (i < ec) {
                                    const auto& e = sdo.unresolved_graph_edges[i];
                                    m->type_repair_edge_var[i].store(e.var_rep,
                                                                     std::memory_order_relaxed);
                                    m->type_repair_edge_cix[i].store(e.constraint_index,
                                                                     std::memory_order_relaxed);
                                    m->type_repair_edge_kind[i].store(e.kind,
                                                                      std::memory_order_relaxed);
                                    m->type_repair_edge_lhs[i].store(e.lhs.index,
                                                                     std::memory_order_relaxed);
                                    m->type_repair_edge_rhs[i].store(e.rhs.index,
                                                                     std::memory_order_relaxed);
                                } else {
                                    m->type_repair_edge_var[i].store(0, std::memory_order_relaxed);
                                    m->type_repair_edge_cix[i].store(0, std::memory_order_relaxed);
                                    m->type_repair_edge_kind[i].store(0, std::memory_order_relaxed);
                                    m->type_repair_edge_lhs[i].store(0, std::memory_order_relaxed);
                                    m->type_repair_edge_rhs[i].store(0, std::memory_order_relaxed);
                                }
                            }
                        }
                    }
                }
                // Issue #2221: blame-complete surface after solve_delta_occurrence.
                // Vacuous empty greenfield (no frames, no roots) is exempt so
                // test/no-typecheck commits stay green; non-vacuous incomplete
                // chains are observe-counted and hard-reject under require-on.
                sdo_provenance_continuity = sdo.provenance_continuity;
                sdo_blame_complete = blame_complete;
                sdo_blame_nonvacuous = reuse || !blame.frames.empty() || blame.complete ||
                                       blame.partial || sdo.touched_roots > 0 ||
                                       sdo.occurrence_priority_roots > 0 || sdo.let_poly_roots > 0;
                if (sdo_blame_nonvacuous) {
                    c.blame_commit_check_total.fetch_add(1, std::memory_order_relaxed);
                    g_blame_commit_check_total.fetch_add(1, std::memory_order_relaxed);
                    if (!sdo_blame_complete) {
                        c.blame_commit_incomplete_observe_total.fetch_add(
                            1, std::memory_order_relaxed);
                        g_blame_commit_incomplete_observe_total.fetch_add(
                            1, std::memory_order_relaxed);
                        if (require_blame_complete_on_commit()) {
                            cr.blame_ok = false;
                            c.blame_commit_reject_total.fetch_add(1, std::memory_order_relaxed);
                            g_blame_commit_reject_total.fetch_add(1, std::memory_order_relaxed);
                            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                                m->blame_commit_reject_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                        }
                    } else if (!sdo_provenance_continuity && require_blame_complete_on_commit()) {
                        // Continuity miss with complete-looking chain still fails hard.
                        cr.blame_ok = false;
                        c.blame_commit_reject_total.fetch_add(1, std::memory_order_relaxed);
                        g_blame_commit_reject_total.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            delete scratch_tc;
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
    // Issue #2221: incomplete blame under require-on forces provenance fail
    // so Agents never ship unattributable composite commits.
    if (!cr.blame_ok)
        audit.provenance_ok = false;
    cr.audit = audit;
    record_composite_invariant_audit(nested, batch_active, audit);

    const bool first_ok =
        cr.solve_ok && cr.linear_ok && cr.blame_ok && audit.all_ok() && cr.audit_ok;
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
            // Issue #2180: re-run solve_delta_occurrence on stashed CS.
            // Partial type recovery must not paper over CONFLICT/TIMEOUT.
            if (commit_type_checker_opaque_ && commit_cs_live_) {
                try {
                    auto* ctc = static_cast<TypeChecker*>(commit_type_checker_opaque_);
                    auto& cs = ctc->constraint_system();
                    if (mutation_id != 0)
                        cs.set_active_mutation_id(mutation_id);
                    std::span<const aura::core::TypeId> occ_span{};
                    if (commit_occurrence_vars_opaque_) {
                        auto* occ = static_cast<std::vector<aura::core::TypeId>*>(
                            commit_occurrence_vars_opaque_);
                        occ_span = *occ;
                    }
                    auto sdo2 = aura::compiler::solve_delta_occurrence(cs, occ_span, nullptr,
                                                                       compiler_metrics_);
                    using aura::compiler::SolveResult;
                    // Issue #2260: reject truncated reverify on recovery re-solve.
                    if (sdo2.truncated_reverify)
                        c.boundary_solve_truncated_seen_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                    // Issue #2277 AC1: same production-default escalation on the
                    // partial-recovery re-solve path. If escalate returns
                    // TIMEOUT, solve_ok remains false (no half-solved ship).
                    auto post_escalate2 = cs.escalate_if_production(sdo2.status, nullptr);
                    const bool trunc_hard2 =
                        aura::compiler::typed_audit::truncate_commit_hard_enabled();
                    cr.solve_ok = (post_escalate2 == SolveResult::SOLVED) &&
                                  (!sdo2.truncated_reverify || !trunc_hard2);
                    // Issue #2458: re-apply truncate-commit gate on recovery path
                    // so Full partial recovery cannot half-green after a soft
                    // SOLVED+truncated residual.
                    {
                        SolveDeltaOccurrenceResult sdo2_gate = sdo2;
                        sdo2_gate.status = post_escalate2;
                        sdo2_gate.truncated_reverify =
                            sdo2.truncated_reverify || cs.last_reverify_truncated();
                        auto gate2 =
                            aura::compiler::commit_ok_after_delta_snapshot(cs, &sdo2_gate, nullptr);
                        if (gate2.rejected)
                            cr.solve_ok = false;
                        else if (gate2.recovered)
                            cr.solve_ok = true;
                        else if (gate2.observed && post_escalate2 == SolveResult::SOLVED)
                            cr.solve_ok = true;
                    }
                    if (!cr.solve_ok)
                        c.composite_commit_solve_fail_total.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    // [SILENCE-PRIM]
                    cr.solve_ok = false;
                }
            }
        }
        if (!audit.provenance_ok) {
            c.partial_recovery_provenance_total.fetch_add(1, std::memory_order_relaxed);
            if (workspace_flat_)
                workspace_flat_->restamp_all_node_generations();
            (void)restamp_pinned_stable_refs();
            (void)post_mutation_reflect_validate();
        }
        // Issue #2223: ADT renarrow / revalidate before re-audit.
        if (!audit.adt_ok) {
            c.partial_recovery_adt_total.fetch_add(1, std::memory_order_relaxed);
            partial_recover_adt_exhaustiveness(mutation_id);
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
        // Issue #2180: require solve_ok after re-solve — type CONFLICT must reject.
        // Issue #2221: incomplete blame under require-on must not ship via recovery
        // (restamp does not synthesize a complete DeltaBlameChain).
        if (!cr.blame_ok)
            after.provenance_ok = false;
        if (after_ok && after.all_ok() && !esc_after.cross_batch_linear_escape && cr.solve_ok &&
            cr.blame_ok) {
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
            // Issue #2286: pass cache_epoch to the visitor so the OwnershipEscapeSummary
            // publish key (metrics, cache_epoch) matches what CompilerService uses
            // for the thread-local lookup before lower_to_ir.
            PostMutationInvariantVisitor visitor(*pool, *reg, compiler_metrics(),
                                                 current_cache_epoch());
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

    // ── Issue #2223 / #2264: ADT match exhaustiveness in workspace match sites ──
    // Soft post-mutate TC may filter "missing constructor" TypeErrors;
    // the invariant suite still requires exhaustive matches under Full
    // so composite / hard-gate paths cannot ship non-exhaustive ADT self-mod.
    // Soft/Sampled without hard-gate does not force-rollback solely for ADT
    // (finish_mutate_hard_gate / boundary only deny when hard_gate).
    r.adt_ok = true;
    r.adt_match_sites_present = false;
    r.adt_sites_checked = 0;
    r.adt_non_exhaustive = 0;
    // Issue #2264 test seam: inject non-exhaustive before real walk.
    if (inject_adt_non_exhaustive_.exchange(0, std::memory_order_relaxed) != 0) {
        r.adt_ok = false;
        r.adt_match_sites_present = true;
        r.adt_sites_checked = 1;
        r.adt_non_exhaustive = 1;
    } else if (flat && pool && reg) {
        try {
            const auto n = flat->size();
            for (aura::ast::NodeId id = 0; id < n; ++id) {
                if (!flat->has_match_info(id))
                    continue;
                r.adt_match_sites_present = true;
                const auto exh = check_match_exhaustiveness(*flat, *pool, *reg, id);
                if (!exh.checked)
                    continue;
                ++r.adt_sites_checked;
                if (!exh.exhaustive) {
                    ++r.adt_non_exhaustive;
                    r.adt_ok = false;
                }
            }
        } catch (...) {
            // [SILENCE-PRIM] ADT walk failure → adt_ok=false (fail-closed).
            r.adt_ok = false;
        }
    }

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
        if (r.adt_ok)
            m->typed_mutation_adt_ok_total.fetch_add(1, std::memory_order_relaxed);
        else
            m->typed_mutation_adt_fail_total.fetch_add(1, std::memory_order_relaxed);
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

// ── Issue #2260: MutationBoundary type-proof (SOLVED / !truncated) ───────
//
// Extends composite commit's SOLVED requirement (#2180) to every hard-gate
// boundary exit. Soft/Sampled small dirty may continue with truncated_seen
// metrics only — never leave Full/Strict with partial type-proof live.

bool Evaluator::boundary_solve_proof_gate(bool hard_gate, bool linear_ops_present,
                                          std::uint64_t nodes_changed, bool* out_truncated,
                                          bool* out_force_fail) noexcept {
    using namespace aura::compiler::typed_audit;
    using aura::compiler::SolveResult;
    auto& ac = g_typed_mutation_audit_counters;
    if (out_truncated)
        *out_truncated = false;
    if (out_force_fail)
        *out_force_fail = false;

    if (hard_gate)
        ac.boundary_solve_hard_gate_total.fetch_add(1, std::memory_order_relaxed);

    // No type system / no workspace → proof N/A (treat as ok).
    if (!workspace_flat_ || !type_registry_)
        return true;

    auto run_sdo =
        [&](ConstraintSystem& cs,
            std::span<const aura::core::TypeId> occ) -> aura::compiler::SolveDeltaOccurrenceResult {
        try {
            return aura::compiler::solve_delta_occurrence(cs, occ, nullptr, compiler_metrics_);
        } catch (...) {
            // [SILENCE-PRIM] solve failure → TIMEOUT-shaped fail.
            aura::compiler::SolveDeltaOccurrenceResult bad{};
            bad.status = SolveResult::TIMEOUT;
            bad.truncated_reverify = true;
            return bad;
        }
    };

    ConstraintSystem* cs_ptr = nullptr;
    std::span<const aura::core::TypeId> occ_span{};
    if (commit_type_checker_opaque_ && commit_cs_live_) {
        auto* ctc = static_cast<TypeChecker*>(commit_type_checker_opaque_);
        cs_ptr = &ctc->constraint_system();
        if (commit_occurrence_vars_opaque_) {
            auto* occ =
                static_cast<std::vector<aura::core::TypeId>*>(commit_occurrence_vars_opaque_);
            occ_span = *occ;
        }
    }

    bool solved = true;
    bool truncated = false;
    bool has_unresolved_nodes = false;

    if (cs_ptr) {
        auto sdo = run_sdo(*cs_ptr, occ_span);
        solved = (sdo.status == SolveResult::SOLVED);
        truncated = sdo.truncated_reverify;
        has_unresolved_nodes = !sdo.unresolved_affected_nodes.empty() || !sdo.unresolved.empty();
        if (truncated) {
            ac.boundary_solve_truncated_seen_total.fetch_add(1, std::memory_order_relaxed);
            if (out_truncated)
                *out_truncated = true;
        }
        // Issue #2458: Soft observe / Hard full-solve-or-reject on truncated
        // or incomplete blame (residual half-green after #2260 soft continue).
        {
            SolveDeltaOccurrenceResult sdo_gate{};
            sdo_gate.status = sdo.status;
            sdo_gate.truncated_reverify = truncated;
            sdo_gate.unresolved = sdo.unresolved;
            auto gate = aura::compiler::commit_ok_after_delta_snapshot(*cs_ptr, &sdo_gate, nullptr);
            if (gate.observed && !hard_gate)
                return true; // Soft AC1
            if (gate.recovered) {
                if (out_truncated)
                    *out_truncated = false;
                return true; // Hard recovered via full solve
            }
            if (gate.rejected) {
                // Fall through to hard-gate force-fail / soft still continues below.
                if (!hard_gate)
                    return true; // Soft never force-fails here
                solved = false;
                truncated = true;
                if (out_truncated)
                    *out_truncated = true;
            } else if (!hard_gate) {
                // Happy path soft: continue.
                return true;
            } else if (solved && !truncated && gate.allow) {
                return true; // AC4 hard happy path
            }
        }
        // Soft/Sampled without CS-side bad snap already returned above.
        if (!hard_gate)
            return true;
        if (solved && !truncated)
            return true;
        // AC4: TIMEOUT / partial should expose affected nodes when hard-gate fires.
        (void)has_unresolved_nodes;
    } else {
        // No stashed CS: soft continues; hard-gate will full-resync below.
        if (!hard_gate)
            return true;
        solved = false;
        truncated = false;
    }

    // Hard-gate: prefer full resync when type-only / cheap dirty; else force-fail.
    const auto force_n =
        production_defaults_active() ? kAuditForceNodesChangedProduction : kAuditForceNodesChanged;
    const bool prefer_rollback = linear_ops_present || nodes_changed >= force_n;

    if (!prefer_rollback) {
        ac.boundary_solve_full_resync_total.fetch_add(1, std::memory_order_relaxed);
        // Full re-infer (selective empty log → full path inside helper).
        const bool resync_ok = run_post_mutate_typecheck_no_lock();
        // Re-solve on stashed CS after resync when available.
        if (commit_type_checker_opaque_ && commit_cs_live_) {
            auto* ctc = static_cast<TypeChecker*>(commit_type_checker_opaque_);
            auto& cs2 = ctc->constraint_system();
            std::span<const aura::core::TypeId> occ2{};
            if (commit_occurrence_vars_opaque_) {
                auto* occ =
                    static_cast<std::vector<aura::core::TypeId>*>(commit_occurrence_vars_opaque_);
                occ2 = *occ;
            }
            auto sdo2 = run_sdo(cs2, occ2);
            if (sdo2.truncated_reverify) {
                ac.boundary_solve_truncated_seen_total.fetch_add(1, std::memory_order_relaxed);
                if (out_truncated)
                    *out_truncated = true;
            }
            if (sdo2.status == SolveResult::SOLVED && !sdo2.truncated_reverify)
                return true;
            // Full resync cleared diags but reverify still truncated → still fail hard.
            if (resync_ok && sdo2.status == SolveResult::SOLVED && !sdo2.truncated_reverify)
                return true;
            (void)resync_ok;
        } else if (resync_ok) {
            // No CS after resync: empty-diag full infer is the proof.
            return true;
        }
    }

    // Force-fail path (AC1: never silent continue under hard-gate).
    ac.boundary_solve_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
    if (out_force_fail)
        *out_force_fail = true;
    return false;
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
    // Issue #2223: force hard-gate when workspace has match sites (Sampled
    // must not under-sample ADT self-mod), mirror linear_ops_present.
    bool match_sites = false;
    if (workspace_flat_) {
        const auto n = workspace_flat_->size();
        for (aura::ast::NodeId id = 0; id < n; ++id) {
            if (workspace_flat_->has_match_info(id)) {
                match_sites = true;
                break;
            }
        }
    }
    if (!requires_invariant_hard_gate(nodes_changed, linear_ops_present, strict, match_sites)) {
        ac.hard_gate_sampled_skip_total.fetch_add(1, std::memory_order_relaxed);
        // Soft path still observes truncated reverify when CS is live (AC2).
        (void)boundary_solve_proof_gate(/*hard_gate=*/false, linear_ops_present, nodes_changed);
        return true;
    }
    ac.hard_gate_audits_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #2260: type-proof hard gate before invariant suite.
    bool proof_truncated = false;
    bool proof_force = false;
    const bool proof_ok = boundary_solve_proof_gate(
        /*hard_gate=*/true, linear_ops_present, nodes_changed, &proof_truncated, &proof_force);
    if (!proof_ok || proof_force) {
        last_mutate_error_ = format_invariant_deny_reason("type-proof", capability_tenant_id(), op);
        ac.hard_gate_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
        ac.full_strategy_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
        ac.typed_mutation_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
            m->typed_mutation_full_force_rollback_total.fetch_add(1, std::memory_order_relaxed);
            m->typed_mutation_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #2309: hard-gate force-rollback clears the process-wide
        // escape → MoveOp elision gate so a stale "blocked" set from the
        // failed txn can't leak into a subsequent independent mutate
        // (multi-Agent / multi-round). Without this clear, a binding
        // that's no longer escaped post-rollback would still see its
        // MoveOp forced (or vice versa).
        aura_escape_move_gate_clear();
        g_linear_escape_gate_clear_on_rollback_total.fetch_add(1, std::memory_order_relaxed);
        if (strict) {
            strict_mutate_hold_.store(1, std::memory_order_relaxed);
            ac.hard_gate_strict_hold_total.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t mid0 = total_mutations_.load(std::memory_order_relaxed);
        const std::uint64_t epoch0 = defuse_version_.load(std::memory_order_relaxed);
        capture_audit_event_forced(mid0, op, classify_kind(op), epoch0, epoch0, AuditOutcome::Error,
                                   0, static_cast<std::uint32_t>(nodes_changed),
                                   static_cast<std::int64_t>(aura_fiber_current_id()), 0);
        return false;
    }
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
        // Issue #2223: ADT renarrow / revalidate recovery category.
        if (!r.adt_ok) {
            ac.partial_recovery_adt_total.fetch_add(1, std::memory_order_relaxed);
            partial_recover_adt_exhaustiveness(mid);
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
    else if (!r.adt_ok)
        kind = "adt";
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
    // Issue #2309: see the matching clear site at the boundary_solve_proof_gate
    // reject branch above — same fix applied here so a failed typed-mutation
    // audit (Full / Strict) doesn't leak its escape gate into the next mutate.
    aura_escape_move_gate_clear();
    g_linear_escape_gate_clear_on_rollback_total.fetch_add(1, std::memory_order_relaxed);
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

// Issue #2180: long-lived TypeChecker for composite commit CS reuse.
void Evaluator::destroy_commit_type_checker() noexcept {
    // If commit TC is an alias of the #2220 persistent TC, only null the
    // commit pointer (persistent owns the heap object).
    if (commit_type_checker_opaque_ &&
        commit_type_checker_opaque_ == persistent_typechecker_opaque_) {
        commit_type_checker_opaque_ = nullptr;
    } else if (commit_type_checker_opaque_) {
        delete static_cast<TypeChecker*>(commit_type_checker_opaque_);
        commit_type_checker_opaque_ = nullptr;
    }
    if (commit_occurrence_vars_opaque_) {
        delete static_cast<std::vector<aura::core::TypeId>*>(commit_occurrence_vars_opaque_);
        commit_occurrence_vars_opaque_ = nullptr;
    }
    commit_tc_registry_gen_ = 0;
    commit_cs_live_ = false;
}

// Issue #2220: long-lived TypeChecker for multi-round Agent mutate.
void Evaluator::destroy_persistent_typechecker() noexcept {
    if (persistent_typechecker_opaque_) {
        // Drop commit alias first so we don't double-delete.
        if (commit_type_checker_opaque_ == persistent_typechecker_opaque_) {
            commit_type_checker_opaque_ = nullptr;
            commit_cs_live_ = false;
        }
        delete static_cast<TypeChecker*>(persistent_typechecker_opaque_);
        persistent_typechecker_opaque_ = nullptr;
    }
    persistent_tc_registry_gen_ = 0;
    persistent_tc_workspace_gen_ = 0;
}

void Evaluator::invalidate_persistent_typechecker() noexcept {
    if (!persistent_typechecker_opaque_ && !commit_type_checker_opaque_)
        return;
    destroy_persistent_typechecker();
    // Also drop a non-aliased commit TC (legacy path).
    destroy_commit_type_checker();
    ++persistent_tc_invalidate_total_;
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->typecheck_persistent_invalidate_total.fetch_add(1, std::memory_order_relaxed);
    }
}

void* Evaluator::ensure_typechecker() noexcept {
    try {
        auto* reg_raw = ensure_type_registry();
        if (!reg_raw)
            return nullptr;
        auto* reg = static_cast<aura::core::TypeRegistry*>(reg_raw);
        const auto reg_gen = type_registry_generation();
        const auto ws_gen = workspace_flat_generation();
        if (persistent_typechecker_opaque_ &&
            (persistent_tc_registry_gen_ != reg_gen || persistent_tc_workspace_gen_ != ws_gen)) {
            invalidate_persistent_typechecker();
        }
        if (!persistent_typechecker_opaque_) {
            auto* tc = new TypeChecker(*reg);
            if (compiler_metrics_)
                tc->set_metrics(compiler_metrics_);
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
                tc->inject_type_sigs(sig_map, mod_src_map);
            }
            persistent_typechecker_opaque_ = tc;
            persistent_tc_registry_gen_ = reg_gen;
            persistent_tc_workspace_gen_ = ws_gen;
            ++persistent_tc_create_total_;
            if (compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
                m->typecheck_persistent_create_total.fetch_add(1, std::memory_order_relaxed);
                m->typecheck_persistent_wired.store(1, std::memory_order_relaxed);
            }
        } else {
            ++persistent_tc_reuse_total_;
            if (compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
                m->typecheck_persistent_reuse_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        auto* tc = static_cast<TypeChecker*>(persistent_typechecker_opaque_);
        if (compiler_metrics_)
            tc->set_metrics(compiler_metrics_);
        // Epoch for cs_cache_ / predicate memo invalidation (#168 / #2065).
        tc->set_cache_epoch(defuse_version_.load(std::memory_order_relaxed));
        return persistent_typechecker_opaque_;
    } catch (...) {
        // [SILENCE-PRIM] ensure is best-effort; callers fall back to stack TC.
        return nullptr;
    }
}

void Evaluator::stash_partial_constraint_state(void* type_checker_opaque) noexcept {
    if (!type_checker_opaque)
        return;
    try {
        auto* src = static_cast<TypeChecker*>(type_checker_opaque);
        auto* reg_raw = ensure_type_registry();
        if (!reg_raw)
            return;
        auto* reg = static_cast<aura::core::TypeRegistry*>(reg_raw);
        const auto reg_gen = type_registry_generation();
        // Issue #2220: when source is the persistent TypeChecker, alias it as
        // the commit TC so composite_txn_commit solves the same CS (not empty).
        if (type_checker_opaque == persistent_typechecker_opaque_) {
            // Drop a non-aliased prior commit TC if any.
            if (commit_type_checker_opaque_ &&
                commit_type_checker_opaque_ != persistent_typechecker_opaque_) {
                delete static_cast<TypeChecker*>(commit_type_checker_opaque_);
            }
            commit_type_checker_opaque_ = persistent_typechecker_opaque_;
            commit_tc_registry_gen_ = reg_gen;
            if (!commit_occurrence_vars_opaque_)
                commit_occurrence_vars_opaque_ = new std::vector<aura::core::TypeId>();
            auto* occ =
                static_cast<std::vector<aura::core::TypeId>*>(commit_occurrence_vars_opaque_);
            *occ = src->last_occurrence_vars();
            commit_cs_live_ = src->commit_cs_has_work() || src->last_partial_cs_live() ||
                              src->constraint_system().is_dirty() ||
                              src->constraint_system().touched_roots_size() > 0 || !occ->empty();
            return;
        }
        if (commit_type_checker_opaque_ && commit_tc_registry_gen_ != reg_gen)
            destroy_commit_type_checker();
        if (!commit_type_checker_opaque_) {
            auto* tc = new TypeChecker(*reg);
            if (compiler_metrics_)
                tc->set_metrics(compiler_metrics_);
            commit_type_checker_opaque_ = tc;
            commit_tc_registry_gen_ = reg_gen;
        }
        auto* dst = static_cast<TypeChecker*>(commit_type_checker_opaque_);
        if (compiler_metrics_)
            dst->set_metrics(compiler_metrics_);
        // Import solve_delta_cs_ (already received engine marks in partial).
        dst->constraint_system().import_delta_marks_from(src->constraint_system());
        for (const auto& t : src->last_occurrence_vars()) {
            if (t.valid())
                dst->constraint_system().mark_touched_on_delta(t, /*occurrence_narrow=*/true);
        }
        // Stash occurrence span for solve_delta_occurrence call.
        if (!commit_occurrence_vars_opaque_)
            commit_occurrence_vars_opaque_ = new std::vector<aura::core::TypeId>();
        auto* occ = static_cast<std::vector<aura::core::TypeId>*>(commit_occurrence_vars_opaque_);
        *occ = src->last_occurrence_vars();
        commit_cs_live_ = src->commit_cs_has_work() || src->last_partial_cs_live() ||
                          dst->constraint_system().is_dirty() ||
                          dst->constraint_system().touched_roots_size() > 0 || !occ->empty();
    } catch (...) {
        // [SILENCE-PRIM] stash is best-effort; commit falls back to empty CS.
    }
}

// Issue #2223: Full-strategy ADT renarrow + revalidate (partial recovery).
// Best-effort: renarrow does not synthesize missing match clauses; re-audit
// must still see adt_ok for commit to continue.
void Evaluator::partial_recover_adt_exhaustiveness(std::uint64_t mutation_id) noexcept {
    try {
        if (!workspace_flat_ || !workspace_pool_ || !type_registry_)
            return;
        auto& flat = *workspace_flat_;
        auto& pool = *workspace_pool_;
        auto& reg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
        std::vector<aura::ast::NodeId> roots;
        for (const auto& rec : flat.all_mutations()) {
            if (rec.target_node != 0)
                roots.push_back(rec.target_node);
            if (rec.parent_id != 0)
                roots.push_back(rec.parent_id);
        }
        if (roots.empty() && flat.root != 0)
            roots.push_back(flat.root);
        (void)selective_adt_guardshape_renarrow(flat, pool, reg, roots, compiler_metrics_);
        aura::ast::MutationRecord stub{};
        stub.mutation_id = mutation_id;
        revalidate_adt_typed_mutation_scope(flat, pool, reg, roots, stub, current_cache_epoch(),
                                            compiler_metrics_);
    } catch (...) {
        // [SILENCE-PRIM] ADT recovery best-effort
    }
}

void Evaluator::inject_commit_cs_type_conflict_for_test() noexcept {
    try {
        auto* reg_raw = ensure_type_registry();
        if (!reg_raw)
            return;
        auto* reg = static_cast<aura::core::TypeRegistry*>(reg_raw);
        const auto reg_gen = type_registry_generation();
        if (commit_type_checker_opaque_ && commit_tc_registry_gen_ != reg_gen)
            destroy_commit_type_checker();
        if (!commit_type_checker_opaque_) {
            auto* tc = new TypeChecker(*reg);
            if (compiler_metrics_)
                tc->set_metrics(compiler_metrics_);
            commit_type_checker_opaque_ = tc;
            commit_tc_registry_gen_ = reg_gen;
        }
        auto* tc = static_cast<TypeChecker*>(commit_type_checker_opaque_);
        auto& cs = tc->constraint_system();
        const auto t = cs.fresh_var();
        cs.add_delta({Constraint::EQUAL, t, reg->int_type()});
        (void)cs.solve_delta();
        // Second delta conflicts — remains dirty for next solve_delta_occurrence.
        cs.add_delta({Constraint::EQUAL, t, reg->string_type()});
        commit_cs_live_ = true;
    } catch (...) {
        // [SILENCE-PRIM] test helper
    }
}

// Issue #2260: force truncated_reverify on next solve_delta_occurrence.
// Mirrors test_adaptive_reverify_limit_2146 AC2 setup (low limit + clean fan-out).
void Evaluator::inject_commit_cs_truncated_reverify_for_test() noexcept {
    try {
        auto* reg_raw = ensure_type_registry();
        if (!reg_raw)
            return;
        auto* reg = static_cast<aura::core::TypeRegistry*>(reg_raw);
        const auto reg_gen = type_registry_generation();
        if (commit_type_checker_opaque_ && commit_tc_registry_gen_ != reg_gen)
            destroy_commit_type_checker();
        if (!commit_type_checker_opaque_) {
            auto* tc = new TypeChecker(*reg);
            if (compiler_metrics_)
                tc->set_metrics(compiler_metrics_);
            commit_type_checker_opaque_ = tc;
            commit_tc_registry_gen_ = reg_gen;
        }
        auto* tc = static_cast<TypeChecker*>(commit_type_checker_opaque_);
        auto& cs = tc->constraint_system();
        // Pin reverify limit low so clean fan-out truncates.
        cs.force_reverify_limit_for_test(8);
        auto shared = cs.fresh_var();
        cs.mark_touched_on_delta(shared, /*occurrence_narrow=*/false);
        for (int i = 0; i < 40; ++i) {
            auto o = cs.fresh_var();
            Constraint c;
            c.kind = Constraint::EQUAL;
            c.lhs = shared;
            c.rhs = o;
            cs.add(c);
        }
        // Dirty delta so solve_delta / occurrence runs reverify.
        Constraint d2;
        d2.kind = Constraint::EQUAL;
        d2.lhs = cs.fresh_var();
        d2.rhs = cs.fresh_var();
        d2.source_mutation_id = 2260;
        cs.add_delta(d2);
        cs.mark_touched_on_delta(d2.lhs, false);
        commit_cs_live_ = true;
        if (!commit_occurrence_vars_opaque_)
            commit_occurrence_vars_opaque_ = new std::vector<aura::core::TypeId>();
        auto* occ = static_cast<std::vector<aura::core::TypeId>*>(commit_occurrence_vars_opaque_);
        occ->clear();
        occ->push_back(shared);
    } catch (...) {
        // [SILENCE-PRIM] test helper
    }
}

// Issue #2221: seed commit CS with incomplete / complete last_blame_chain.
void Evaluator::inject_commit_cs_incomplete_blame_for_test() noexcept {
    try {
        auto* reg_raw = ensure_type_registry();
        if (!reg_raw)
            return;
        auto* reg = static_cast<aura::core::TypeRegistry*>(reg_raw);
        const auto reg_gen = type_registry_generation();
        if (commit_type_checker_opaque_ && commit_tc_registry_gen_ != reg_gen)
            destroy_commit_type_checker();
        if (!commit_type_checker_opaque_) {
            auto* tc = new TypeChecker(*reg);
            if (compiler_metrics_)
                tc->set_metrics(compiler_metrics_);
            commit_type_checker_opaque_ = tc;
            commit_tc_registry_gen_ = reg_gen;
        }
        auto* tc = static_cast<TypeChecker*>(commit_type_checker_opaque_);
        tc->force_last_blame_incomplete_for_test(/*mutation_id=*/2221, /*node=*/1);
        commit_cs_live_ = true;
    } catch (...) {
        // [SILENCE-PRIM] test helper
    }
}

void Evaluator::inject_commit_cs_complete_blame_for_test() noexcept {
    try {
        auto* reg_raw = ensure_type_registry();
        if (!reg_raw)
            return;
        auto* reg = static_cast<aura::core::TypeRegistry*>(reg_raw);
        const auto reg_gen = type_registry_generation();
        if (commit_type_checker_opaque_ && commit_tc_registry_gen_ != reg_gen)
            destroy_commit_type_checker();
        if (!commit_type_checker_opaque_) {
            auto* tc = new TypeChecker(*reg);
            if (compiler_metrics_)
                tc->set_metrics(compiler_metrics_);
            commit_type_checker_opaque_ = tc;
            commit_tc_registry_gen_ = reg_gen;
        }
        auto* tc = static_cast<TypeChecker*>(commit_type_checker_opaque_);
        tc->force_last_blame_complete_for_test(/*mutation_id=*/2221, /*pred=*/7, /*node=*/3);
        commit_cs_live_ = true;
    } catch (...) {
        // [SILENCE-PRIM] test helper
    }
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

        // Issue #2285 Phase 2: selective invalidate from occurrence_targets
        // (broader than dirty_var_names which only walks the target_node
        // subtree). Covers type_dep additions and any If-context that the
        // reanalysis surfaced but the dirty walk missed. Zero cost when
        // occurrence_targets is empty (helper no-op on empty set).
        std::size_t dropped_affected = 0;
        {
            std::unordered_set<std::string> guard_affected_names;
            guard_affected_names.reserve(occurrence_targets.size());
            for (auto nid : occurrence_targets) {
                if (nid == aura::ast::NULL_NODE || nid >= flat.size())
                    continue;
                auto nv = flat.get(nid);
                if (nv.sym_id != aura::ast::INVALID_SYM &&
                    (nv.tag == aura::ast::NodeTag::Variable ||
                     nv.tag == aura::ast::NodeTag::Define || nv.tag == aura::ast::NodeTag::Let ||
                     nv.tag == aura::ast::NodeTag::LetRec ||
                     nv.tag == aura::ast::NodeTag::Lambda)) {
                    auto nm = pool.resolve(nv.sym_id);
                    if (!nm.empty())
                        guard_affected_names.insert(std::string(nm));
                }
            }
            if (!guard_affected_names.empty())
                dropped_affected =
                    eng->invalidate_predicate_memo_for_var_names(guard_affected_names);
        }

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
            if (dropped_affected > 0) {
                // Issue #2285 Phase 2: Phase-2-only drops (occurrence_targets
                // names not already in dirty_var_names).
                m->guard_exit_selective_invalidate_total.fetch_add(dropped_affected,
                                                                   std::memory_order_relaxed);
                m->predicate_memo_selective_invalidate_total.fetch_add(dropped_affected,
                                                                       std::memory_order_relaxed);
                m->predicate_memo_boundary_selective_total.fetch_add(dropped_affected,
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