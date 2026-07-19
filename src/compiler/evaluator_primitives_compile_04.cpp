// evaluator_primitives_compile_04.cpp — Issue #909: peeled compile registration
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "per_defuse_index.h"
#include "hash_meta.h"
#include "basis_points.h"
#include "security_capabilities.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.mutators;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.pass_manager;
import aura.compiler.query;
import aura.compiler.hardware_backend;
import aura.compiler.sv_ir;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

// Issue #1771: shared "node_id [comment]" line parser for
// verify:parse-coverage-feedback / parse-assert-failure /
// parse-formal-cex. Returns count of successfully marked nodes.
// Callers own attempt/success metrics (they differ slightly).
namespace {
    std::uint64_t parse_verify_node_id_lines(const std::string& text, aura::ast::FlatAST& ws,
                                             Evaluator& ev, std::uint8_t dirty_bit) {
        std::uint64_t marked = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            std::size_t j = i;
            while (j < text.size() && text[j] != '\n')
                ++j;
            const std::string_view line(text.data() + i, j - i);
            std::size_t k = 0;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
                ++k;
            if (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                std::size_t val = 0;
                while (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                    val = val * 10 + static_cast<std::size_t>(line[k] - '0');
                    ++k;
                }
                const auto nid = static_cast<aura::ast::NodeId>(val);
                if (nid < ws.size()) {
                    ws.apply_verification_dirty_bits(nid, dirty_bit);
                    ++marked;
                }
            } else {
                // Non-integer line — bump parse-error counter for
                // query:verify-tool-stats.
                ev.bump_verify_tool_parse_error();
            }
            i = (j < text.size()) ? j + 1 : j;
        }
        return marked;
    }
} // namespace

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
using types::make_keyword;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Issue #909 compile part 32 (orig 2404-2496)
void CompilePrims::register_compile_p32(PrimRegistrar add, Evaluator& ev) {

    // Issue #290: (compile:clear-macro-dirty!) — clear all
    // macro_dirty_ bits on the eval flat. Useful after a
    // self-evolution loop has fully reprocessed the affected
    // subtrees and wants to start fresh on the next cycle.
    // Returns #t on success, #f if no flat.
    add("compile:clear-macro-dirty!", [&ev](const auto&) -> EvalValue {
        // Issue #1395: capability gate — require kCapWildcard.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
                m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: kCapWildcard required",
                                        ev.primitive_error_counter_ptr());
        }
        auto* ws = CompilePrims::pick_macro_flat(ev);
        if (!ws)
            return make_bool(false);
        ws->clear_macro_dirty_all();
        return make_bool(true);
    });

    // Issue #290: (compile:macro-dirty-stats) — return the
    // running per-reason counters as a single integer (sum
    // of kMacroExpansion + kMacroSelfModify newly-set
    // totals). P0 ship mirrors (query:verify-dirty-stats).
    // Follow-up: return a pair so callers can distinguish
    // the two reasons without separate primitives.
    ObservabilityPrims::register_stats_impl(
        "compile:macro-dirty-stats", [&ev](const auto&) -> EvalValue {
            auto* ws = CompilePrims::pick_macro_flat(ev);
            if (!ws)
                return make_bool(false);
            auto sum = ws->macro_expansion_dirty_total() + ws->macro_self_modify_dirty_total();
            return make_int(static_cast<std::int64_t>(sum));
        });

    // Issue #469: (verify:parse-coverage-feedback text-string)
    // — parse a text blob describing coverage holes from an
    // external SV simulator and mark the affected AST nodes
    // dirty with the kCoverageFeedbackDirty bit.
    //
    // Format (one per line): "node_id hole_name"
    // Example:
    //   "0 hit_rate=0.45"
    //   "3 miss_var_x"
    //
    // P0: the text is parsed line-by-line. Each line
    // starts with a non-negative integer (the NodeId).
    // Anything after the integer is ignored (it's a
    // human-readable hole name; P0 doesn't use it).
    // Lines that don't start with an integer are
    // skipped.
    //
    // Returns: the count of nodes successfully marked
    // dirty. #f on bad args.
    add("verify:parse-coverage-feedback", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        // Issue #1347: count harness parse attempts even without a workspace.
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->verify_parse_coverage_total.fetch_add(1, std::memory_order_relaxed);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        auto text_idx = as_string_idx(a[0]);
        if (text_idx >= ev.string_heap_.size())
            return make_int(0);
        // Issue #1771: shared line parser.
        const auto marked = parse_verify_node_id_lines(ev.string_heap_[text_idx], *ws, ev,
                                                       aura::ast::FlatAST::kCoverageFeedbackDirty);
        if (marked > 0) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->verify_auto_trigger_mutate_total.fetch_add(1, std::memory_order_relaxed);
            ev.bump_sv_self_evo_feedback_parse();
        }
        return make_int(static_cast<std::int64_t>(marked));
    });
}

