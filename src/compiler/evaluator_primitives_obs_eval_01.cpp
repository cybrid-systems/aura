// evaluator_primitives_obs_eval_01.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 8 (orig lines 1891-2024)
void ObservabilityPrims::register_eval_p8(PrimRegistrar add, Evaluator& ev) {

    // Issue #640: query:sv-verification-closedloop-stats —
    // Agent-discoverable structured dashboard for the
    // Verification Feedback → Structured SV Mutate Closed-Loop
    // (P0 EDA-SV-Review + commercial reliability surface).
    //
    // Note the naming distinction from #630:
    //   - (query:sv-verification-closedloop-stats-hash) (#630)
    //     is the historical hash primitive from before the
    //     AC1+AC2+AC3 enforcement work existed (12+ fields,
    //     predicate-driven coverage / assert / cex summary).
    //   - (query:sv-verification-closedloop-stats) (#640, this
    //     primitive) is the *enforcement-track* companion that
    //     focuses on the closed-loop counters for AC1+AC2+AC3
    //     and uses the issue-specified exact name from #640's
    //     AC4 (no `-hash` suffix).
    //
    // Fields (3 + sentinel):
    //   - feedback-apply       new sv_verify_feedback_apply_total
    //                          atomic (foundation for AC1
    //                          (eda:apply-verification-feedback
    //                          report) primitive wire-up —
    //                          bumped per successful feedback
    //                          → Guard + StableNodeRef +
    //                          sv_ir structured mutate).
    //                          Value is 0 until AC1 wire-up.
    //   - guard-reemit         new sv_guard_reemit_hook_total
    //                          atomic (foundation for AC2
    //                          Guard success → hardware_backend
    //                          re-emit hook wire-up).
    //                          Value is 0 until AC2 wire-up.
    //   - stable-ref-strict    new sv_stable_ref_provenance_
    //                          strict_total atomic (foundation
    //                          for AC3 strengthened StableNodeRef
    //                          provenance check on SV mutate
    //                          paths). Value is 0 until AC3
    //                          wire-up.
    //   - schema == 640         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~80% of the closed-
    // loop observability surface:
    //   - (query:verification-feedback-loop-stats) (#579) —
    //     8-field feedback → mutate closed-loop hash
    //   - (query:sv-verification-closedloop-stats-hash)
    //     (#630) — historical hash primitive
    //   - hardware_backend_hook_calls_total (#693) +
    //     commercial_reemits_total (#693) +
    //     feedback_mutate_hits_total (#693) +
    //     ppa_savings_total (#693) +
    //     verification_loop_success_total (#693)
    //   - eda_sv_feedback_mutate_success_total (#695) +
    //     eda_sv_stable_ref_invalidation_total (#695) +
    //     eda_sv_corruption_detected_total (#695)
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:sv-verification-closedloop-stats`
    // (no `-hash` suffix) with AC1+AC2+AC3-specific counters
    // — was *not* shipped under that exact name. So #640
    // ships ONE new Aura primitive + 3 new foundation
    // atomics.
    //
    // The remaining #640 AC1 + AC2 + AC3 work is invasive
    // C++ on the verification-feedback hot path
    // (eda:apply-verification-feedback primitive + Guard +
    // StableNodeRef capture + sv_ir structured mutate +
    // hardware_backend re-emit hook + strengthened
    // StableNodeRef provenance check) + needs the 5000+
    // fiber stress + TSan coverage from the issue body —
    // separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:sv-verification-closedloop-stats", [&ev](const auto&) -> EvalValue {
            // feedback-apply: new foundation atomic
            // (0 until AC1 eda:apply-verification-feedback
            // wire-up).
            const std::uint64_t feedback_apply =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->sv_verify_feedback_apply_total.load(std::memory_order_relaxed)
                    : 0;
            // guard-reemit: new foundation atomic
            // (0 until AC2 Guard → hardware_backend re-emit
            // hook wire-up).
            const std::uint64_t guard_reemit =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->sv_guard_reemit_hook_total.load(std::memory_order_relaxed)
                    : 0;
            // stable-ref-strict: new foundation atomic
            // (0 until AC3 strengthened StableNodeRef
            // provenance check wire-up).
            const std::uint64_t stable_ref_strict =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->sv_stable_ref_provenance_strict_total.load(std::memory_order_relaxed)
                    : 0;
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
            insert_kv("feedback-apply", static_cast<std::int64_t>(feedback_apply));
            insert_kv("guard-reemit", static_cast<std::int64_t>(guard_reemit));
            insert_kv("stable-ref-strict", static_cast<std::int64_t>(stable_ref_strict));
            insert_kv("schema", 640);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 9 (orig lines 2025-2096)
void ObservabilityPrims::register_eval_p9(PrimRegistrar add, Evaluator& ev) {

    // Issue #661: query:sv-interface-structure-stats — SV InterfaceIR
    // + ModportIR structure observability (P1 EDA-SV).
    //
    // The 3 counters track the structured interface IR/ModportIR
    // BUILDER shape (the foundations the issue body Action #4
    // calls out: ports_count + modport_views + direction_changes):
    //   - ports-count         sv_interface_ports_total
    //       Bumped per Interface body port addition (lifetime
    //       running total). Wired into `eda:parse-netlist`'s
    //       interface parse path.
    //   - modport-views       sv_interface_modport_views_total
    //       Bumped per Modport view addition. Wired into
    //       `eda:parse-netlist`'s modport parse path.
    //   - direction-changes   sv_interface_direction_changes_total
    //       Bumped per port direction change. Currently bumped
    //       via the test-only helpers (the production wire via
    //       `eda:set-port-direction` is follow-up work for
    //       Action #3 in the issue body).
    //   - interface-events-total
    //       Sum of the 3 above (per-call derivation, not a
    //       separate atomic). Lets dashboards show overall
    //       interface-structure-event volume at a glance.
    //   - schema == 661
    //
    // Non-duplicative with #640/#630/#539/#497/#498/#496 (those
    // cover SVA, verification, and pattern scopes); #661 covers
    // the interface IR/ModportIR BUILDER shape specifically.
    ObservabilityPrims::register_stats_impl(
        "query:sv-interface-structure-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t ports_count =
                static_cast<std::int64_t>(ev.get_sv_interface_ports_total());
            const std::int64_t modport_views =
                static_cast<std::int64_t>(ev.get_sv_interface_modport_views_total());
            const std::int64_t direction_changes =
                static_cast<std::int64_t>(ev.get_sv_interface_direction_changes_total());
            const std::int64_t events_total = ports_count + modport_views + direction_changes;
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
            insert_kv("ports-count", ports_count);
            insert_kv("modport-views", modport_views);
            insert_kv("direction-changes", direction_changes);
            insert_kv("interface-events-total", events_total);
            insert_kv("schema", 661);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 10 (orig lines 2097-2221)
void ObservabilityPrims::register_eval_p10(PrimRegistrar add, Evaluator& ev) {

    // Issue #641: query:stable-ref-provenance-sv-stats —
    // Agent-discoverable structured dashboard for the
    // StableNodeRef Cross-Fiber Provenance Enforcement in
    // Multi-Agent Orchestration (P0 EDSL-Review + commercial
    // reliability surface).
    //
    // Note the naming distinction from #631:
    //   - (query:stable-ref-provenance-sv-stats-hash) (#631)
    //     is the historical hash primitive from the
    //     pre-enforcement era (5 fields, provenance summary
    //     before fiber_id/workspace_id enforcement).
    //   - (query:stable-ref-provenance-sv-stats) (#641, this
    //     primitive) is the *enforcement-track* companion that
    //     focuses on the cross-fiber / multi-agent
    //     provenance enforcement counters for AC1+AC2+AC4
    //     and uses the issue-specified exact name from #641's
    //     AC3 (no `-hash` suffix).
    //
    // Fields (3 + sentinel):
    //   - fiber-check         new stable_ref_fiber_provenance_
    //                          check_total atomic (foundation
    //                          for AC1 fiber_id / workspace_id
    //                          match enforcement in query:/
    //                          mutate: + Guard dtor). Value
    //                          is 0 until AC1 wire-up.
    //   - auto-refresh        new stable_ref_provenance_auto_
    //                          refresh_total atomic (foundation
    //                          for AC2 Guard success →
    //                          auto-refresh provenance stamp).
    //                          Value is 0 until AC2 wire-up.
    //   - sv-feedback-wired   new stable_ref_sv_feedback_wired_
    //                          total atomic (foundation for AC4
    //                          provenance-checked SV feedback
    //                          path wire-up).
    //                          Value is 0 until AC4 wire-up.
    //   - schema == 641         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~70% of the
    // cross-fiber provenance observability surface:
    //   - (query:stable-ref-provenance) (#604) — base
    //     provenance summary primitive (no SV-specific track)
    //   - (query:stable-ref-provenance-sv-stats-hash) (#631)
    //     — historical hash primitive
    //   - stable_ref_provenance_query_total (#631) + cross_
    //     fiber_violations_total (#631) + safe_resolves_total
    //     (#631) — cross-fiber / multi-agent SV provenance
    //     counters
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:stable-ref-provenance-sv-stats` (no
    // `-hash` suffix) with AC1+AC2+AC4-specific counters —
    // was *not* shipped under that exact name. So #641 ships
    // ONE new Aura primitive + 3 new foundation atomics.
    //
    // The remaining #641 AC1 + AC2 + AC4 work is invasive C++
    // on the StableNodeRef validate_with_provenance +
    // Guard dtor + SV feedback hot path + needs the
    // multi-fiber steal + SV sequences + TSan coverage from
    // the issue body — separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-provenance-sv-stats", [&ev](const auto&) -> EvalValue {
            // fiber-check: new foundation atomic
            // (0 until AC1 fiber_id / workspace_id match
            // enforcement wire-up).
            const std::uint64_t fiber_check =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->stable_ref_fiber_provenance_check_total.load(std::memory_order_relaxed)
                    : 0;
            // auto-refresh: new foundation atomic
            // (0 until AC2 Guard success →
            // auto-refresh provenance stamp wire-up).
            const std::uint64_t auto_refresh =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->stable_ref_provenance_auto_refresh_total.load(std::memory_order_relaxed)
                    : 0;
            // sv-feedback-wired: new foundation atomic
            // (0 until AC4 provenance-checked SV feedback
            // path wire-up).
            const std::uint64_t sv_feedback_wired =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->stable_ref_sv_feedback_wired_total.load(std::memory_order_relaxed)
                    : 0;
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
            insert_kv("fiber-check", static_cast<std::int64_t>(fiber_check));
            insert_kv("auto-refresh", static_cast<std::int64_t>(auto_refresh));
            insert_kv("sv-feedback-wired", static_cast<std::int64_t>(sv_feedback_wired));
            insert_kv("schema", 641);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 11 (orig lines 2222-2348)
void ObservabilityPrims::register_eval_p11(PrimRegistrar add, Evaluator& ev) {

    // Issue #642: query:arena-auto-compaction-stats —
    // Agent-discoverable structured dashboard for the Arena
    // Auto-Compaction + Fiber/GC Safepoint Coordination
    // (P0 Prompt6-MemorySafety + commercial reliability
    // surface).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:arena-auto-compact-stats) — earlier
    //     primitive focused on the auto-compact trigger only
    //   - (query:arena-auto-compact-defrag-stats) (#569) —
    //     extended version with defrag breakdown
    //   - (query:arena-auto-compaction-stats) (#642, this
    //     primitive) — *enforcement-track* companion that
    //     focuses on the auto-compaction + fiber/GC
    //     safepoint coordination counters for AC1+AC2+AC3
    //     and uses the issue-specified exact name from #642's
    //     AC4 (`-compaction` with the `-ion` suffix, NOT
    //     `-compact`).
    //
    // Fields (3 + sentinel):
    //   - auto-trigger        new arena_auto_compact_trigger_
    //                          total atomic (foundation for
    //                          AC1 allocate_raw auto-trigger
    //                          compact on fragmentation >
    //                          threshold + fiber safepoint
    //                          coordination). Value is 0
    //                          until AC1 wire-up.
    //   - live-move-yield     new arena_live_move_yield_total
    //                          atomic (foundation for AC2
    //                          compact/defrag enhanced with
    //                          live move + yield support).
    //                          Value is 0 until AC2 wire-up.
    //   - guard-defrag        new arena_guard_request_defrag_
    //                          total atomic (foundation for
    //                          AC3 Guard/invalidate → request_
    //                          defrag wiring). Value is 0
    //                          until AC3 wire-up.
    //   - schema == 642         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641
    //                          sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~70% of the
    // auto-compaction observability surface:
    //   - (query:arena-auto-stats) — broader arena stats
    //   - (query:arena-auto-compact-stats) — earlier
    //     auto-compact trigger primitive
    //   - (query:arena-auto-compact-defrag-stats) (#569) —
    //     defrag breakdown primitive
    //   - (query:arena-compaction-stats) — base compaction
    //     summary primitive
    //   - (query:arena-fragmentation-snapshot) — snapshot
    //     primitive
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:arena-auto-compaction-stats` (`-ion`
    // suffix, not `-compact`) with AC1+AC2+AC3-specific
    // counters — was *not* shipped under that exact name.
    // So #642 ships ONE new Aura primitive + 3 new foundation
    // atomics.
    //
    // The remaining #642 AC1 + AC2 + AC3 work is invasive
    // C++ on the allocate_raw + compact/defrag + Guard hot
    // path + needs the 10k+ mutate + 20+ fibers + TSan/ASan
    // coverage from the issue body — separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:arena-auto-compaction-stats", [&ev](const auto&) -> EvalValue {
            // auto-trigger: new foundation atomic
            // (0 until AC1 allocate_raw auto-trigger wire-up).
            const std::uint64_t auto_trigger =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->arena_auto_compact_trigger_total.load(std::memory_order_relaxed)
                    : 0;
            // live-move-yield: new foundation atomic
            // (0 until AC2 live move + yield wire-up).
            const std::uint64_t live_move_yield =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->arena_live_move_yield_total.load(std::memory_order_relaxed)
                    : 0;
            // guard-defrag: new foundation atomic
            // (0 until AC3 Guard/invalidate → request_defrag
            // wire-up).
            const std::uint64_t guard_defrag =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->arena_guard_request_defrag_total.load(std::memory_order_relaxed)
                    : 0;
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
            insert_kv("auto-trigger", static_cast<std::int64_t>(auto_trigger));
            insert_kv("live-move-yield", static_cast<std::int64_t>(live_move_yield));
            insert_kv("guard-defrag", static_cast<std::int64_t>(guard_defrag));
            insert_kv("schema", 642);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 12 (orig lines 2349-2569)
void ObservabilityPrims::register_eval_p12(PrimRegistrar add, Evaluator& ev) {

    // Issue #643: query:primitives-meta — Agent-discoverable
    // structured per-primitive AI-native introspection
    // primitive (P0 Stdlib-Impl-P1 foundation — implements
    // #633 AC3+AC4 + #559 classification wire-up).
    //
    // Note the naming distinction from #498:
    //   - (query:primitive-metadata) (#498, no `s`) — base
    //     AI-native primitive introspection primitive
    //     (no per-primitive lookup arg, returns list)
    //   - (query:primitives-meta) (#643, this primitive) —
    //     per-primitive lookup form per issue body AC2 exact
    //     spec. Accepts optional [name] argument:
    //       - (query:primitives-meta) → list of all
    //         primitive meta pairs (alias for catalog)
    //       - (query:primitives-meta 'foo) → single meta
    //         pair for primitive "foo" or () if not found
    //     Uses the new primitives_meta_query_total counter
    //     (distinct from primitives_meta_catalog_query_total
    //     #617 which tracks the catalog primitive).
    //
    // Fields per entry (8) + sentinel:
    //   - name              primitive name (string)
    //   - has-fn            1 if the primitive has a registered
    //                       function body, 0 otherwise
    //   - arity             from PrimMeta.arity (255 = variadic)
    //                       — foundation for the DEFINE_PRIMITIVE
    //                       macro arity-at-compile validation
    //                       (#643 AC1)
    //   - pure              from PrimMeta.pure (bool) — lets the
    //                       Agent decide whether memoization /
    //                       const-folding applies (Issue #669
    //                       fill-the-gap enrichment)
    //   - safety            from PrimMeta.safety_flags (int) —
    //                       0x01=mutates, 0x02=io, 0x04=fiber
    //                       (#480 + #669 enrichment)
    //   - doc               from PrimMeta.doc (string, "") —
    //                       lets the Agent render help text for
    //                       end-users without hardcoded
    //                       (#480 + #669 enrichment)
    //   - category          classification from #559
    //                       (mutation-safety / core /
    //                       internal-observable / convenience)
    //   - schema == 669     sentinel for Agent drift detection
    //                       (changed from 643 in #669 to signal
    //                       the enriched 8-field shape; pre-#669
    //                       shape was 5 fields with hardcoded
    //                       arity=0 / category="internal-observable" / no
    //                       pure / no safety / no doc)
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~70% of the AI-native
    // meta introspection surface:
    //   - (query:primitive-metadata) (#498) — base AI-native
    //     primitive introspection (no per-primitive lookup arg)
    //   - (query:primitives-meta-catalog) (#617) — catalog
    //     primitive with category + arity + meta
    //   - (query:primitives-by-category) — category filter
    //     primitive
    //   - (query:primitives-extension-stats) (#618/#625) —
    //     extension stats
    //   - primitives_meta_catalog_query_total (#617) — catalog
    //     hit-rate counter
    // What the issue body AC2 specifies by **exact name +
    // signature** — `query:primitives-meta [name]` accepting
    // an optional [name] argument for per-primitive lookup
    // — was *not* shipped under that exact signature. So #643
    // ships ONE new Aura primitive (with optional [name] arg
    // dispatch) + 3 new foundation atomics.
    //
    // The remaining #643 AC1 (DEFINE_PRIMITIVE macro) + AC3
    // (PRIM_ERROR unification) + AC4 (primitives_style.md) work
    // is invasive C++ on the registry / evaluator.ixx /
    // primitives_detail header + needs the AI-Agent generate-
    // primitive demo + ./build.py check + CI gate coverage
    // from the issue body — separate follow-ups.
    add("query:primitives-meta", [&ev](const auto& a) -> EvalValue {
        // Bump the new per-primitive-lookup counter (distinct
        // from primitives_meta_catalog_query_total #617).
        if (auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())) {
            m->primitives_meta_query_total.fetch_add(1, std::memory_order_relaxed);
        }
        // The foundation scaffolding atomics (currently 0
        // until AC1+AC3 wire-up).
        const std::uint64_t define_macro_used =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->define_primitive_macro_used_total.load(std::memory_order_relaxed)
                : 0;
        const std::uint64_t prim_error_unified =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->prim_error_unified_total.load(std::memory_order_relaxed)
                : 0;
        auto build_pair = [&](const std::string& name) -> EvalValue {
            // Issue #669: enrich the per-name response with
            // real PrimMeta fields (arity, pure, safety,
            // doc, category) via meta_for_slot. Pre-#669 the
            // shape returned hardcoded arity=0 + has-fn=1 +
            // category="internal-observable" — PrimMeta was
            // populated (#480) but never reached the Agent.
            //
            // Schema now exposes 8 fields:
            //   - name              primitive name
            //   - has-fn            1 if registered, 0 if unknown
            //   - arity             from PrimMeta.arity (255 = variadic)
            //   - pure              from PrimMeta.pure (bool)
            //   - safety            from PrimMeta.safety_flags (int)
            //   - doc               from PrimMeta.doc (string, "")
            //   - category          from PrimMeta.category (string)
            //   - schema            669 (drift sentinel — changed
            //                       from 643 to signal the
            //                       enriched field set)
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, EvalValue v) {
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
                        vals[idx] = v.val;
                        ht->size++;
                        return;
                    }
                }
            };
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(name);
            insert_kv("name", make_string(static_cast<std::uint64_t>(name_idx)));
            // Look up the real PrimMeta via slot_for_name +
            // meta_for_slot. If unknown, has-fn=0 + default
            // PrimMeta{} (the Agent can distinguish "known
            // primitive with no body" from "unknown" via
            // has-fn).
            const auto slot = ev.primitives_.slot_for_name(name);
            const bool known = slot < ev.primitives_.slot_count();
            const PrimMeta& pm = known ? ev.primitives_.meta_for_slot(slot) : PrimMeta{};
            insert_kv("has-fn", make_int(known ? 1 : 0));
            insert_kv("arity", make_int(static_cast<std::int64_t>(pm.arity)));
            insert_kv("pure", make_bool(pm.pure));
            insert_kv("safety", make_int(static_cast<std::int64_t>(pm.safety_flags)));
            auto doc_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(pm.doc);
            insert_kv("doc", make_string(static_cast<std::uint64_t>(doc_idx)));
            auto cat_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(pm.category);
            insert_kv("category", make_string(static_cast<std::uint64_t>(cat_idx)));
            insert_kv("schema", make_int(669));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        // Dispatch: optional [name] argument.
        if (!a.empty() && aura::compiler::types::is_string(a[0])) {
            const auto idx = aura::compiler::types::as_string_idx(a[0]);
            if (idx < ev.string_heap_.size()) {
                const auto& name = ev.string_heap_[idx];
                // Build the meta hash for the requested name.
                // Whether or not the primitive exists, we return
                // the meta shape so the Agent can introspect —
                // has-fn=0 + arity=0 if not found is a valid
                // response (lets the Agent distinguish "known
                // primitive with no body" from "unknown").
                return build_pair(name);
            }
            return make_void();
        }
        // No [name] arg → return a pair with the aggregate
        // foundation counters + the schema sentinel so the
        // Agent can dashboard at-a-glance. (Full catalog
        // form is provided by #617 query:primitives-meta-
        // catalog — the new primitive specializes on
        // per-name lookup.)
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
        insert_kv("define-macro-used", static_cast<std::int64_t>(define_macro_used));
        insert_kv("prim-error-unified", static_cast<std::int64_t>(prim_error_unified));
        insert_kv("schema", 643);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

// Issue #909 part 13 (orig lines 2570-2641)
void ObservabilityPrims::register_eval_p13(PrimRegistrar add, Evaluator& ev) {

    // Issue #669: query:primitives-meta-stats — per-meta
    // observability summary (P1 stdlib-impl AI-native
    // introspection gap-fill). Reports the count of
    // primitives by meta status so the Agent can see
    // how much of the stdlib surface has been
    // meta-documented vs left default.
    //
    // Non-duplicative with #617 query:primitives-meta-catalog
    // (which returns the registry-level summary) and
    // #697 query:primitives-extension-stats (which tracks
    // runtime counters). #669 adds the per-meta-axis
    // observability summary that the Agent needs to
    // know "how much is documented?" at-a-glance.
    //
    // Fields (4 + sentinel):
    //   - meta-hits            primitives_meta_query_total
    //                          (this primitive's call counter)
    //   - documented-count     documented_meta_count()
    //                          (primitives with non-empty doc)
    //   - schema-documented    schema_documented_meta_count()
    //                          (primitives with both doc AND
    //                          schema set)
    //   - total-registered     primitives_.slot_count()
    //   - schema == 669        drift sentinel
    ObservabilityPrims::register_stats_impl(
        "query:primitives-meta-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t meta_hits = static_cast<std::int64_t>(
                ev.compiler_metrics()
                    ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                          ->primitives_meta_query_total.load(std::memory_order_relaxed)
                    : 0);
            const std::int64_t documented =
                static_cast<std::int64_t>(ev.get_primitive_documented_meta_count());
            const std::int64_t schema_documented =
                static_cast<std::int64_t>(ev.primitives_.schema_documented_meta_count());
            const std::int64_t total = static_cast<std::int64_t>(ev.primitives_.slot_count());
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
            insert_kv("meta-hits", meta_hits);
            insert_kv("documented-count", documented);
            insert_kv("schema-documented", schema_documented);
            insert_kv("total-registered", total);
            insert_kv("schema", 669);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 14 (orig lines 2642-2729)
void ObservabilityPrims::register_eval_p14(PrimRegistrar add, Evaluator& ev) {

    // Issue #671: query:primitives-consistency-stats —
    // primitives_detail lambda capture discipline + style
    // compliance observability (P1 stdlib-impl consistency).
    //
    // Companion to #709 (query:primitives-registry-stats which
    // bundles registry-level metrics) but specialized on the
    // capture-discipline axis. The existing #709 primitive
    // exposes `capture-violations` as one of 7 fields; #671
    // carves out a dedicated primitive focused on consistency:
    //   - capture-violations-detected
    //       primitive_capture_violations_total — bumped by
    //       prim_record_capture_violation when a primitive
    //       fails the capture contract (no error_counter on
    //       a mutate path)
    //   - style-compliance-pct
    //       derived metric: (1 - capture_violations /
    //       slot_count) * 100 — 100 means every primitive
    //       passes the contract
    //   - capture-contract-version
    //       kPrimCaptureContractVersion (defined in
    //       primitives_detail.h) — bump when the contract
    //       changes so the Agent can detect drift
    //   - recommended-action
    //       0 = no action, 1 = backfill missing meta, 2 =
    //       audit capture contract. Triggered when
    //       capture_violations > 0 or documented < slots.
    //   - schema == 671
    //
    // Non-duplicative with #709 (registry-level summary with
    // 7 fields covering fast-path + EDA integration),
    // #615 (PRIM_ERROR macro shape), #643 (DEFINE_PRIMITIVE
    // macro for registration), #617 (catalog registry
    // summary).
    ObservabilityPrims::register_stats_impl(
        "query:primitives-consistency-stats", [&ev](const auto&) -> EvalValue {
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t capture_viol =
                m ? m->primitive_capture_violations_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t slots = ev.primitives_.slot_count();
            const std::uint64_t documented = ev.primitives_.documented_meta_count();
            const std::int64_t style_compliance_pct =
                slots > 0 ? static_cast<std::int64_t>(
                                ((slots > capture_viol ? slots - capture_viol : 0) * 100) / slots)
                          : 100;
            std::int64_t recommended_action = 0;
            if (capture_viol > 0)
                recommended_action = 2; // audit capture contract
            else if (documented < slots)
                recommended_action = 1; // backfill missing meta
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
            insert_kv("capture-violations-detected", static_cast<std::int64_t>(capture_viol));
            insert_kv("style-compliance-pct", style_compliance_pct);
            insert_kv("registry-slots", static_cast<std::int64_t>(slots));
            insert_kv("documented-count", static_cast<std::int64_t>(documented));
            insert_kv("capture-contract-version",
                      static_cast<std::int64_t>(kPrimCaptureContractVersion));
            insert_kv("recommended-action", recommended_action);
            insert_kv("schema", 671);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 15 (orig lines 2730-2789)
void ObservabilityPrims::register_eval_p15(PrimRegistrar add, Evaluator& ev) {

    // Issue #751: query:primitives-contract-stats — P0 PRIM_ERROR /
    // capture discipline enforcement dashboard (refines #728/#671/#615;
    // non-duplicative with #671 primitives-consistency-stats which
    // focuses on registry meta backfill + recommended-action).
    //
    // Fields (4 + sentinel):
    //   - capture-violations     primitive_capture_violations_total
    //   - prim-error-hits        prim_error_unified_total
    //   - style-compliance-pct   derived (slots - violations) / slots * 100
    //   - capture-contract-version kPrimCaptureContractVersion
    //   - schema == 751
    ObservabilityPrims::register_stats_impl(
        "query:primitives-contract-stats", [&ev](const auto&) -> EvalValue {
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t capture_viol =
                m ? m->primitive_capture_violations_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t prim_errors =
                m ? m->prim_error_unified_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t slots = ev.primitives_.slot_count();
            const std::int64_t style_compliance_pct =
                slots > 0 ? static_cast<std::int64_t>(
                                ((slots > capture_viol ? slots - capture_viol : 0) * 100) / slots)
                          : 100;
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
            insert_kv("capture-violations", static_cast<std::int64_t>(capture_viol));
            insert_kv("prim-error-hits", static_cast<std::int64_t>(prim_errors));
            insert_kv("style-compliance-pct", style_compliance_pct);
            insert_kv("capture-contract-version",
                      static_cast<std::int64_t>(kPrimCaptureContractVersion));
            insert_kv("schema", 751);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
