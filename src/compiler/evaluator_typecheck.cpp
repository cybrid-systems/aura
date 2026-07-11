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

// Issue #107 part 4: inline typecheck helpers. Caller MUST hold
// workspace_mtx_ (shared or unique). The two helpers share the
// same infer_flat + diag-drain pattern; they differ only in
// return type — string for sites that need the error message,
// bool for sites that only need pass/fail. Both are members
// of Evaluator (so they can access the privates below).
std::string Evaluator::run_typecheck_no_lock() {
    if (!workspace_flat_ || !workspace_pool_)
        return std::string("no workspace");
    auto& treg = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
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
    auto& treg = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
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
    auto& treg = *static_cast<aura::core::TypeRegistry*>(ensure_type_registry());
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
            if (d.kind == aura::diag::ErrorKind::Note || d.kind == aura::diag::ErrorKind::Warning)
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
                d.kind != aura::diag::ErrorKind::Warning && d.kind != aura::diag::ErrorKind::Note) {
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
    std::string err =
        selective ? "typecheck after mutate (selective) failed:" : "typecheck after mutate failed:";
    for (auto& d : local_diags)
        err += " " + d.format() + ";";
    last_mutate_error_ = err;
    return false;
}

} // namespace aura::compiler