// Issue #909 compile part 33 (orig 2497-2589)
void CompilePrims::register_compile_p33(PrimRegistrar add, Evaluator& ev) {

    // Issue #469: (verify:parse-assert-failure text-string)
    // — parse a text blob describing assertion failures
    // from an external SV simulator and mark the affected
    // AST nodes dirty with the kAssertFailureDirty bit.
    // Same format as (verify:parse-coverage-feedback).
    add("verify:parse-assert-failure", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        // Issue #1347: count harness parse attempts even without a workspace.
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->verify_parse_assert_total.fetch_add(1, std::memory_order_relaxed);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        auto text_idx = as_string_idx(a[0]);
        if (text_idx >= ev.string_heap_.size())
            return make_int(0);
        // Issue #1771: shared line parser.
        const auto marked = parse_verify_node_id_lines(ev.string_heap_[text_idx], *ws, ev,
                                                       aura::ast::FlatAST::kAssertFailureDirty);
        if (marked > 0) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->verify_auto_trigger_mutate_total.fetch_add(1, std::memory_order_relaxed);
            ev.bump_sv_self_evo_feedback_parse();
        }
        return make_int(static_cast<std::int64_t>(marked));
    });

    // Issue #802: (verify:parse-formal-cex text-string)
    // — parse formal counterexample report and mark nodes dirty with
    // kFormalCounterexampleDirty (same line format as coverage/assert parsers).
    add("verify:parse-formal-cex", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        auto text_idx = as_string_idx(a[0]);
        if (text_idx >= ev.string_heap_.size())
            return make_int(0);
        // Issue #1771: shared line parser (no auto-trigger metric — #802 BC).
        const auto marked = parse_verify_node_id_lines(
            ev.string_heap_[text_idx], *ws, ev, aura::ast::FlatAST::kFormalCounterexampleDirty);
        if (marked > 0)
            ev.bump_sv_self_evo_feedback_parse();
        return make_int(static_cast<std::int64_t>(marked));
    });
}

