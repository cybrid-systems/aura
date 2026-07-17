// evaluator_primitives_obs_eval_13.cpp — Issue #909: peeled domain registration from observability
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
import aura.compiler.optimization_passes;
import aura.core.concept_constraints;

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

// Issue #909 part 104 (orig lines 11638-11700)
void ObservabilityPrims::register_eval_p104(PrimRegistrar add, Evaluator& ev) {

    // (engine:metrics "query:pass-concepts-stats") — Issue #1577:
    // centralized concept_constraints inventory + import hits.
    ObservabilityPrims::register_stats_impl(
        "query:pass-concepts-stats", [&ev](const auto&) -> EvalValue {
            using namespace aura::compiler::pass_concepts;
            note_concept_constraints_import();
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            const auto mod_kidx = ev.string_heap_.size();
            ev.string_heap_.push_back("aura.core.concept_constraints");
            const std::pair<std::string, EvalValue> fields[] = {
                {"phase", make_int(kConceptConstraintsPhase)},
                {"concept-count", make_int(kPassConceptCount)},
                {"import-hits",
                 make_int(static_cast<std::int64_t>(
                     concept_constraints_import_hits.load(std::memory_order_relaxed)))},
                {"module", make_string(mod_kidx)},
                {"schema", make_int(1577)},
            };
            for (auto& [k, v] : fields) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // (engine:metrics "query:optimization-passes-stats") — Issue #1576:
    // concrete optimization_passes registry + contract/pipeline counters.
    ObservabilityPrims::register_stats_impl(
        "query:optimization-passes-stats", [&ev](const auto&) -> EvalValue {
            using namespace aura::compiler::opt_registry;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            const auto load = [](std::atomic<std::uint64_t>& a) {
                return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
            };
            const std::pair<std::string, EvalValue> fields[] = {
                {"phase", make_int(kOptimizationPassesPhase)},
                {"pass-runs", make_int(load(opt_pass_runs_total))},
                {"pass-errors", make_int(load(opt_pass_errors_total))},
                {"contracts-pre-checks", make_int(load(opt_contracts_pre_checks_total))},
                {"contracts-post-checks", make_int(load(opt_contracts_post_checks_total))},
                {"contract-violations-soft", make_int(load(opt_contract_violations_soft_total))},
                {"pipeline-factory-runs", make_int(load(opt_pipeline_factory_runs_total))},
                {"default-table-count", make_int(static_cast<std::int64_t>(default_pass_count()))},
                {"constant-fold-runs",
                 make_int(load(
                     opt_pass_runs_by_kind[static_cast<std::size_t>(PassKind::ConstantFold)]))},
                {"type-propagation-runs",
                 make_int(load(
                     opt_pass_runs_by_kind[static_cast<std::size_t>(PassKind::TypePropagation)]))},
                {"compute-kind-runs",
                 make_int(
                     load(opt_pass_runs_by_kind[static_cast<std::size_t>(PassKind::ComputeKind)]))},
                {"shape-aware-fold-runs",
                 make_int(load(
                     opt_pass_runs_by_kind[static_cast<std::size_t>(PassKind::ShapeAwareFold)]))},
                {"define-dirty-skips",
                 make_int(static_cast<std::int64_t>(
                     aura::compiler::optimization_passes_skipped_by_define_dirty.load(
                         std::memory_order_relaxed)))},
                {"schema", make_int(1576)},
            };
            for (auto& [k, v] : fields) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // (query:closure-epoch-concurrency-stats) — Issue #739:
    // atomic epoch visibility under fiber steal + invalidate.
    // Fields (3 + sentinel):
    //   - stale-epoch-on-steal: epoch_stale_steal_caught
    //   - fence-enforced: closure_epoch_fence_enforced_total
    //   - linear-violation-prevented: linear_violation_prevented_epoch_total
    //   - schema == 739
    ObservabilityPrims::register_stats_impl(
        "query:closure-epoch-concurrency-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t stale_steal = 0;
            std::uint64_t fence_enforced = 0;
            std::uint64_t linear_prevented = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                stale_steal = m->epoch_stale_steal_caught.load(std::memory_order_relaxed);
                fence_enforced =
                    m->closure_epoch_fence_enforced_total.load(std::memory_order_relaxed);
                linear_prevented =
                    m->linear_violation_prevented_epoch_total.load(std::memory_order_relaxed);
            }
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            const std::pair<std::string, EvalValue> fields[] = {
                {"stale-epoch-on-steal", make_int(static_cast<std::int64_t>(stale_steal))},
                {"fence-enforced", make_int(static_cast<std::int64_t>(fence_enforced))},
                {"linear-violation-prevented",
                 make_int(static_cast<std::int64_t>(linear_prevented))},
                {"schema", make_int(739)},
            };
            for (auto& [k, v] : fields) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
