// evaluator_typecheck.cpp — P1-j: inline typecheck helpers
// aura.compiler.evaluator module partition.

module;

#include "observability_metrics.h"


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
using namespace types;
using namespace aura::diag;

// Issue #107 part 4: inline typecheck helpers. Caller MUST hold
// workspace_mtx_ (shared or unique). The two helpers share the
// same infer_flat + diag-drain pattern; they differ only in
// return type — string for sites that need the error message,
// bool for sites that only need pass/fail. Both are members
// of Evaluator (so they can access the privates below).
std::string Evaluator::run_typecheck_no_lock() {
    if (!workspace_flat_ || !workspace_pool_)
        return std::string("no workspace");
    if (!type_registry_) {
        type_registry_ = new aura::core::TypeRegistry();
        owns_type_registry_ = true;
    }
    auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
    aura::compiler::TypeChecker tc(treg);
    if (!declared_type_sigs_.empty()) {
        std::unordered_map<std::string, std::string> sig_map;
        std::unordered_map<std::string, std::string> mod_src_map;
        for (auto& [name, decl] : declared_type_sigs_) {
            sig_map[name] = decl.type_str;
            if (!decl.module_file.empty())
                mod_src_map[name] = decl.module_file;
        }
        tc.inject_type_sigs(sig_map, mod_src_map);
    }
    aura::diag::DiagnosticCollector diag;
    auto result = tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
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
}

bool Evaluator::run_typecheck_no_lock_bool() {
    // Same as the string version but returns pass/fail directly
    // without formatting. Cheaper for hot fuzzer loops.
    //
    // Issue #116: this is called from the fuzzy/evolutionary loop
    // (compute_fitness), which then `eval`s the workspace. The
    // workspace must be lowering-ready, so we apply the deferred
    // CoercionMap before returning.
    if (!workspace_flat_ || !workspace_pool_)
        return true;
    if (!type_registry_) {
        type_registry_ = new aura::core::TypeRegistry();
        owns_type_registry_ = true;
    }
    auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
    aura::compiler::TypeChecker tc(treg);
    if (!declared_type_sigs_.empty()) {
        std::unordered_map<std::string, std::string> sig_map;
        std::unordered_map<std::string, std::string> mod_src_map;
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
            aura::compiler::apply_coercion_map(*workspace_flat_, cm);
        }
    }
    workspace_flat_->clear_all_dirty();
    return diag.diagnostics().empty();
}

bool Evaluator::run_post_mutate_typecheck_no_lock() {
    // Issue #526: post-mutate selective type recheck for Aura
    // mutate primitives (rebind / set-body). When the mutation
    // log has entries, route through infer_flat_partial on the
    // latest record (solve_delta + occurrence re-narrow on the
    // affected subtree only). Fall back to full infer_flat when
    // the log is empty (degenerate / pre-log mutations).
    if (!workspace_flat_ || !workspace_pool_)
        return true;
    if (!type_registry_) {
        type_registry_ = new aura::core::TypeRegistry();
        owns_type_registry_ = true;
    }
    auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
    aura::compiler::TypeChecker tc(treg);
    if (!declared_type_sigs_.empty()) {
        std::unordered_map<std::string, std::string> sig_map;
        std::unordered_map<std::string, std::string> mod_src_map;
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
            m->narrowing_applied_total.fetch_add(st.narrowing_applied, std::memory_order_relaxed);
            m->narrowing_skipped_total.fetch_add(st.narrowing_skipped, std::memory_order_relaxed);
            m->narrowing_reanalyzed_total.fetch_add(st.narrowing_reanalyzed,
                                                    std::memory_order_relaxed);
            m->and_or_meet_uses_total.fetch_add(st.and_or_meet_uses, std::memory_order_relaxed);
            m->and_or_join_uses_total.fetch_add(st.and_or_join_uses, std::memory_order_relaxed);
            m->narrowing_dirty_recovery_total.fetch_add(st.narrowing_dirty_recovery,
                                                        std::memory_order_relaxed);
        }
    } else {
        tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
        workspace_flat_->clear_all_dirty();
    }

    {
        auto cm = tc.take_coercions();
        if (!cm.empty()) {
            aura::compiler::apply_coercion_map(*workspace_flat_, cm);
            // Issue #659: post-mutate CoercionMap application counts as an
            // incremental coercion win on the typed-mutation path.
            if (compiler_metrics_) {
                auto* metrics = static_cast<struct CompilerMetrics*>(compiler_metrics_);
                metrics->coercion_zerooverhead_win_total.fetch_add(cm.size(),
                                                                   std::memory_order_relaxed);
            }
        }
    }

    auto local_diags = diag.diagnostics();
    if (local_diags.empty()) {
        last_mutate_error_.clear();
        return true;
    }
    std::string err =
        selective ? "typecheck after mutate (selective) failed:" : "typecheck after mutate failed:";
    for (auto& d : local_diags)
        err += " " + d.format() + ";";
    last_mutate_error_ = err;
    return false;
}

} // namespace aura::compiler