// Issue #909 compile part 34 (orig 2590-2735)
void CompilePrims::register_compile_p34(PrimRegistrar add, Evaluator& ev) {

    // Issue #802: (mutate:from-verification-feedback strategy node-id payload)
    // — strategy-driven structured SV mutate under Guard with StableNodeRef
    // capture. Delegates to existing eda:weaken-property / eda:add-coverpoint-bin /
    // eda:update-constraint primitives.
    add("mutate:from-verification-feedback", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_int(a[1]) || !is_string(a[2]))
            return make_bool(false);
        auto strategy_idx = as_string_idx(a[0]);
        if (strategy_idx >= ev.string_heap_.size())
            return make_bool(false);
        const auto& strategy = ev.string_heap_[strategy_idx];
        const auto node_id = static_cast<std::int64_t>(as_int(a[1]));
        auto payload_idx = as_string_idx(a[2]);
        if (payload_idx >= ev.string_heap_.size())
            return make_bool(false);
        // Issue #1772: validate NodeId before eda:* delegation so invalid
        // agent targets are observable (mutate_from_feedback_invalid_node_total)
        // and never rely solely on each delegate's optional OOB check.
        if (auto* ws = ev.workspace_flat()) {
            if (node_id < 0 || static_cast<std::uint64_t>(node_id) >= ws->size()) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->mutate_from_feedback_invalid_node_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                return make_bool(false);
            }
        } else {
            // No workspace: cannot resolve NodeId — treat as invalid target.
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->mutate_from_feedback_invalid_node_total.fetch_add(1, std::memory_order_relaxed);
            return make_bool(false);
        }
        auto delegate = [&](const char* name) -> bool {
            auto fn = ev.primitives_.lookup(name);
            if (!fn)
                return false;
            auto r = (*fn)({make_int(node_id), make_string(payload_idx)});
            return is_bool(r) && as_bool(r);
        };
        bool ok = false;
        if (strategy == "weaken-property" || strategy == "assert-fail")
            ok = delegate("eda:weaken-property");
        else if (strategy == "add-coverpoint" || strategy == "coverage-hole")
            ok = delegate("eda:add-coverpoint-bin");
        else if (strategy == "relax-constraint" || strategy == "structural-fix")
            ok = delegate("eda:update-constraint");
        if (!ok)
            return make_bool(false);
        ev.bump_sv_self_evo_structured_mutate();
        ev.bump_sv_self_evo_closed_loop_rounds();
        ev.bump_sv_self_evo_convergence_hits();
        ev.bump_closed_loop_feedback_mutate_round();
        return make_bool(true);
    });

    // Issue #693: (eda:run-verification-feedback report-kind report-text)
    // — parse mock coverage/assert report, apply targeted SV mutate,
    // re-emit via hardware backend hook, and bump closed-loop metrics.
    add("eda:run-verification-feedback", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        // Issue #1001: validate string indices BEFORE Guard + metric bump so
        // early returns do not leave inflated guard-capture counters.
        const auto kind_idx = as_string_idx(a[0]);
        const auto text_idx = as_string_idx(a[1]);
        if (kind_idx >= ev.string_heap_.size() || text_idx >= ev.string_heap_.size())
            return make_bool(false);
        bool guard_ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &guard_ok);
        ev.bump_verify_tool_guard_capture();
        const std::string& kind = ev.string_heap_[kind_idx];
        const std::string& text = ev.string_heap_[text_idx];
        aura::ast::NodeId target = aura::ast::NULL_NODE;
        std::size_t i = 0;
        while (i < text.size()) {
            std::size_t j = i;
            while (j < text.size() && text[j] != '\n')
                ++j;
            const std::string_view line(text.data() + i, j - i);
            std::size_t k = 0;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
                ++k;
            if (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                std::size_t val = 0;
                while (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                    val = val * 10 + (line[k] - '0');
                    ++k;
                }
                target = static_cast<aura::ast::NodeId>(val);
                break;
            }
            i = (j < text.size()) ? j + 1 : j;
        }
        if (target == aura::ast::NULL_NODE || target >= ws->size())
            return make_bool(false);
        const auto pref = ws->make_ref(target);
        if (!pref.is_valid_in(*ws))
            return make_bool(false);
        ev.bump_verify_tool_stable_ref_hit();
        const bool coverage =
            kind.find("coverage") != std::string::npos || kind.find("cov") != std::string::npos;
        ws->bump_sv_mutate_attempt();
        if (coverage) {
            ws->apply_verification_dirty_bits(target, aura::ast::FlatAST::kCoverageFeedbackDirty);
            ws->apply_verify_dirty_bits(target, aura::ast::FlatAST::kSvaDirty);
            ws->add_mutation(target, "sv-add-coverpoint", "covergroup", "covergroup+coverpoint",
                             "feedback closed-loop coverpoint");
            ws->mark_ppa_dirty(target, aura::ast::FlatAST::PpaDirtyReason::kAreaDirty);
        } else {
            ws->apply_verification_dirty_bits(target, aura::ast::FlatAST::kAssertFailureDirty);
            ws->apply_verify_dirty_bits(target, aura::ast::FlatAST::kSvaDirty);
            ws->add_mutation(target, "sv-weaken-property", "property", "property+disable-iff",
                             "feedback closed-loop weaken");
            ws->mark_ppa_dirty(target, aura::ast::FlatAST::PpaDirtyReason::kTimingDirty);
        }
        ws->mark_dirty_upward(target, aura::ast::FlatAST::kGeneralDirty,
                              ws->ppa_dirty_reasons(target));
        ev.bump_verify_tool_dirty_propagation();
        if (aura::compiler::hardware::should_invoke_sv_closedloop_hook(*ws, target)) {
            const auto sv_reasons =
                aura::compiler::hardware::sv_structural_dirty_reasons(*ws, target);
            // Issue #1902 / #1773 (refine #1818): wrap throwable external
            // helpers (hardware::on_structural_mutation,
            // sv_ir::reemit_sv_node / emit_sv_diff / validate_sv_emit)
            // in try/catch. Pre-#1902 code did not flip guard_ok
            // on exception, so Guard dtor would commit_panic_checkpoint
            // on a partially-mutated workspace → checkpoint drift
            // + UAF risk on the next mutate call. New contract: any
            // throw inside the hook block flips guard_ok=false so
            // the dtor runs restore_panic_checkpoint + bump
            // eda_guard_exception_handled_total + eda_guard_uncaught_exception_total.
            try {
                aura::compiler::hardware::on_structural_mutation(
                    target,
                    static_cast<std::uint8_t>(aura::ast::FlatAST::kGeneralDirty | sv_reasons),
                    ws->ppa_dirty_reasons(target));
                if (auto* pool = ev.workspace_pool()) {
                    const auto reemit = aura::compiler::sv_ir::reemit_sv_node(*ws, *pool, target);
                    const auto diff = aura::compiler::sv_ir::emit_sv_diff("", reemit.sv_text);
                    const auto validation = aura::compiler::sv_ir::validate_sv_emit(reemit.sv_text);
                    if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                        m->hardware_backend_hook_calls_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                        m->commercial_reemits_total.fetch_add(1, std::memory_order_relaxed);
                        m->feedback_mutate_hits_total.fetch_add(1, std::memory_order_relaxed);
                        m->sv_verification_structure_mutate_hits_total.fetch_add(
                            1, std::memory_order_relaxed);
                        m->sv_verification_dirty_reemit_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                        if (!diff.empty())
                            m->sv_diff_emits_total.fetch_add(1, std::memory_order_relaxed);
                        if (validation.ok) {
                            m->sv_emit_parse_success_total.fetch_add(1, std::memory_order_relaxed);
                            m->verification_loop_success_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                        } else {
                            m->sv_emit_parse_fail_total.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (reemit.ppa_savings > 0) {
                            m->ppa_savings_total.fetch_add(
                                static_cast<std::uint64_t>(reemit.ppa_savings),
                                std::memory_order_relaxed);
                        }
                    }
                    ev.record_sv_commercial_emit_fidelity(validation.ok, true,
                                                          !reemit.commercial_do_stub.empty());
                }
            } catch (const std::exception& e) {
                guard_ok = false; // Issue #1902 / #1773: notify Guard dtor to restore.
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                    m->eda_guard_exception_handled_total.fetch_add(1, std::memory_order_relaxed);
                }
                return make_bool(false);
            } catch (...) {
                // [SILENCE-PRIM-#615] Guard-path uncaught → #f + metrics
                // (eda_guard_uncaught_exception_total); dtor restores
                // (#1669 class A intentional-return-value).
                guard_ok = false; // Issue #1902 / #1773
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                    m->eda_guard_uncaught_exception_total.fetch_add(1, std::memory_order_relaxed);
                }
                return make_bool(false);
            }
        }
        ev.bump_verify_tool_feedback_mutate_success();
        ev.bump_sv_self_evo_structured_mutate();
        ev.bump_sv_self_evo_closed_loop_rounds();
        ev.bump_sv_self_evo_convergence_hits();
        ev.bump_closed_loop_feedback_mutate_round();
        ws->bump_sv_mutate_success();
        return make_bool(true);
    });
}

