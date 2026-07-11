// evaluator_primitives_obs_eval_09.cpp — Issue #909: peeled domain registration from observability
// monolith aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "observability_metrics.h"
#include "compiler/shape.h"
#include "compiler/value_tags.h"
#include "core/cpp26_contract_stats.h"
#include "core/arena_auto_policy_stats.h"
#include "jit_typed_mutation_stats.h"
#include "shape_jit_pass_closedloop_stats.h"
#include "ci_build_info.h"
#include "primitives_meta.h"
#include "primitives_detail.h"
#include "serve/metrics.h"
#include "hash_meta.h"
#include "basis_points.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;
import aura.compiler.pass_manager;

extern "C" std::uint64_t aura_fiber_static_steal_outermost_mutation_boundary_total();
extern "C" std::uint64_t aura_fiber_static_steal_inner_mutation_boundary_deferred_total();
extern "C" std::uint64_t aura_fiber_static_cross_fiber_mutation_safe_steal_total();
extern "C" std::uint64_t aura_fiber_init_aura_result_ok_total();
extern "C" std::uint64_t aura_fiber_init_aura_result_err_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_ok_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_err_total();
extern "C" std::uint64_t aura_jit_guest_exception_bridge_total();

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

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

// Issue #909 part 72 (orig lines 8582-8661)
void ObservabilityPrims::register_eval_p72(PrimRegistrar add, Evaluator& ev) {

    add("query:primitives-hotpath-slo-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t call_total = 0;
        std::uint64_t pair_total = 0;
        std::uint64_t fastpath_hits = 0;
        std::uint64_t depth_max = 0;
        std::uint64_t contract_viol = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            call_total = m->primitive_call_total.load(std::memory_order_relaxed);
            pair_total = m->pair_alloc_total.load(std::memory_order_relaxed);
            fastpath_hits = m->primitive_fastpath_hits_total.load(std::memory_order_relaxed);
            depth_max = m->cdr_depth_max.load(std::memory_order_relaxed);
            contract_viol = m->primitive_capture_violations_total.load(std::memory_order_relaxed);
        }
        // Reuse the #614 stability-score formula: alloc_per_call
        // (integer division) + cdr_depth penalty capped at < 50
        // before regression flag. Same computation, exposed as a
        // 0-10000 fixed-point pct via × 100.
        const std::int64_t alloc_per_call =
            static_cast<std::int64_t>(pair_total / (call_total + 1));
        const std::int64_t stability_penalty =
            static_cast<std::int64_t>(alloc_per_call * 3 + (depth_max > 32 ? depth_max / 8 : 0));
        const std::int64_t stability_score = stability_penalty >= 100 ? 0 : 100 - stability_penalty;
        // current-vs-baseline-pct: stability_score × 100 = 0-10000
        // fixed-point percent. 10000 = 100.00% baseline (no load,
        // no regression). The body SLO target is "no regression
        // >5%" which maps to current-vs-baseline-pct >= 9500
        // (i.e., stability_score >= 95).
        const std::int64_t current_vs_baseline_pct = stability_score * 100;
        // fastpath-hit-rate-pct: 10000 baseline when call_total == 0
        // (vacuously true, mirror #774 convergence_rate). Otherwise
        // compute (fastpath_hits / (call_total + 1)) × 10000.
        // The +1 in the denominator avoids divide-by-zero AND
        // matches the #614 alloc_per_call formula.
        std::int64_t fastpath_hit_rate_pct = 10000; // 100.00% baseline
        if (call_total > 0) {
            fastpath_hit_rate_pct = static_cast<std::int64_t>(
                (fastpath_hits * ::aura::compiler::kBasisPointScale) / (call_total + 1));
        }
        // regression-flag: 1 if current-vs-baseline-pct < 5000
        // (= stability_score < 50, the #614 "regression" threshold
        // that recommends action 3). Otherwise 0.
        const std::int64_t regression_flag = current_vs_baseline_pct < 5000 ? 1 : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("current-vs-baseline-pct", current_vs_baseline_pct);
        insert_kv("contract-violations", static_cast<std::int64_t>(contract_viol));
        insert_kv("fastpath-hit-rate-pct", fastpath_hit_rate_pct);
        insert_kv("regression-flag", regression_flag);
        insert_kv("schema", 776);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 73 (orig lines 8662-8828)
void ObservabilityPrims::register_eval_p73(PrimRegistrar add, Evaluator& ev) {

    // Issue #777: query:eda-production-readiness — Consolidated
    // EDA Infrastructure Primitives Production Readiness Roadmap
    // + Milestone Tracker with Measurable Fidelity/SLO Gates
    // observability dashboard (P0 EDA stdlib production-readiness
    // surface; refines/consolidates #726/#748/#772/#774/#749/#738
    // /#725/#724; non-duplicative with any individual EDA primitive
    // — #777 is the FIRST observability surface that aggregates the
    // *milestone-completeness composite* across the 4 EDA
    // production roadmap milestones as a single deployment-grade
    // production-readiness dashboard the Agent reads to decide
    // whether the EDA stdlib is production-ready for commercial
    // verification self-evolution).
    //
    // Milestone definitions (from body):
    //   M1: basic feedback primitives + emit
    //   M2: full SV EDSL + dirty re-emit
    //   M3: commercial fidelity + roundtrip + long-running harness
    //   M4: multi-agent concurrent SLOs
    //
    // For each milestone, the primitive looks up the expected
    // observability/runtime primitives via ev.primitives_.lookup
    // and computes completeness = (found / total_expected) × 10000
    // (0-10000 fixed-point percent × 100; 10000 = 100% = all
    // expected primitives registered).
    //
    // Fields (6 + sentinel):
    //   - m1-completeness-pct  M1: 5 expected primitives
    //                          (primitive:generate-skeleton +
    //                           verify:parse-coverage-feedback +
    //                           verify:parse-assert-failure +
    //                           verify:parse-formal-cex +
    //                           mutate:from-verification-feedback)
    //   - m2-completeness-pct  M2: 4 expected primitives
    //                          (query:sv-verification-structure-
    //                           stats + query:sv-commercial-emit-
    //                           fidelity-stats + query:sv-
    //                           verification-self-evolution-stats +
    //                           query:sv-closedloop-slo)
    //   - m3-completeness-pct  M3: 3 expected primitives
    //                          (query:primitives-hotpath-slo-stats
    //                           + compile:inline-pass-stats +
    //                           compile:dead-coercion-stats)
    //   - m4-completeness-pct  M4: 2 expected primitives
    //                          (query:workspace-closedloop-
    //                           orchestration-stats + query:
    //                           workspace-closedloop-fiber-eda-
    //                           stats)
    //   - blocking-issues-count fixed count of related open
    //                          EDA issues (#749, #738, #725, #724,
    //                          #726, #748, #772, #774 per body —
    //                          closed ones: #726, #748, #772, #774 =
    //                          4 closed; remaining open: #749,
    //                          #738, #725, #724 = 4 open)
    //   - recommendation       0=production-ready (all milestones
    //                          >= 9500), 1=near-ready (all >= 8000),
    //                          2=in-progress (all >= 5000),
    //                          3=early-stage (any < 5000)
    //   - schema == 777
    //
    // Phase 1 ships the primitive + milestone aggregation.
    // The actual milestone table maintenance + CI gate wiring +
    // SEVA demo + contributing guide updates + per-issue link
    // tracking + notification on milestone completion are all
    // follow-up work (each is a dedicated session in the issue
    // tracker + contributing.md + CI pipeline + SEVA demo +
    // primitives_style.md).
    add("query:eda-production-readiness", [&ev](const auto&) -> EvalValue {
        // Helper: count how many expected primitives are registered.
        // Returns found_count + computes completeness as
        // (found * ::aura::compiler::kBasisPointScale) / total via integer division.
        auto milestone_pct = [&](std::initializer_list<const char*> expected) -> std::int64_t {
            std::size_t total = expected.size();
            if (total == 0)
                return 10000; // vacuously true (100.00% baseline)
            std::size_t found = 0;
            for (const char* name : expected) {
                if (ev.primitives_.lookup(name).has_value())
                    ++found;
            }
            return static_cast<std::int64_t>((found * ::aura::compiler::kBasisPointScale) / total);
        };
        // M1: basic feedback primitives + emit (5 expected)
        const std::int64_t m1_pct = milestone_pct({
            "primitive:generate-skeleton",
            "verify:parse-coverage-feedback",
            "verify:parse-assert-failure",
            "verify:parse-formal-cex",
            "mutate:from-verification-feedback",
        });
        // M2: full SV EDSL + dirty re-emit (4 expected)
        const std::int64_t m2_pct = milestone_pct({
            "query:sv-verification-structure-stats",
            "query:sv-commercial-emit-fidelity-stats",
            "query:sv-verification-self-evolution-stats",
            "query:sv-closedloop-slo",
        });
        // M3: commercial fidelity + roundtrip + long-running
        // harness (3 expected primitives; long-running harness is
        // not a primitive but a test fixture, so we use 3 as a
        // representative observability surface for this milestone).
        const std::int64_t m3_pct = milestone_pct({
            "query:primitives-hotpath-slo-stats",
            "compile:inline-pass-stats",
            "compile:dead-coercion-stats",
        });
        // M4: multi-agent concurrent SLOs (2 expected)
        const std::int64_t m4_pct = milestone_pct({
            "query:workspace-closedloop-orchestration-stats",
            "query:workspace-closedloop-fiber-eda-stats",
        });
        // Blocking-issues-count: fixed value based on body list of
        // related EDA issues. Closed: #726, #748, #772, #774 = 4.
        // Open (per body list at issue creation): #749, #738,
        // #725, #724 = 4. The number is hardcoded for now; future
        // work could pull from GitHub API or issue tracker at
        // primitive-call time.
        const std::int64_t blocking_issues_count = 4;
        // Recommendation: based on minimum milestone completeness.
        const std::int64_t min_pct = std::min({m1_pct, m2_pct, m3_pct, m4_pct});
        std::int64_t recommendation = 3;
        if (min_pct >= 9500)
            recommendation = 0; // production-ready
        else if (min_pct >= 8000)
            recommendation = 1; // near-ready
        else if (min_pct >= 5000)
            recommendation = 2; // in-progress
        else
            recommendation = 3; // early-stage
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("m1-completeness-pct", m1_pct);
        insert_kv("m2-completeness-pct", m2_pct);
        insert_kv("m3-completeness-pct", m3_pct);
        insert_kv("m4-completeness-pct", m4_pct);
        insert_kv("blocking-issues-count", blocking_issues_count);
        insert_kv("recommendation", recommendation);
        insert_kv("schema", 777);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 74 (orig lines 8829-8932)
void ObservabilityPrims::register_eval_p74(PrimRegistrar add, Evaluator& ev) {

    // Issue #778: query:ffi-call-overhead-stats — FFI
    // call overhead observability for batch terminal
    // output + rendering engine hot-path (P1 perf
    // surface; non-duplicative with #131 FFI primitive
    // extraction, #699 query:ffi-calls-stats). #778 is
    // the FIRST observability surface that tracks the FFI
    // call volume at the primitive-call layer (c-load +
    // c-func + c-opaque + c-alloc + c-struct-set! +
    // c-struct-ref — all of which increment
    // coverage_counters_[8]) + exposes the production-
    // readiness signals for the deferred batch FFI
    // primitive + (terminal-batch-write) work the body
    // asks for. The actual ns/op measurement is in
    // test_issue_778.cpp as a benchmark (the production
    // wiring is deferred — see body Phase 2+).
    //
    // Fields (4 + sentinel):
    //   - ffi-call-count         read from
    //                              ev.get_ffi_call_count() =
    //                              coverage_counters_[8] (total FFI
    //                              primitive invocations; bumps
    //                              monotonically over the
    //                              Evaluator's lifetime)
    //   - batch-ffi-supported    fixed 0 (the batch FFI
    //                              primitive is Phase 2+ deferred
    //                              per body "Add batch FFI
    //                              primitive or memory view
    //                              support in
    //                              ffi_primitives_impl.cpp")
    //   - terminal-batch-write-supported
    //                            fixed 0 (the terminal-batch-
    //                              write primitive is Phase 2+
    //                              deferred per body "Provide
    //                              terminal-batch-write or
    //                              similar high-level primitive
    //                              that minimizes crossings")
    //   - recommendation         0=production-ready (both
    //                              batch-ffi-supported and
    //                              terminal-batch-write-
    //                              supported = 1), 1=partial
    //                              (one = 1, other = 0),
    //                              2=missing-primitive (both
    //                              = 0 but ffi-call-count > 0
    //                              means Agent is using FFI),
    //                              3=early-stage (both = 0
    //                              and no FFI usage yet)
    //   - schema == 778
    add("query:ffi-call-overhead-stats", [&ev](const auto&) -> EvalValue {
        const std::int64_t ffi_call_count = static_cast<std::int64_t>(ev.get_ffi_call_count());
        // Hardcoded flags for the deferred batch-FFI primitives.
        // When the actual batch FFI primitive + terminal-batch-
        // write primitive ship (Phase 2+ per body), these will
        // be derived from a primitive existence check (mirror
        // #777's live lookup pattern).
        const std::int64_t batch_ffi_supported = 0;
        const std::int64_t terminal_batch_write_supported = 0;
        // Recommendation: derived from the 2 supported flags +
        // FFI usage signal.
        std::int64_t recommendation = 3;
        if (batch_ffi_supported == 1 && terminal_batch_write_supported == 1)
            recommendation = 0; // production-ready
        else if (batch_ffi_supported == 1 || terminal_batch_write_supported == 1)
            recommendation = 1; // partial
        else if (ffi_call_count > 0)
            recommendation = 2; // missing-primitive (Agent is using FFI)
        else
            recommendation = 3; // early-stage (no FFI usage yet)
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("ffi-call-count", ffi_call_count);
        insert_kv("batch-ffi-supported", batch_ffi_supported);
        insert_kv("terminal-batch-write-supported", terminal_batch_write_supported);
        insert_kv("recommendation", recommendation);
        insert_kv("schema", 778);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 75 (orig lines 8933-9033)
void ObservabilityPrims::register_eval_p75(PrimRegistrar add, Evaluator& ev) {

    // Issue #779: query:dirty-region-rendering-stats — Dirty
    // region / delta rendering observability for terminal
    // rendering engine (P2 perf surface; non-duplicative with
    // the existing vector primitives in
    // evaluator_primitives_vector.cpp). #779 is the FIRST
    // observability surface that exposes the
    // production-readiness signals for the deferred dirty
    // region / delta rendering work the body asks for
    // (terminal-dirty-region tracking + present-delta
    // efficient output). The actual primitives are
    // Phase 2+ deferred — when they ship, the 2 hardcoded
    // "not yet supported" flags flip to 1 via the live
    // primitive lookup pattern (mirror #777).
    //
    // Fields (4 + sentinel):
    //   - dirty-region-count        hardcoded 0 (no existing
    //                                counter for dirty regions
    //                                on main; would be bumped by
    //                                the (terminal-dirty-region)
    //                                primitive when it ships)
    //   - present-delta-supported   hardcoded 0 (the
    //                                (present-delta) primitive
    //                                is Phase 2+ deferred per
    //                                body "Implement efficient
    //                                present-delta that only
    //                                outputs changed areas")
    //   - terminal-dirty-region-supported
    //                              hardcoded 0 (the
    //                                (terminal-dirty-region)
    //                                primitive is Phase 2+
    //                                deferred per body "Add
    //                                terminal-dirty-region
    //                                tracking primitives")
    //   - recommendation            0=production-ready (both
    //                                supported flags = 1),
    //                                1=partial (one = 1),
    //                                2=missing-primitive (both
    //                                = 0 but dirty-region-
    //                                count > 0 indicates
    //                                rendering activity),
    //                                3=early-stage (both = 0
    //                                AND no dirty region
    //                                activity)
    //   - schema == 779
    add("query:dirty-region-rendering-stats", [&ev](const auto&) -> EvalValue {
        // No existing counter for dirty regions on main; the
        // (terminal-dirty-region) primitive + the dirty-region
        // counter will be added when Phase 2 ships.
        const std::int64_t dirty_region_count = 0;
        // Hardcoded flags for the deferred primitives (mirror
        // #778 batch-ffi-supported pattern).
        const std::int64_t present_delta_supported = 0;
        const std::int64_t terminal_dirty_region_supported = 0;
        // Recommendation: derived from the 2 supported flags +
        // dirty-region-count signal.
        std::int64_t recommendation = 3;
        if (present_delta_supported == 1 && terminal_dirty_region_supported == 1)
            recommendation = 0; // production-ready
        else if (present_delta_supported == 1 || terminal_dirty_region_supported == 1)
            recommendation = 1; // partial
        else if (dirty_region_count > 0)
            recommendation = 2; // missing-primitive (rendering active)
        else
            recommendation = 3; // early-stage (no rendering yet)
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("dirty-region-count", dirty_region_count);
        insert_kv("present-delta-supported", present_delta_supported);
        insert_kv("terminal-dirty-region-supported", terminal_dirty_region_supported);
        insert_kv("recommendation", recommendation);
        insert_kv("schema", 779);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 76 (orig lines 9034-9154)
void ObservabilityPrims::register_eval_p76(PrimRegistrar add, Evaluator& ev) {

    // Issue #780: query:jit-rendering-coverage-stats — JIT
    // / hot-update coverage observability for rendering hot
    // paths (P2 perf surface; non-duplicative with the
    // existing (query:jit-stats) #427, (query:jit-consistency-
    // stats), (query:jit-interpreter-parity-stats) #720, and
    // (query:jit-typed-mutation-stats) #746). #780 is the
    // FIRST observability surface that tracks the JIT
    // coverage for the rendering hot paths the body asks
    // for (present() + drawing loops in I/O-heavy
    // rendering) + exposes the production-readiness
    // signals for the deferred rendering-path JIT + hot-
    // update optimization work the body asks for.
    //
    // Fields (4 + sentinel):
    //   - hotpath-eval-flat-calls  reused #441 atomic
    //                              (hotpath_eval_flat_calls)
    //                              — total JIT path eval-flat
    //                              invocations (the JIT hot
    //                              path the body says is NOT
    //                              covering rendering)
    //   - hotpath-lowering-calls   reused (hotpath_lowering
    //                              _calls) — total JIT
    //                              lowering invocations
    //   - rendering-path-jit-supported
    //                              hardcoded 0 (rendering
    //                              path JIT is Phase 2+
    //                              deferred per body
    //                              "present() and drawing
    //                              loops remain in
    //                              interpreted mode or have
    //                              high overhead")
    //   - hot-update-rendering-optimized
    //                              hardcoded 0 (hot-update
    //                              rendering optimization is
    //                              Phase 2+ deferred per
    //                              body "Hot-update works for
    //                              general code but lacks
    //                              special handling for
    //                              performance-critical
    //                              rendering functions")
    //   - recommendation           0=production-ready (both
    //                              optimization flags = 1),
    //                              1=partial (one = 1),
    //                              2=missing-optimization
    //                              (both = 0 but hotpath
    //                              counters > 0 means JIT
    //                              path is being exercised),
    //                              3=early-stage (both = 0
    //                              AND no JIT activity)
    //   - schema == 780
    add("query:jit-rendering-coverage-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = ev.compiler_metrics()
                            ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                            : nullptr;
        // Reused #441 atomics — the JIT hot path counters.
        const std::int64_t hotpath_eval_flat_calls =
            m ? static_cast<std::int64_t>(
                    m->hotpath_eval_flat_calls.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hotpath_lowering_calls =
            m ? static_cast<std::int64_t>(m->hotpath_lowering_calls.load(std::memory_order_relaxed))
              : 0;
        // Hardcoded flags for the deferred rendering-path
        // optimizations. When the actual JIT rendering-path
        // + hot-update rendering optimization ship
        // (Phase 2+ per body), these will be derived from
        // a primitive existence check (mirror #777's live
        // lookup pattern).
        const std::int64_t rendering_path_jit_supported = 0;
        const std::int64_t hot_update_rendering_optimized = 0;
        // Recommendation: derived from the 2 optimization
        // flags + JIT activity signal (sum of both hotpath
        // counters).
        const std::int64_t jit_activity = hotpath_eval_flat_calls + hotpath_lowering_calls;
        std::int64_t recommendation = 3;
        if (rendering_path_jit_supported == 1 && hot_update_rendering_optimized == 1)
            recommendation = 0; // production-ready
        else if (rendering_path_jit_supported == 1 || hot_update_rendering_optimized == 1)
            recommendation = 1; // partial
        else if (jit_activity > 0)
            recommendation = 2; // missing-optimization (JIT active)
        else
            recommendation = 3; // early-stage (no JIT activity)
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("hotpath-eval-flat-calls", hotpath_eval_flat_calls);
        insert_kv("hotpath-lowering-calls", hotpath_lowering_calls);
        insert_kv("rendering-path-jit-supported", rendering_path_jit_supported);
        insert_kv("hot-update-rendering-optimized", hot_update_rendering_optimized);
        insert_kv("recommendation", recommendation);
        insert_kv("schema", 780);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 77 (orig lines 9155-9276)
void ObservabilityPrims::register_eval_p77(PrimRegistrar add, Evaluator& ev) {

    // Issue #781: query:zero-copy-framebuffer-stats — High-
    // performance byte buffer + zero-copy primitives
    // observability for framebuffer management
    // (P2 perf surface; non-duplicative with the existing
    // memory primitives in evaluator_primitives_memory.cpp
    // and vector primitives in evaluator_primitives_vector
    // .cpp). #781 is the FIRST observability surface that
    // tracks the pair allocation pressure that the body
    // says is wasted on per-frame buffer construction
    // (Building output buffers per frame incurs
    // unnecessary allocations and copies) + exposes the
    // production-readiness signals for the deferred
    // zero-copy byte-buffer + ANSI sequence helper +
    // memory profiling work the body asks for.
    //
    // Fields (4 + sentinel):
    //   - pair-alloc-total        reused #491 atomic
    //                              (pair_alloc_total) — total
    //                              pair allocations across
    //                              list / append / reverse /
    //                              map / filter (the allocation
    //                              pressure signal the body
    //                              mentions)
    //   - zero-copy-supported      hardcoded 0 (the
    //                              (zero-copy-view) primitive
    //                              + byte-buffer primitive
    //                              with zero-copy semantics
    //                              is Phase 2+ deferred per
    //                              body "Enhance or add
    //                              specialized byte-buffer
    //                              primitives with zero-copy
    //                              and view support")
    //   - ansi-helper-supported    hardcoded 0 (the
    //                              (ansi-sequence-build) or
    //                              similar helper primitive
    //                              is Phase 2+ deferred per
    //                              body "Provide helpers for
    //                              efficient ANSI sequence
    //                              construction")
    //   - memory-profiling-supported
    //                              hardcoded 0 (the
    //                              rendering memory profiling
    //                              primitive is Phase 2+
    //                              deferred per body "Add
    //                              memory profiling for
    //                              rendering workloads")
    //   - recommendation           0=production-ready (all
    //                              3 support flags = 1),
    //                              1=partial (any 1 or 2 = 1),
    //                              2=missing-primitive (all
    //                              = 0 but pair_alloc_total
    //                              > 0 means memory pressure
    //                              exists), 3=early-stage
    //                              (all = 0 AND no allocation
    //                              activity)
    //   - schema == 781
    add("query:zero-copy-framebuffer-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = ev.compiler_metrics()
                            ? static_cast<const CompilerMetrics*>(ev.compiler_metrics())
                            : nullptr;
        // Reused #491 atomic — the pair allocation counter
        // the body identifies as the "unnecessary allocations
        // and copies" pressure signal.
        const std::int64_t pair_alloc_total =
            m ? static_cast<std::int64_t>(m->pair_alloc_total.load(std::memory_order_relaxed)) : 0;
        // Issues #1178/#1181/#1184 Phase 1: real support flags.
        // zero_copy_output.ixx + batch_terminal ANSI helpers + render
        // memory-profiling metrics are now scaffolded / active.
        const std::int64_t zero_copy_supported =
            m ? static_cast<std::int64_t>(
                    m->zero_copy_framebuffer_supported.load(std::memory_order_relaxed))
              : 1;
        const std::int64_t ansi_helper_supported =
            m ? static_cast<std::int64_t>(m->ansi_helper_supported.load(std::memory_order_relaxed))
              : 1;
        const std::int64_t memory_profiling_supported =
            m ? static_cast<std::int64_t>(
                    m->render_memory_profiling_supported.load(std::memory_order_relaxed))
              : 1;
        // Recommendation: derived from the 3 support flags +
        // pair_alloc_total signal.
        std::int64_t recommendation = 3;
        if (zero_copy_supported == 1 && ansi_helper_supported == 1 &&
            memory_profiling_supported == 1)
            recommendation = 0; // production-ready
        else if (zero_copy_supported == 1 || ansi_helper_supported == 1 ||
                 memory_profiling_supported == 1)
            recommendation = 1; // partial
        else if (pair_alloc_total > 0)
            recommendation = 2; // missing-primitive (allocations happening)
        else
            recommendation = 3; // early-stage (no allocation activity)
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("pair-alloc-total", pair_alloc_total);
        insert_kv("zero-copy-supported", zero_copy_supported);
        insert_kv("ansi-helper-supported", ansi_helper_supported);
        insert_kv("memory-profiling-supported", memory_profiling_supported);
        insert_kv("recommendation", recommendation);
        insert_kv("schema", 781);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 78 (orig lines 9277-9426)
void ObservabilityPrims::register_eval_p78(PrimRegistrar add, Evaluator& ev) {

    // Issue #782: query:terminal-rendering-module-stats —
    // Dedicated terminal rendering primitives module +
    // profiling integration observability (P2
    // infrastructure surface; non-duplicative with the
    // existing vector + memory + I/O primitives in
    // evaluator_primitives_vector.cpp / _memory.cpp /
    // _io.cpp). #782 is the FIRST observability surface
    // that exposes the production-readiness signals for
    // the deferred evaluator_primitives_terminal.cpp
    // module + core rendering primitives (clear,
    // draw-batch, present, dirty tracking) +
    // shape_profiler integration + example terminal
    // renderer the body asks for.
    //
    // Fields (4 + sentinel):
    //   - core-primitive-count    live count of expected
    //                              terminal rendering core
    //                              primitives registered
    //                              (4 expected per body:
    //                              `clear`, `draw-batch`,
    //                              `present`, dirty
    //                              tracking; 0 on fresh
    //                              service because no
    //                              evaluator_primitives
    //                              _terminal.cpp exists
    //                              yet — computed via
    //                              live primitive lookup,
    //                              mirror #777 pattern)
    //   - terminal-module-available
    //                              hardcoded 0 (the
    //                              evaluator_primitives
    //                              _terminal.cpp module
    //                              is Phase 2+ deferred
    //                              per body "no
    //                              evaluator_primitives
    //                              _terminal.cpp or
    //                              equivalent module for
    //                              high-performance
    //                              terminal/character
    //                              graphics rendering")
    //   - shape-profiler-integration-available
    //                              hardcoded 0 (the
    //                              shape_profiler.cpp
    //                              integration for
    //                              rendering paths is
    //                              Phase 2+ deferred per
    //                              body "Integrate with
    //                              existing
    //                              observability and
    //                              shape_profiler.cpp")
    //   - example-renderer-available
    //                              hardcoded 0 (the
    //                              minimal high-perf
    //                              terminal renderer
    //                              example is Phase 2+
    //                              deferred per body
    //                              "Provide example
    //                              implementation of a
    //                              minimal high-perf
    //                              terminal renderer")
    //   - recommendation           0=production-ready
    //                              (terminal-module-
    //                              available = 1 AND
    //                              shape-profiler-
    //                              integration = 1 AND
    //                              example-renderer = 1
    //                              AND core-primitive-
    //                              count = 4),
    //                              1=partial (any of the
    //                              3 module flags = 1 or
    //                              core-primitive-count
    //                              > 0), 2=missing-module
    //                              (all 3 = 0 but
    //                              core-primitive-count
    //                              > 0 = core primitives
    //                              exist without module
    //                              wrapper), 3=early-
    //                              stage (all 3 = 0 AND
    //                              core-primitive-count
    //                              == 0)
    //   - schema == 782
    add("query:terminal-rendering-module-stats", [&ev](const auto&) -> EvalValue {
        // Live primitive lookup: count how many of the
        // expected core rendering primitives are
        // registered. Mirror #777 milestone_pct pattern.
        const std::vector<const char*> expected_core_primitives = {"clear", "draw-batch", "present",
                                                                   "dirty-tracking"};
        std::size_t found_count = 0;
        for (const char* name : expected_core_primitives) {
            if (ev.primitives_.lookup(name).has_value())
                ++found_count;
        }
        const std::int64_t core_primitive_count = static_cast<std::int64_t>(found_count);
        // Hardcoded flags for the deferred module + profiler
        // integration + example renderer (mirror
        // #778-#781 hardcoded "not yet" flag pattern).
        const std::int64_t terminal_module_available = 0;
        const std::int64_t shape_profiler_integration_available = 0;
        const std::int64_t example_renderer_available = 0;
        // Recommendation: derived from the 3 module flags +
        // core-primitive-count signal.
        std::int64_t recommendation = 3;
        if (terminal_module_available == 1 && shape_profiler_integration_available == 1 &&
            example_renderer_available == 1 && core_primitive_count == 4)
            recommendation = 0; // production-ready
        else if (terminal_module_available == 1 || shape_profiler_integration_available == 1 ||
                 example_renderer_available == 1 || core_primitive_count > 0)
            recommendation = 1; // partial
        else if (core_primitive_count > 0)
            recommendation = 2; // missing-module (core primitives exist without module wrapper)
        else
            recommendation = 3; // early-stage (no core primitives, no module)
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("core-primitive-count", core_primitive_count);
        insert_kv("terminal-module-available", terminal_module_available);
        insert_kv("shape-profiler-integration-available", shape_profiler_integration_available);
        insert_kv("example-renderer-available", example_renderer_available);
        insert_kv("recommendation", recommendation);
        insert_kv("schema", 782);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 79 (orig lines 9427-9551)
void ObservabilityPrims::register_eval_p79(PrimRegistrar add, Evaluator& ev) {

    // Issue #783: query:orchestration-steal-outermost-stats —
    // P0 production-grade work-stealing observability for
    // multi-fiber mutation under MutationBoundaryGuard.
    // Refines the coarse steal_deferred_mutation_boundary_count_
    // metric (#451) into "outermost safe steal" + "inner
    // deferred" + "cross-fiber safe steal", and surfaces the
    // Phase 2+ deferred work (strict StableRef refresh on
    // resume + EnvFrame version refresh + #754 bias-driven
    // outermost deferral).
    //
    // Fields (6 + sentinel):
    //   - outermost-steal-total          process-wide lifetime
    //                                    # of successful work-steals
    //                                    at a MutationBoundary point
    //                                    with depth==0 (safe +
    //                                    boundary) — from the
    //                                    new Fiber::static_steal_
    //                                    outermost_mutation_
    //                                    boundary_count_ atomic
    //   - inner-deferred-total           process-wide lifetime
    //                                    # of steal attempts
    //                                    deferred because the
    //                                    victim held an inner
    //                                    MutationBoundary guard
    //                                    (depth>0 — unsafe to
    //                                    move) — from Fiber::
    //                                    static_steal_inner_
    //                                    mutation_boundary_
    //                                    deferred_count_
    //   - cross-fiber-safe-steal-total   process-wide lifetime
    //                                    # of outermost safe
    //                                    steals that crossed
    //                                    between workers — from
    //                                    Fiber::static_cross_
    //                                    fiber_mutation_safe_
    //                                    steal_count_
    //   - strict-stable-ref-refresh     hardcoded 0 (Phase 2+
    //                                    deferred: actually force
    //                                    StableRef refresh on
    //                                    resume of a stolen
    //                                    outermost fiber)
    //   - envframe-version-refresh      hardcoded 0 (Phase 2+
    //                                    deferred: actually bump
    //                                    EnvFrame::version_ on
    //                                    resume of a stolen fiber)
    //   - bias-deferred-outermost-total hardcoded 0 (#754 bias
    //                                    feature not shipped —
    //                                    would record outermost
    //                                    defers driven by the
    //                                    adaptive bias scheduler)
    //   - recommendation                 0/1/2/3 derived from
    //                                    the 3 deferred flags +
    //                                    activity signal
    //   - schema == 783
    add("query:orchestration-steal-outermost-stats", [&ev](const auto&) -> EvalValue {
        // Read the 3 NEW static aggregates (Issue #783).
        const std::uint64_t outermost_total =
            aura_fiber_static_steal_outermost_mutation_boundary_total();
        const std::uint64_t inner_deferred_total =
            aura_fiber_static_steal_inner_mutation_boundary_deferred_total();
        const std::uint64_t cross_fiber_total =
            aura_fiber_static_cross_fiber_mutation_safe_steal_total();
        // 3 hardcoded "not yet" flags for Phase 2+ deferred
        // work (mirror #778/#779/#780/#781/#782 hardcoded
        // flag pattern).
        const std::int64_t strict_stable_ref_refresh = 0;
        const std::int64_t envframe_version_refresh = 0;
        const std::int64_t bias_deferred_outermost_total = 0;
        // Recommendation: derived from the 3 deferred flags
        // + activity signal. Note: the existing
        // is_at_mutation_boundary_safe() already enforces
        // depth==0 (Phase 1), so even with all 3 deferred
        // flags == 0, the steal path is safe — just without
        // the additional StableRef/EnvFrame safety nets.
        std::int64_t recommendation = 3;
        if (strict_stable_ref_refresh == 1 && envframe_version_refresh == 1 &&
            bias_deferred_outermost_total == 1)
            recommendation = 0; // production-ready with all Phase 2+
        else if (strict_stable_ref_refresh == 1 || envframe_version_refresh == 1 ||
                 bias_deferred_outermost_total == 1)
            recommendation = 1; // partial Phase 2+
        else if (outermost_total > 0 || inner_deferred_total > 0 || cross_fiber_total > 0)
            recommendation = 2; // Phase 1 only (steal split shipped)
        else
            recommendation = 3; // early-stage (no steal activity yet)
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
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
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("outermost-steal-total", static_cast<std::int64_t>(outermost_total));
        insert_kv("inner-deferred-total", static_cast<std::int64_t>(inner_deferred_total));
        insert_kv("cross-fiber-safe-steal-total", static_cast<std::int64_t>(cross_fiber_total));
        insert_kv("strict-stable-ref-refresh", strict_stable_ref_refresh);
        insert_kv("envframe-version-refresh", envframe_version_refresh);
        insert_kv("bias-deferred-outermost-total", bias_deferred_outermost_total);
        insert_kv("recommendation", recommendation);
        insert_kv("schema", 783);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

} // namespace aura::compiler::primitives_detail