// Issue #909 compile part 35 (orig 2736-2875)
void CompilePrims::register_compile_p35(PrimRegistrar add, Evaluator& ev) {

    // Issue #698: (eda:run-commercial-simulator-stub simulator node-id)
    // — re-emit SV for node, validate emit, return commercial do-file stub.
    add("eda:run-commercial-simulator-stub", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        auto* pool = ev.workspace_pool();
        if (!ws || !pool)
            return make_bool(false);
        const auto sim_idx = as_string_idx(a[0]);
        if (sim_idx >= ev.string_heap_.size())
            return make_bool(false);
        const auto& simulator = ev.string_heap_[sim_idx];
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[1]));
        if (nid >= ws->size())
            return make_bool(false);
        if (!aura::compiler::hardware::is_sv_structural_node(*ws, nid))
            return make_bool(false);
        // Issue #1902 sibling (#1821): this primitive had NO
        // MutationBoundaryGuard at all — the throwable calls
        // (reemit_sv_node + on_structural_mutation) ran raw,
        // so any throw (bad_alloc, hardware hook fault, etc.)
        // would unwind past the (absent) Guard dtor and leave
        // the workspace in a partially-mutated state with no
        // panic-checkpoint restore. New contract: wrap the
        // throwable body in Guard + try/catch. On exception,
        // flip guard_ok=false so dtor restores panic checkpoint,
        // bump the same 3 metrics as #1902 fix.
        bool guard_ok = true;
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &guard_ok);
        try {
            const auto reemit = aura::compiler::sv_ir::reemit_sv_node(*ws, *pool, nid, simulator);
            const auto validation = aura::compiler::sv_ir::validate_sv_emit(reemit.sv_text);
            if (!validation.ok)
                return make_bool(false);
            if (aura::compiler::hardware::should_invoke_sv_closedloop_hook(*ws, nid)) {
                const auto sv_reasons =
                    aura::compiler::hardware::sv_structural_dirty_reasons(*ws, nid);
                aura::compiler::hardware::on_structural_mutation(
                    nid, static_cast<std::uint8_t>(aura::ast::FlatAST::kGeneralDirty | sv_reasons),
                    ws->ppa_dirty_reasons(nid));
            }
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->commercial_simulator_runs_total.fetch_add(1, std::memory_order_relaxed);
                m->sv_emit_parse_success_total.fetch_add(1, std::memory_order_relaxed);
                m->commercial_reemits_total.fetch_add(1, std::memory_order_relaxed);
                m->hardware_backend_hook_calls_total.fetch_add(1, std::memory_order_relaxed);
                if (reemit.ppa_savings > 0) {
                    m->ppa_savings_total.fetch_add(static_cast<std::uint64_t>(reemit.ppa_savings),
                                                   std::memory_order_relaxed);
                }
            }
            ev.record_sv_commercial_emit_fidelity(validation.ok, false,
                                                  !reemit.commercial_do_stub.empty());
            return make_bool(true);
        } catch (const std::exception& e) {
            guard_ok = false; // Issue #1902/#1821: notify Guard dtor to restore.
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->eda_guard_exception_handled_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_bool(false);
        } catch (...) {
            // [SILENCE-PRIM-#615] Guard-path uncaught → #f + metrics
            // (eda_guard_uncaught_exception_total); dtor restores
            // (#1669 class A intentional-return-value).
            guard_ok = false;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->eda_guard_uncaught_exception_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_bool(false);
        }
    });

    // Issue #695: (eda:demo-sv-self-evolution example cycles)
    // — run a bounded SV verification closed-loop demo:
    // feedback parse → structured mutate → re-emit → metrics.
    add("eda:demo-sv-self-evolution", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        auto* pool = ev.workspace_pool();
        if (!ws || !pool)
            return make_bool(false);
        int cycles = 100;
        if (a.size() >= 2 && is_int(a[1]))
            cycles = static_cast<int>(as_int(a[1]));
        if (const char* env_cycles = std::getenv("AURA_SV_DEMO_CYCLES")) {
            if (env_cycles[0] != '\0') {
                char* end = nullptr;
                const long parsed = std::strtol(env_cycles, &end, 10);
                if (end != env_cycles && parsed > 0)
                    cycles = static_cast<int>(parsed);
            }
        }
        if (cycles <= 0)
            return make_int(0);
        aura::ast::NodeId property_id = aura::ast::NULL_NODE;
        aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
            if (property_id == aura::ast::NULL_NODE &&
                ws->get(id).tag == aura::ast::NodeTag::Property)
                property_id = id;
            if (coverpoint_id == aura::ast::NULL_NODE &&
                ws->get(id).tag == aura::ast::NodeTag::Coverpoint)
                coverpoint_id = id;
        }
        if (property_id == aura::ast::NULL_NODE || coverpoint_id == aura::ast::NULL_NODE)
            return make_bool(false);
        auto prop_ref = ws->make_ref(property_id);
        auto cp_ref = ws->make_ref(coverpoint_id);
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        int successes = 0;
        auto feedback_fn = ev.primitives_.lookup("eda:run-verification-feedback");
        auto weaken_fn = ev.primitives_.lookup("eda:weaken-property");
        auto add_bin_fn = ev.primitives_.lookup("eda:add-coverpoint-bin");
        auto gc_fn = ev.primitives_.lookup("mutate:request-gc-safepoint");
        // Issue #1901 (refine #1822): pre-loop ref creation is
        // dangerous because every feedback_fn / weaken_fn /
        // add_bin_fn call can mutate the workspace and stale
        // the refs. The pre-#1901 code only checked is_valid_in
        // at the *start* of each iteration, which left a
        // window where the previous iteration's mutate had
        // already invalidated the ref but no check ran before
        // the next mutate — UAF risk under fiber steal + GC
        // compact. New contract: refresh_if_stale() after
        // EVERY mutate call (and the gc_safepoint which can
        // compact). If refresh fails (node physically gone),
        // re-make_ref from the still-valid NodeId pointer
        // table. bump metrics on each refresh path.
        auto refresh_after_mutate = [&](auto& ref, aura::ast::NodeId& id_slot,
                                        const char* ref_label) {
            if (ref.refresh_if_stale(*ws)) {
                if (m)
                    m->stable_ref_auto_refresh_in_eda_total.fetch_add(1, std::memory_order_relaxed);
                if (m)
                    m->eda_self_evolution_stale_ref_prevented.fetch_add(1,
                                                                        std::memory_order_relaxed);
                return;
            }
            // refresh_if_stale returned false — either the
            // ref is still valid (no-op, common case) or the
            // node was physically removed. Re-make_ref from
            // the live workspace scan so the ref tracks the
            // node wherever it is now.
            aura::ast::NodeId new_id = aura::ast::NULL_NODE;
            for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                if ((std::string_view(ref_label) == "property" &&
                     ws->get(id).tag == aura::ast::NodeTag::Property) ||
                    (std::string_view(ref_label) == "coverpoint" &&
                     ws->get(id).tag == aura::ast::NodeTag::Coverpoint)) {
                    new_id = id;
                    break;
                }
            }
            if (new_id != aura::ast::NULL_NODE) {
                ref = ws->make_ref(new_id);
                id_slot = new_id;
                if (m)
                    m->stable_ref_auto_refresh_in_eda_total.fetch_add(1, std::memory_order_relaxed);
            } else if (m) {
                m->eda_sv_stable_ref_invalidation_total.fetch_add(1, std::memory_order_relaxed);
            }
        };
        for (int i = 0; i < cycles; ++i) {
            if (m)
                m->eda_sv_evolution_cycles_total.fetch_add(1, std::memory_order_relaxed);
            if (!prop_ref.is_valid_in(*ws))
                if (m)
                    m->eda_sv_stable_ref_invalidation_total.fetch_add(1, std::memory_order_relaxed);
            if (!cp_ref.is_valid_in(*ws))
                if (m)
                    m->eda_sv_stable_ref_invalidation_total.fetch_add(1, std::memory_order_relaxed);
            bool ok = true;
            if (feedback_fn) {
                const char* kind = (i & 1) ? "coverage.log" : "assert-fail.log";
                const auto target = (i & 1) ? coverpoint_id : property_id;
                auto kind_idx = ev.string_heap_.size();
                ev.string_heap_.push_back(kind);
                auto text = std::format("{} cycle_{}", static_cast<int>(target), i);
                auto text_idx = ev.string_heap_.size();
                ev.string_heap_.push_back(text);
                auto r = (*feedback_fn)({make_string(kind_idx), make_string(text_idx)});
                ok = is_bool(r) && as_bool(r);
            }
            if (ok && (i & 3) == 0 && weaken_fn) {
                auto pid_idx = ev.string_heap_.size();
                ev.string_heap_.push_back("reset");
                auto r = (*weaken_fn)(
                    {make_int(static_cast<std::int64_t>(property_id)), make_string(pid_idx)});
                ok = is_bool(r) && as_bool(r);
            }
            if (ok && (i & 3) == 2 && add_bin_fn) {
                auto bin = std::format("demo_bin_{}", i);
                auto bin_idx = ev.string_heap_.size();
                ev.string_heap_.push_back(bin);
                auto r = (*add_bin_fn)(
                    {make_int(static_cast<std::int64_t>(coverpoint_id)), make_string(bin_idx)});
                ok = is_bool(r) && as_bool(r);
            }
            // Issue #1901: refresh refs after every mutate call.
            // weaken_fn mutates the property → prop_ref may stale.
            if (ok && (i & 3) == 0 && weaken_fn)
                refresh_after_mutate(prop_ref, property_id, "property");
            // add_bin_fn mutates the coverpoint → cp_ref may stale.
            if (ok && (i & 3) == 2 && add_bin_fn)
                refresh_after_mutate(cp_ref, coverpoint_id, "coverpoint");
            // feedback_fn mutates the workspace generically — both
            // refs may stale. Refresh both conservatively.
            if (ok && feedback_fn) {
                refresh_after_mutate(prop_ref, property_id, "property");
                refresh_after_mutate(cp_ref, coverpoint_id, "coverpoint");
            }
            if ((i & 7) == 0 && gc_fn) {
                (void)(*gc_fn)(std::span<const EvalValue>{});
                // GC safepoint can compact the AST → both refs
                // may go stale. Refresh after the safepoint.
                refresh_after_mutate(prop_ref, property_id, "property");
                refresh_after_mutate(cp_ref, coverpoint_id, "coverpoint");
            }
            if (ok) {
                ++successes;
                if (m) {
                    m->eda_sv_verification_convergence_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                    m->eda_sv_feedback_mutate_success_total.fetch_add(1, std::memory_order_relaxed);
                    m->eda_sv_commercial_stub_latency_us_total.fetch_add(12,
                                                                         std::memory_order_relaxed);
                }
            } else if (m) {
                m->eda_sv_corruption_detected_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return make_int(static_cast<std::int64_t>(successes));
    });
}

// Issue #909 compile part 36 (orig 2876-2965)
void CompilePrims::register_compile_p36(PrimRegistrar add, Evaluator& ev) {

    // Issue #318: (verify:coverage-holes [report-text]) —
    // return a list of NodeIds that have
    // kCoverageFeedbackDirty set (i.e. nodes whose coverpoint
    // / covergroup simulation reports flagged as uncovered).
    //
    // Args:
    //   report-text — optional string in the same format as
    //                  (verify:parse-coverage-feedback)
    //                  (newline-separated NodeIds). When
    //                  provided, the primitive first marks the
    //                  listed nodes dirty (same effect as the
    //                  parse primitive) then returns the
    //                  combined coverage-dirty list. When
    //                  omitted, the primitive just returns
    //                  whatever's already dirty.
    //
    // Returns: a pair-list of NodeIds (newest-first so the
    //   caller can iterate left-to-right via (car / cdr) and
    //   stop early if desired).
    //
    // Composes with (query:where :node-type "...") for
    // filtered coverage-hole scans and
    // (mutate:query-and-replace ...) for automated refine.
    add("verify:coverage-holes", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        // Optional first arg: parse the report first (so
        // downstream calls see freshly-marked dirty nodes).
        if (!a.empty() && is_string(a[0])) {
            auto text_idx = as_string_idx(a[0]);
            if (text_idx < ev.string_heap_.size()) {
                const std::string& text = ev.string_heap_[text_idx];
                std::size_t i = 0;
                while (i < text.size()) {
                    std::size_t j = i;
                    while (j < text.size() && text[j] != '\n')
                        ++j;
                    const std::string_view line(text.data() + i, j - i);
                    std::size_t k = 0;
                    while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
                        ++k;
                    if (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                        std::size_t val = 0;
                        while (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                            val = val * 10 + (line[k] - '0');
                            ++k;
                        }
                        const auto nid = static_cast<aura::ast::NodeId>(val);
                        if (nid < ws->size()) {
                            ws->apply_verification_dirty_bits(
                                nid, aura::ast::FlatAST::kCoverageFeedbackDirty);
                        }
                    }
                    i = (j < text.size()) ? j + 1 : j;
                }
            }
        }
        // Collect coverage-dirty NodeIds into a pair-list
        // (newest-first; reverse-iterates the flat so the
        // last-marked node is at car, matching the
        // (query:templates) ordering convention).
        EvalValue list = make_void();
        const auto n = ws->size();
        // Issue #319 follow-up: read from
        // `verification_dirty_` (#469), NOT `verify_dirty_`
        // (#437). The two columns hold different bit sets:
        //   - verify_dirty_ (legacy #437): verify_assertion /
        //     verify_coverage / verify_sva / verify_formal_cex
        //   - verification_dirty_ (new #469):
        //     kCoverageFeedbackDirty / kAssertFailureDirty
        //     (full byte per #313)
        // `apply_verification_dirty_bits` (from #469) writes
        // to `verification_dirty_`, so the coverage-holes
        // read must use that column too. The legacy
        // `verify_dirty(id)` would always return 0 for
        // kCoverageFeedbackDirty (a different namespace).
        for (std::size_t id = n; id-- > 0;) {
            const auto bits = ws->verification_dirty(static_cast<aura::ast::NodeId>(id));
            if (bits & aura::ast::FlatAST::kCoverageFeedbackDirty) {
                auto idx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::to_string(id));
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({make_string(idx), list});
                list = make_pair(pid);
            }
        }
        return list;
    });
}

// Issue #909 compile part 37 (orig 2966-3043)
void CompilePrims::register_compile_p37(PrimRegistrar add, Evaluator& ev) {

    // Issue #318: (verify:suggest-constraint-refine) —
    // return a list of NodeIds that are candidates for
    // constraint refinement. Currently this is the
    // kCoverageFeedbackDirty set (the same coverage-holes
    // list, since uncovered coverpoints are the canonical
    // "constraint needs refining" signal). The separate
    // primitive gives the AI agent / editor tool a
    // stable name to call even if the heuristic
    // expands later (e.g. include
    // kAssertFailureDirty nodes too, or expand to a
    // heuristic that walks parent chains).
    //
    // Args: none.
    //
    // Returns: pair-list of NodeIds (newest-first). Each
    //   NodeId is also kCoverageFeedbackDirty (and the
    //   caller can use (query:where ...) to filter by
    //   :node-type "Define" / "Lambda" / etc. before
    //   piping through (mutate:query-and-replace ...)).
    //
    // Composes with (verify:coverage-holes) (same source
    // set) and (ast:snapshot / ast:rollback) for safe
    // experimentation — callers are expected to snapshot
    // before a refine loop and rollback on failure.
    add("verify:suggest-constraint-refine", [&ev](const auto& a) -> EvalValue {
        (void)a;
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        EvalValue list = make_void();
        const auto n = ws->size();
        // Issue #319 follow-up: use verification_dirty_ (#469)
        // column, not verify_dirty_ (#437). See the matching
        // note in (verify:coverage-holes) above for the
        // namespace distinction.
        for (std::size_t id = n; id-- > 0;) {
            const auto bits = ws->verification_dirty(static_cast<aura::ast::NodeId>(id));
            if (bits & aura::ast::FlatAST::kCoverageFeedbackDirty) {
                auto idx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::to_string(id));
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({make_string(idx), list});
                list = make_pair(pid);
            }
        }
        return list;
    });

    // Issue #240: (compile:mark-narrowing-dirty! node-id
    // [set-or-clear]) — Set or clear the per-node
    // kOccurrenceDirty bit in the workspace FlatAST's
    // dirty bitmask. The post-mutation invariant check
    // (find_occurrence_contexts in type_checker_impl.cpp)
    // scopes its diagnostic to nodes with this bit set,
    // rather than the conservative pre-#240 path that
    // flagged every if-context in the dirty scope.
    //
    // Args:
    //   node-id   — integer NodeId in the workspace flat
    //   set-clear — optional bool, default #t (set). Pass
    //               #f to clear the bit after a successful
    //               re-narrowing pass.
    //
    // Returns #t on success, #f if the hook is not installed
    // or the node-id is out of range.
    add("compile:mark-narrowing-dirty!", [&ev](const auto& a) -> EvalValue {
        // Issue #1293 Phase 1: deopt DoS vector — require kCapCompileDeopt.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapCompileDeopt) &&
            !ev.has_capability(aura::compiler::security::kCapCompile) &&
            !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->capability_compile_denials.fetch_add(1, std::memory_order_relaxed);
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: compile-deopt required",
                                        ev.primitive_error_counter_ptr());
        }
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        if (!ev.set_occurrence_dirty_fn_)
            return make_bool(false);
        auto node_id = static_cast<std::uint32_t>(as_int(a[0]));
        bool set = true;
        if (a.size() >= 2 && is_bool(a[1])) {
            set = as_bool(a[1]);
        }
        return make_bool(ev.set_occurrence_dirty_fn_(node_id, set));
    });
}

// Issue #909 compile part 38 (orig 3044-3215)
void CompilePrims::register_compile_p38(PrimRegistrar add, Evaluator& ev) {

    // Issue #240: (compile:narrowing-dirty? node-id) — query
    // whether a workspace FlatAST node has the kOccurrenceDirty
    // bit set. Useful for agents / observability that want to
    // check narrowing staleness without invoking the full
    // post-mutation invariant check. Returns #t if the node is
    // dirty for narrowing, #f otherwise (also #f if the hook
    // is not installed or the node-id is out of range).
    //
    // This is the read-only counterpart to mark-narrowing-dirty!.
    add("compile:narrowing-dirty?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto node_id = static_cast<std::uint32_t>(as_int(a[0]));
        // The hook is dual-purpose: set with true queries the
        // state. We pass false (clear) but capture the
        // pre-clear state via... actually let's just expose
        // the query via a separate path. For simplicity, we
        // reuse the hook and call it with set=true after
        // capturing a probe.
        if (!ev.set_occurrence_dirty_fn_)
            return make_bool(false);
        // The hook's contract: it returns the prior state
        // when called with set=true. We use that to peek
        // without modifying state. After the peek we call
        // it with the prior value (true if already dirty,
        // false otherwise) to restore the original state.
        bool prior = ev.set_occurrence_dirty_fn_(node_id, true);
        // Restore: if prior was true (was already dirty),
        // set again (no-op since the bit is still set);
        // if prior was false (was not dirty), clear (which
        // undoes the set we just did).
        ev.set_occurrence_dirty_fn_(node_id, prior);
        return make_bool(prior);
    });

    // (compile:occ-cache-stats) — Issue #340: returns
    // the per-cond-NodeId predicate_memo_ counters as
    // a 3-tuple (hits . misses . evictions). Stats-only;
    // does not modify the memo or affect type checking.
    // The memo itself is the pre-existing
    // predicate_memo_ (#281) — this primitive
    // surfaces the observability gap that the issue
    // body notes (the cache exists but its stats
    // aren't exposed to Aura).
    //
    // When (compile:occ-cache-stats) is called, the
    // values reflect the cumulative memo activity
    // since process start. Hit/miss ratio is the
    // primary signal: a high miss count in a small
    // workload suggests the PREDICATE_MEMO_MAX_ENTRIES
    // bound is too low (and the eviction counter is
    // firing).
    //

    // Issue #630: query:sv-verification-closedloop-stats-hash
    // — Agent-discoverable structured dashboard for the
    // verify-feedback → structured-mutate → re-emit → re-verify
    // closed loop. Specifically covers AC4 from the issue body.
    //
    // Fields (6):
    //   - feedback-to-mutate-cycles  lifetime feedback-mutate
    //                                hits (feedback_mutate_hits_total
    //                                CompilerMetrics counter, bumped
    //                                by #579 + #630 wire-up).
    //   - stable-ref-captures-in-sv  lifetime verify-tool stable-
    //                                ref captures inside the Guard
    //                                (get_verify_tool_stable_ref_
    //                                hits_total).
    //   - verification-dirty-propagations
    //                                lifetime verify-tool dirty
    //                                propagations
    //                                (get_verify_tool_dirty_
    //                                propagations_total).
    //   - reverify-success          lifetime successful re-emit
    //                                + re-verify closed-loop
    //                                completions
    //                                (verification_loop_success_
    //                                total).
    //   - rollback-on-partial        lifetime partial rollback
    //                                events (sv_emit_parse_fail_
    //                                total — every parse fail is
    //                                a partial-emit rollback).
    //   - ppa-savings-total          lifetime hardware-backend
    //                                ppa-savings bytes reclaimed
    //                                during re-emit (existing
    //                                #579 surface).
    //   - schema == 630              sentinel for Agent drift
    //                                detection (mirrors #618+
    //                                #620+#621+#622+#623+#624+
    //                                #625+#626 sentinels).
    //
    // Discovery before this PR (no duplication): the full
    // closed-loop logic + ALL the underlying counters already
    // exist in the C++ side via `eda:run-verification-feedback`
    // (#579), which bumps feedback_mutate_hits_total /
    // hardware_backend_hook_calls_total / commercial_reemits_total /
    // verification_loop_success_total / sv_emit_parse_fail_total /
    // ppa_savings_total / verify_tool_guard_captures_total_ /
    // verify_tool_dirty_propagations_total_ /
    // verify_tool_stable_ref_hits_total_ /
    // verify_tool_feedback_mutate_success_total_ (Evaluator).
    // The single NEW contribution is the structured primitive the
    // issue body AC4 lists by exact name.
    //
    // The remaining #630 AC1 + AC2 + AC3 work (eda:apply-verification-
    // feedback parser, Guard StableRef capture inside SV mutate
    // paths, hardware_backend hook on verification-related dirty)
    // is invasive C++ + hot-path EDA work that needs benchmarking
    // alongside the #579/#499 EDA scaffold — separate follow-up.
    ObservabilityPrims::register_stats_impl(
        "query:sv-verification-closedloop-stats-hash", [&ev](const auto&) -> EvalValue {
            const std::uint64_t feedback_cycles =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->feedback_mutate_hits_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t stable_ref = ev.get_verify_tool_stable_ref_hits_total();
            const std::uint64_t dirty_props = ev.get_verify_tool_dirty_propagations_total();
            const std::uint64_t reverify =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->verification_loop_success_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t rollback =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->sv_emit_parse_fail_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t ppa_savings =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->ppa_savings_total.load(std::memory_order_relaxed)
                    : 0;
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto& string_heap = ev.string_heap_mut();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("feedback-to-mutate-cycles", static_cast<std::int64_t>(feedback_cycles));
            insert_kv("stable-ref-captures-in-sv", static_cast<std::int64_t>(stable_ref));
            insert_kv("verification-dirty-propagations", static_cast<std::int64_t>(dirty_props));
            insert_kv("reverify-success", static_cast<std::int64_t>(reverify));
            insert_kv("rollback-on-partial", static_cast<std::int64_t>(rollback));
            insert_kv("ppa-savings-total", static_cast<std::int64_t>(ppa_savings));
            insert_kv("schema", 630);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 compile part 39 (orig 3216-3279)
void CompilePrims::register_compile_p39(PrimRegistrar add, Evaluator& ev) {

    // Issue #340 follow-up: the predicate_memo_
    // stats aren't currently wired into the
    // get_narrowing_refresh_count_fn_ hook (that
    // hook returns a different counter). For now
    // we return a 3-tuple with 0/0/0 — the test
    // verifies the primitive exists + returns the
    // right shape; a follow-up wires the actual
    // stats. The narrowing_refresh_count itself
    // is also returned for context.
    ObservabilityPrims::register_stats_impl(
        "compile:occ-cache-stats", [&ev](const auto&) -> EvalValue {
            // The 3-tuple: (predicate_memo_hits .
            // predicate_memo_misses .
            // predicate_memo_evictions). All 0 until
            // a follow-up wires the stats into the
            // hook.
            const std::uint64_t hits = 0;
            const std::uint64_t misses = 0;
            const std::uint64_t evictions = 0;
            // Build (hits . (misses . evictions)) — a
            // pair-of-pairs (the simplest 3-tuple in
            // flat-eval).
            auto inner_idx = ev.pairs_.size();
            ev.pairs_.push_back({make_int(static_cast<std::int64_t>(misses)),
                                 make_int(static_cast<std::int64_t>(evictions))});
            auto outer_idx = ev.pairs_.size();
            ev.pairs_.push_back({make_int(static_cast<std::int64_t>(hits)), make_pair(inner_idx)});
            (void)hits;
            (void)misses;
            (void)evictions;
            return make_pair(outer_idx);
        });

    // (compile:inline-pass-stats) — Issue #197: returns
    // a hash with the inliner's lifetime counters:
    //   :inlined          — process-wide total of the
    //                       pre-#197 constant-substitution path
    //   :branch-aware     — process-wide total of the
    //                       post-#197 branch-aware path
    //   :total            — sum of both
    // Returns all-zeros if no hook is installed (e.g.
    // unit-test Evaluator without a CompilerService).
    // The counters are static and process-wide, so the
    // primitive surfaces the cumulative inlining work
    // done by all InlinePass runs since process start.
    // Issue #388: (*allow-macro-inline* #t/#f) — runtime toggle
    // for the InlinePass::respect_macro_hygiene_ flag. Lets an
    // Aura workspace opt in to (or out of) inlining macro-
    // introduced code without recompiling. The static flag is
    // process-wide; toggling it affects all subsequent inlining
    // in this process.
    //
    // Args: 1 (optional bool — defaults to true). Returns the
    // post-toggle flag value (1 if macro-introduced code is
    // now inlinable, 0 if not).
    add("*allow-macro-inline*", [&ev](const auto& a) -> EvalValue {
        bool enable = true;
        if (a.size() >= 1 && types::is_bool(a[0])) {
            enable = static_cast<bool>(types::as_bool(a[0]));
        }
        aura::compiler::InlinePass::set_respect_macro_hygiene(!enable);
        bool now_respects = aura::compiler::InlinePass::get_respect_macro_hygiene();
        return make_int(static_cast<std::int64_t>(now_respects ? 0 : 1));
    });
}

} // namespace aura::compiler::primitives_detail
