// evaluator_primitives_obs_eval_03.cpp — Issue #909: peeled domain registration from observability
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

// Issue #909 part 24 (orig lines 3553-3684)
void ObservabilityPrims::register_eval_p24(PrimRegistrar add, Evaluator& ev) {

    // Issue #648: query:panic-checkpoint-fiber-stats —
    // Agent-discoverable structured dashboard for the Panic
    // Checkpoint + Yield Checkpoint Storage Lifecycle +
    // INVALID_VERSION Frame Handling in Fiber Resume +
    // Concurrent GC (P0 Runtime-Gap + Panic production-
    // readiness surface — non-duplicative to #637 #356 #264).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:panic-checkpoint-lifecycle-stats) — existing
    //     high-level panic checkpoint lifecycle summary
    //   - (query:panic-checkpoint-fiber-stats) (#648, this
    //     primitive) — *enforcement-track* companion that
    //     focuses on the AC1+AC2+AC3 counters for fiber
    //     resume transfer + INVALID_VERSION frame handling
    //     in GC + concurrent panic/GC conflict.
    //
    // Fields (3 + sentinel):
    //   - transfer-on-resume    new panic_transfer_on_resume_
    //                            total atomic (foundation for
    //                            AC1 fiber resume panic
    //                            checkpoint transfer).
    //                            Value is 0 until AC1 wire-up.
    //   - invalid-frames-skipped
    //                            new panic_invalid_frames_
    //                            skipped_total atomic
    //                            (foundation for AC2
    //                            INVALID_VERSION frame
    //                            skip/count in GC walk /
    //                            compact). Value is 0 until
    //                            AC2 wire-up.
    //   - concurrent-gc-conflict
    //                            new panic_concurrent_gc_
    //                            conflict_total atomic
    //                            (foundation for AC3
    //                            concurrent panic + GC
    //                            conflict coordination).
    //                            Value is 0 until AC3 wire-up.
    //   - schema == 648           sentinel for Agent drift
    //                            detection (mirrors the full
    //                            chain through
    //                            #618+#620+#621+#622+#623+
    //                            #624+#625+#626+#630+#631+
    //                            #632+#633+#637+#640+#641+
    //                            #642+#643+#644+#645+#646+
    //                            #647 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // panic checkpoint lifecycle observability surface:
    //   - (query:panic-checkpoint-lifecycle-stats) —
    //     high-level panic checkpoint lifecycle summary
    //   - #264 yield checkpoint foundation
    //   - #356 INVALID_VERSION env_frames_ sentinel +
    //     post-rollback frames
    //   - #637 IRClosure + EnvFrame versioning + bridge
    //     invalidate protocol
    //   - #588 per-fiber stack + yield_checkpoint_storage_
    //   - #591 GC pause attribution
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:panic-checkpoint-fiber-stats` with
    // AC1+AC2+AC3-specific counters as a structured hash —
    // was *not* shipped under that exact name. So #648 ships
    // ONE new Aura primitive + 3 new foundation atomics.
    //
    // The remaining #648 AC1 (fiber resume validate/sync
    // per-fiber yield_checkpoint_storage_) + AC2 (GCEnvWalkFn
    // + compact handle INVALID_VERSION frames) + AC3
    // (g_fiber_yield_checkpoint_ + resume_validate_ coordinate
    // with panic checkpoint under MutationBoundary) work is
    // invasive C++ on fiber.cpp resume() + GCEnvWalkFn +
    // compact + Guard panic state + needs the panic during
    // deep mutate + steal + GC matrix + rollback +
    // INVALID_VERSION cases + TSan coverage from the issue
    // body — separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:panic-checkpoint-fiber-stats", [&ev](const auto&) -> EvalValue {
            // transfer-on-resume: new foundation atomic
            // (0 until AC1 fiber resume wire-up).
            const std::uint64_t transfer_on_resume =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->panic_transfer_on_resume_total.load(std::memory_order_relaxed)
                    : 0;
            // invalid-frames-skipped: new foundation atomic
            // (0 until AC2 GC walk/compact wire-up).
            const std::uint64_t invalid_frames_skipped =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->panic_invalid_frames_skipped_total.load(std::memory_order_relaxed)
                    : 0;
            // concurrent-gc-conflict: new foundation atomic
            // (0 until AC3 concurrent panic + GC wire-up).
            const std::uint64_t concurrent_gc_conflict =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->panic_concurrent_gc_conflict_total.load(std::memory_order_relaxed)
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
            insert_kv("transfer-on-resume", static_cast<std::int64_t>(transfer_on_resume));
            insert_kv("invalid-frames-skipped", static_cast<std::int64_t>(invalid_frames_skipped));
            insert_kv("concurrent-gc-conflict", static_cast<std::int64_t>(concurrent_gc_conflict));
            insert_kv("schema", 648);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 25 (orig lines 3685-3831)
void ObservabilityPrims::register_eval_p25(PrimRegistrar add, Evaluator& ev) {

    // Issue #649: query:yield-checkpoint-panic-stats —
    // Agent-discoverable structured dashboard for the Full
    // Per-Fiber YieldCheckpointStorage Re-Stamp + Size
    // Validation on Panic Transfer + Cross-Steal (P0
    // Runtime-Gap + Panic production-readiness surface —
    // non-duplicative to #648 #264).
    //
    // Note the naming distinction from #648:
    //   - (query:panic-checkpoint-fiber-stats) (#648) —
    //     fiber resume panic checkpoint transfer +
    //     INVALID_VERSION GC + concurrent panic/GC
    //     conflict (transport layer)
    //   - (query:yield-checkpoint-panic-stats) (#649, this
    //     primitive) — *yield-checkpoint storage lifecycle*
    //     companion that focuses on the AC1+AC2+AC3
    //     counters for yield_checkpoint re-stamp +
    //     size validation + cross-steal invalidation
    //     (storage lifecycle layer that #648 doesn't cover).
    //
    // Fields (3 + sentinel):
    //   - transfer-with-restamp   new yield_transfer_with_
    //                              restamp_total atomic
    //                              (foundation for AC1 panic
    //                              transfer triggering yield_
    //                              checkpoint re-stamp).
    //                              Value is 0 until AC1
    //                              wire-up.
    //   - size-mismatch-caught    new yield_size_mismatch_
    //                              caught_total atomic
    //                              (foundation for AC2
    //                              yield_checkpoint stack
    //                              size + top-entry version
    //                              mismatch caught in
    //                              restore_post_yield_or_
    //                              rollback). Value is 0
    //                              until AC2 wire-up.
    //   - cross-steal-invalidation
    //                              new yield_cross_steal_
    //                              invalidation_total
    //                              atomic (foundation for
    //                              AC3 cross-steal
    //                              invalidation of pending
    //                              yield checkpoints).
    //                              Value is 0 until AC3
    //                              wire-up.
    //   - schema == 649             sentinel for Agent drift
    //                              detection (mirrors the
    //                              full chain through
    //                              #618+#620+#621+#622+
    //                              #623+#624+#625+#626+
    //                              #630+#631+#632+#633+
    //                              #637+#640+#641+#642+
    //                              #643+#644+#645+#646+
    //                              #647+#648 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // yield checkpoint + panic observability surface:
    //   - (query:panic-checkpoint-fiber-stats) (#648) —
    //     fiber resume panic transfer (transport layer)
    //   - (query:panic-checkpoint-lifecycle-stats) —
    //     high-level panic checkpoint lifecycle summary
    //   - #264 yield checkpoint foundation
    //   - #356 INVALID_VERSION + post-rollback frames
    //   - #588 per-fiber stack + yield_checkpoint_storage_
    //   - transfer_panic_checkpoint_trampoline + bump metric
    //   - restore_post_yield_or_rollback validates
    //     thread/version/depth but no yield_checkpoint
    //     re-stamp or size check
    //   - g_fiber_yield_checkpoint_deleter_ exists but no
    //     panic-state re-stamp coordination
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:yield-checkpoint-panic-stats` with
    // AC1+AC2+AC3-specific counters as a structured hash —
    // was *not* shipped under that exact name. So #649 ships
    // ONE new Aura primitive + 3 new foundation atomics.
    //
    // The remaining #649 AC1 (transfer_panic_checkpoint_
    // trampoline + fiber resume after hook call re-stamp or
    // resize yield_checkpoint_storage_) + AC2 (restore_post_
    // yield_or_rollback adds yield_checkpoint stack size +
    // top-entry version check) + AC3 (g_fiber_yield_checkpoint_
    // coordinates with pending_panic_checkpoint under
    // MutationBoundary) work is invasive C++ on
    // transfer_panic_checkpoint_trampoline + fiber resume +
    // restore_post_yield_or_rollback + g_fiber_yield_checkpoint_
    // + needs the panic during deep yield-boundary + steal +
    // resume matrix + TSan coverage from the issue body —
    // separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:yield-checkpoint-panic-stats", [&ev](const auto&) -> EvalValue {
            // transfer-with-restamp: new foundation atomic
            // (0 until AC1 panic transfer wire-up).
            const std::uint64_t transfer_with_restamp =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->yield_transfer_with_restamp_total.load(std::memory_order_relaxed)
                    : 0;
            // size-mismatch-caught: new foundation atomic
            // (0 until AC2 yield_checkpoint stack size wire-up).
            const std::uint64_t size_mismatch_caught =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->yield_size_mismatch_caught_total.load(std::memory_order_relaxed)
                    : 0;
            // cross-steal-invalidation: new foundation atomic
            // (0 until AC3 cross-steal invalidation wire-up).
            const std::uint64_t cross_steal_invalidation =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->yield_cross_steal_invalidation_total.load(std::memory_order_relaxed)
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
            insert_kv("transfer-with-restamp", static_cast<std::int64_t>(transfer_with_restamp));
            insert_kv("size-mismatch-caught", static_cast<std::int64_t>(size_mismatch_caught));
            insert_kv("cross-steal-invalidation",
                      static_cast<std::int64_t>(cross_steal_invalidation));
            insert_kv("schema", 649);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 26 (orig lines 3832-3966)
void ObservabilityPrims::register_eval_p26(PrimRegistrar add, Evaluator& ev) {

    // Issue #650: query:scheduler-stealbudget-yield-class-stats —
    // Agent-discoverable structured dashboard for the
    // StealBudget in WorkerThread to Use fiber
    // yield_classification() + Outermost Mutation Depth for
    // Adaptive Bias (P0 Runtime-Gap + Scheduler production-
    // readiness surface — non-duplicative to #645).
    //
    // Note the naming distinction from #706:
    //   - (query:scheduler-stealbudget-adaptive-stats) (#706)
    //     — 5-field adaptive bias summary (already covers
    //     mutation-bias-hits + outermost-preferred +
    //     llm-tail-reductions + deferred-pressure-boosts +
    //     global-deferred-mutation-total — the AC3
    //     surface)
    //   - (query:scheduler-steal-bias-stats) (#645) —
    //     per-steal LIFO/FIFO + mutation-deferred counters
    //     (lower-level enforcement layer)
    //   - (query:scheduler-stealbudget-yield-class-stats)
    //     (#650, this primitive) — *yield-class-bias-track*
    //     companion that focuses on the AC1+AC2 enforcement
    //     counters for StealBudget consultation of victim
    //     yield_classification() + outermost mutation depth
    //     + max_before_sleep raised on contention.
    //
    // Fields (3 + sentinel):
    //   - outermost-bias       new stealbudget_outermost_bias_
    //                          total atomic (foundation for
    //                          AC1 bias hits preferring
    //                          outermost Mutation fibers).
    //                          Value is 0 until AC1 wire-up.
    //   - explicit-bias        new stealbudget_explicit_bias_
    //                          total atomic (foundation for
    //                          AC1 bias hits preferring
    //                          Explicit yield reason fibers).
    //                          Value is 0 until AC1 wire-up.
    //   - max-sleep-raised     new stealbudget_max_before_sleep_
    //                          raised_total atomic (foundation
    //                          for AC2 max_before_sleep raised
    //                          on contention). Value is 0
    //                          until AC2 wire-up.
    //   - schema == 650         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643+#644+#645+#646+
    //                          #647+#648+#649 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // StealBudget adaptive bias surface:
    //   - (query:scheduler-stealbudget-adaptive-stats) (#706)
    //     — 5-field adaptive bias summary (the AC3 surface
    //     listed in #650 body)
    //   - (query:scheduler-steal-bias-stats) (#645) — per-
    //     steal LIFO/FIFO + mutation-deferred
    //   - #618 per-fiber yield_reason classification +
    //     is_at_mutation_boundary_safe + outermost depth
    //     probe
    //   - #588 per-fiber stack + StealBudget WINDOW_SIZE=10
    //     thresholds 50%/30%/10%
    //   - #451 work-stealing deque LIFO local + FIFO steal
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:scheduler-stealbudget-adaptive-stats`
    // already ships the AC3 fields. #650 ships the AC1+AC2
    // enforcement-layer companion with a distinct name
    // (`-yield-class-` midfix).
    //
    // The remaining #650 AC1 (try_steal_from / should_steal
    // query yield_classification + outermost depth) + AC2
    // (high steal_deferred_mutation_boundary_count raises
    // max_before_sleep) + AC4 (unit test mock Fiber yield
    // reasons) work is invasive C++ on worker.cpp/h +
    // StealBudget + needs the LLM latency + mixed yield
    // reasons matrix + 20 fibers + TSan coverage from the
    // issue body — separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:scheduler-stealbudget-yield-class-stats", [&ev](const auto&) -> EvalValue {
            // outermost-bias: new foundation atomic
            // (0 until AC1 outermost bias wire-up).
            const std::uint64_t outermost_bias =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->stealbudget_outermost_bias_total.load(std::memory_order_relaxed)
                    : 0;
            // explicit-bias: new foundation atomic
            // (0 until AC1 explicit bias wire-up).
            const std::uint64_t explicit_bias =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->stealbudget_explicit_bias_total.load(std::memory_order_relaxed)
                    : 0;
            // max-sleep-raised: new foundation atomic
            // (0 until AC2 max_before_sleep raise wire-up).
            const std::uint64_t max_sleep_raised =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->stealbudget_max_before_sleep_raised_total.load(
                              std::memory_order_relaxed)
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
            insert_kv("outermost-bias", static_cast<std::int64_t>(outermost_bias));
            insert_kv("explicit-bias", static_cast<std::int64_t>(explicit_bias));
            insert_kv("max-sleep-raised", static_cast<std::int64_t>(max_sleep_raised));
            insert_kv("schema", 650);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 27 (orig lines 3967-4113)
void ObservabilityPrims::register_eval_p27(PrimRegistrar add, Evaluator& ev) {

    // Issue #651: query:gc-panic-deferral-stats — Agent-
    // discoverable structured dashboard for the Actual GC
    // Deferral/Block Logic in
    // block_gc_for_pending_checkpoint_trampoline + Request
    // Shim (P0 Runtime-Gap + GC production-readiness surface —
    // fills TODO in evaluator_fiber_mutation.cpp, non-
    // duplicative to #646 #648).
    //
    // Note the relationship to existing primitives:
    //   - (query:gc-safepoint-stats) — base GC safepoint
    //     primitive (no deferral/panic breakdown)
    //   - (query:gc-safepoint-deferral-stats) (#646) —
    //     deferral + backoff for outermost-vs-inner
    //     MutationBoundary (no panic-specific breakdown)
    //   - (query:panic-checkpoint-fiber-stats) (#648) —
    //     fiber resume panic transfer (no GC-deferral
    //     wire-up)
    //   - (query:gc-panic-deferral-stats) (#651, this
    //     primitive) — *GC-panic coordination* companion that
    //     focuses on the AC1+AC2+AC3 counters for
    //     block_gc trampoline deferral + GC request blocked
    //     by panic + panic/GC conflict resolution.
    //
    // Fields (3 + sentinel):
    //   - pending-panic-deferral
    //                            new gc_panic_pending_deferral_
    //                            total atomic (foundation for
    //                            AC1 pending panic checkpoint
    //                            deferral triggered in
    //                            block_gc trampoline). Value
    //                            is 0 until AC1 wire-up.
    //   - gc-blocked-by-panic   new gc_blocked_by_panic_total
    //                            atomic (foundation for AC2 GC
    //                            safepoint request blocked
    //                            due to pending panic +
    //                            depth > 0). Value is 0 until
    //                            AC2 wire-up.
    //   - conflicts-resolved    new gc_panic_conflict_resolved_
    //                            total atomic (foundation for
    //                            AC3 panic + GC conflict
    //                            resolved without root
    //                            inconsistency). Value is 0
    //                            until AC3 wire-up.
    //   - schema == 651           sentinel for Agent drift
    //                            detection (mirrors the full
    //                            chain through
    //                            #618+#620+#621+#622+#623+
    //                            #624+#625+#626+#630+#631+
    //                            #632+#633+#637+#640+#641+
    //                            #642+#643+#644+#645+#646+
    //                            #647+#648+#649+#650
    //                            sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // GC + panic observability surface:
    //   - (query:gc-safepoint-stats) — base GC safepoint
    //   - (query:gc-safepoint-deferral-stats) (#646) —
    //     deferral + backoff for outermost-vs-inner
    //     MutationBoundary
    //   - (query:panic-checkpoint-fiber-stats) (#648) —
    //     fiber resume panic transfer
    //   - (query:panic-checkpoint-lifecycle-stats) —
    //     high-level panic lifecycle
    //   - block_gc_for_pending_checkpoint_trampoline +
    //     g_block_gc_for_pending_checkpoint exist but with
    //     "actual GC deferral is out of scope for the current
    //     ship (TODO)" comment
    //   - aura_evaluator_request_gc_safepoint forwards but
    //     only records request (no pending panic check)
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:gc-panic-deferral-stats` with
    // AC1+AC2+AC3-specific counters — was *not* shipped
    // under that exact name. So #651 ships ONE new Aura
    // primitive + 3 new foundation atomics.
    //
    // The remaining #651 AC1 (block_gc trampoline real
    // deferral + gc_state phase integration) + AC2
    // (aura_evaluator_request_gc_safepoint pending panic +
    // depth > 0 check + defer/yield/retry) + AC3 (fiber
    // check_gc_safepoint + scheduler wait_for_safepoint
    // pending-panic awareness) work is invasive C++ on
    // evaluator_fiber_mutation.cpp +
    // block_gc_for_pending_checkpoint_trampoline +
    // aura_evaluator_request_gc_safepoint + fiber
    // check_gc_safepoint + scheduler wait_for_safepoint +
    // needs the panic during MutationBoundary + concurrent
    // GC + steal matrix + TSan coverage from the issue body
    // — separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:gc-panic-deferral-stats", [&ev](const auto&) -> EvalValue {
            // pending-panic-deferral: new foundation atomic
            // (0 until AC1 block_gc trampoline wire-up).
            const std::uint64_t pending_panic_deferral =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->gc_panic_pending_deferral_total.load(std::memory_order_relaxed)
                    : 0;
            // gc-blocked-by-panic: new foundation atomic
            // (0 until AC2 aura_evaluator_request_gc_safepoint wire-up).
            const std::uint64_t gc_blocked_by_panic =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->gc_blocked_by_panic_total.load(std::memory_order_relaxed)
                    : 0;
            // conflicts-resolved: new foundation atomic
            // (0 until AC3 panic + GC conflict resolution wire-up).
            const std::uint64_t conflicts_resolved =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->gc_panic_conflict_resolved_total.load(std::memory_order_relaxed)
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
            insert_kv("pending-panic-deferral", static_cast<std::int64_t>(pending_panic_deferral));
            insert_kv("gc-blocked-by-panic", static_cast<std::int64_t>(gc_blocked_by_panic));
            insert_kv("conflicts-resolved", static_cast<std::int64_t>(conflicts_resolved));
            insert_kv("schema", 651);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 28 (orig lines 4114-4264)
void ObservabilityPrims::register_eval_p28(PrimRegistrar add, Evaluator& ev) {

    // Issue #589: query:envframe-dualpath-enforce-stats —
    // Agent-discoverable structured dashboard for the SoA
    // EnvFrame/EnvId dual-path bindings_ vs bindings_symid_
    // consistency + version stamping + stale refresh in
    // materialize_call_env & GCEnvWalkFn (P0 Runtime-Review +
    // SoA production-readiness surface — non-duplicative to
    // existing #543 SoA EnvFrame, #568 children SoA, #205
    // GCEnvWalkFn).
    //
    // Note the relationship to existing primitives:
    //   - (query:envframe-dualpath-stats) — base flat-int
    //     dualpath primitive (the AC4 surface)
    //   - (query:envframe-dualpath-stale-stats) — existing
    //     flat-int stale summary
    //   - (query:envframe-dualpath-stale-stats-hash) (#647)
    //     — stale enforcement-layer hash (cross-fiber /
    //     version mismatch / dualpath-repair counters)
    //   - (query:envframe-dualpath-enforce-stats) (#589,
    //     this primitive) — *enforce-track* companion with
    //     `-enforce-` midfix that focuses on the AC1+AC2+AC3
    //     counters for bind/bind_symid mirror writes +
    //     materialize_call_env dual-path refresh + GCEnvWalkFn
    //     consistency violations (the SoA dual-path
    //     consistency enforcement layer that #647's
    //     `-stale-` midfix does not cover).
    //
    // Fields (3 + sentinel):
    //   - mirror-write        new envframe_dualpath_mirror_
    //                          write_total atomic (foundation
    //                          for AC1 bind/bind_symid mirror
    //                          writes). Value is 0 until AC1
    //                          wire-up.
    //   - dualpath-refresh    new envframe_dualpath_refresh_
    //                          total atomic (foundation for
    //                          AC2 materialize_call_env
    //                          refresh_dual_path_from_soa
    //                          helper calls). Value is 0
    //                          until AC2 wire-up.
    //   - consistency-violations
    //                          new envframe_dualpath_
    //                          consistency_violations_total
    //                          atomic (foundation for AC3
    //                          GCEnvWalkFn consistency
    //                          violations caught). Value
    //                          is 0 until AC3 wire-up.
    //   - schema == 589         sentinel for Agent drift
    //                          detection (back to a lower
    //                          number than #651 since #589
    //                          is an older issue that
    //                          reaches observability
    //                          foundation layer late — the
    //                          schema sentinel still
    //                          matches the issue number for
    //                          Agent drift tracking).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // dual-path observability surface:
    //   - (query:envframe-dualpath-stats) — base flat-int
    //     dualpath primitive (the AC4 surface)
    //   - (query:envframe-dualpath-stale-stats) — existing
    //     flat-int stale summary
    //   - (query:envframe-dualpath-stale-stats-hash) (#647)
    //     — stale enforcement-layer hash
    //   - (query:envframe-stale-stats) — stale refresh stats
    //   - (query:envframe-bump-stats) — bump stats
    //   - #543 SoA EnvFrame foundation
    //   - #568 children SoA
    //   - #205 GCEnvWalkFn foundation
    //   - envframe_desync_detected_ / envframe_stale_refresh_
    //     count_ / envframe_post_rollback_invalidations_ +
    //     envframe_version_mismatch_in_walk_ +
    //     envframe_gc_walk_safe_skips_ + gc_envframe_stale_
    //     skipped_ (existing counters that #589 AC1+AC2+AC3
    //     will exercise)
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:envframe-dualpath-stats` — already
    // ships as the base flat-int primitive. #589 ships the
    // AC1+AC2+AC3 enforcement-layer companion with a
    // distinct name (`-enforce-` midfix).
    //
    // The remaining #589 AC1 (Env::bind_symid / bind always
    // mirror + owner_ set stamp defuse_version_ into
    // env_version_) + AC2 (materialize_call_env on version
    // mismatch call refresh_dual_path_from_soa helper) +
    // AC3 (walk_env_frames / GCEnvWalkFn before emitting
    // roots refresh or skip with metric) work is invasive
    // C++ on evaluator.ixx + evaluator_impl.cpp +
    // gc_coordinator.h + needs the large env chains +
    // mutate + compaction/GC matrix + 5000+ materialize
    // under fibers + TSan coverage from the issue body —
    // separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dualpath-enforce-stats", [&ev](const auto&) -> EvalValue {
            // mirror-write: new foundation atomic
            // (0 until AC1 bind/bind_symid mirror wire-up).
            const std::int64_t mirror_write =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->envframe_dualpath_mirror_write_total.load(std::memory_order_relaxed)
                    : 0;
            // dualpath-refresh: new foundation atomic
            // (0 until AC2 materialize_call_env refresh wire-up).
            const std::int64_t dualpath_refresh =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->envframe_dualpath_refresh_total.load(std::memory_order_relaxed)
                    : 0;
            // consistency-violations: new foundation atomic
            // (0 until AC3 GCEnvWalkFn consistency violation wire-up).
            const std::int64_t consistency_violations =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->envframe_dualpath_consistency_violations_total.load(
                              std::memory_order_relaxed)
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
            insert_kv("mirror-write", static_cast<std::int64_t>(mirror_write));
            insert_kv("dualpath-refresh", static_cast<std::int64_t>(dualpath_refresh));
            insert_kv("consistency-violations", static_cast<std::int64_t>(consistency_violations));
            insert_kv("schema", 589);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 29 (orig lines 4265-4406)
void ObservabilityPrims::register_eval_p29(PrimRegistrar add, Evaluator& ev) {

    // Issue #590: query:aot-hotupdate-stats — Agent-
    // discoverable structured dashboard for the AOT mangle
    // versioning + region filtering + multi-agent hot-update
    // isolation + closure dispatch stale detection (P0
    // Runtime-Review + AOT production-readiness surface —
    // non-duplicative to existing #544 / #323 / #287).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:aot-reload-stats) (#708) — 5-field high-level
    //     reload summary
    //   - (query:aot-reload-func-table-stats) (#644) —
    //     func_table refcount + region filter primitive
    //   - (query:aot-hot-reload-stats) (#358/#452) — earlier
    //     AOT hot-reload primitive
    //   - (query:aot-checkpoint-version-stats) (#708) —
    //     checkpoint version tracking
    //   - (query:aot-hotupdate-stats) (#590, this primitive)
    //     — *multi-agent hot-update isolation* companion with
    //     no `-reload-` midfix that focuses on the AC1+AC2+AC3
    //     counters for region-isolated reload + dispatch
    //     stale prevention + multi-agent reload cycles.
    //
    // Fields (3 + sentinel):
    //   - region-isolation     new aot_hotupdate_region_
    //                          isolation_total atomic
    //                          (foundation for AC1 region
    //                          isolation hits — reload only
    //                          affected target region).
    //                          Value is 0 until AC1 wire-up.
    //   - dispatch-stale      new aot_hotupdate_dispatch_
    //                          stale_prevented_total atomic
    //                          (foundation for AC3 closure
    //                          dispatch stale mismatch
    //                          prevented). Value is 0 until
    //                          AC3 wire-up.
    //   - multi-agent-reload  new aot_hotupdate_multi_agent_
    //                          reload_total atomic
    //                          (foundation for AC1 successful
    //                          multi-agent reload cycles).
    //                          Value is 0 until AC1 wire-up.
    //   - schema == 590         sentinel for Agent drift
    //                          detection (matches issue
    //                          number for Agent drift
    //                          tracking).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // AOT hot-update observability surface:
    //   - (query:aot-reload-stats) (#708) — 5-field
    //     reload summary
    //   - (query:aot-reload-func-table-stats) (#644) —
    //     func_table refcount + region filter primitive
    //   - (query:aot-hot-reload-stats) (#358/#452) —
    //     earlier AOT hot-reload primitive
    //   - (query:aot-checkpoint-version-stats) (#708) —
    //     checkpoint version tracking
    //   - aot_emit_version + runtime defuse_version_ +
    //     aot_reload_attempts_ + aot_hot_update_success_ +
    //     aot_stale_reject_count_ + aot_refcount_swaps_ +
    //     aot_region_mismatch_ (#708) — existing counters
    //   - mangle_aot_name (with emit_version + module_version)
    //   - aura_reload_aot_module (dlopen + aot_emit_version
    //     check + g_aot_module_version)
    // What the issue body AC2 specifies by **exact name +
    // fields** — `query:aot-hotupdate-stats` (no `-reload-`
    // midfix) with reload_success + stale_reject +
    // region_isolation_hits + dispatch_stale_prevented —
    // was *not* shipped under that exact name. The existing
    // #708 5-field summary already covers some of these
    // counters in aggregate; #590 ships the multi-agent
    // hot-update isolation focused primitive.
    //
    // The remaining #590 AC1 (mangle_aot_name +
    // generate_registration_c add region/agent_id prefix +
    // reload success iterate func_table rebind matching
    // version/region with refcounts) + AC2 ((aot:reload-
    // with-region path version region) primitive wire-up) +
    // AC3 (closure dispatch version check on func_id
    // lookup; on mismatch force deopt or error with metric)
    // work is invasive C++ on aura_jit_bridge.cpp +
    // mangle_aot_name + generate_registration_c + closure
    // dispatch path + needs the multi-agent region matrix +
    // 1000+ reload cycles + concurrent mutate/eval + TSan
    // coverage from the issue body — separate follow-ups.
    ObservabilityPrims::register_stats_impl(
        "query:aot-hotupdate-stats", [&ev](const auto&) -> EvalValue {
            // region-isolation: new foundation atomic
            // (0 until AC1 region isolation wire-up).
            const std::int64_t region_isolation =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->aot_hotupdate_region_isolation_total.load(std::memory_order_relaxed)
                    : 0;
            // dispatch-stale: new foundation atomic
            // (0 until AC3 dispatch stale prevention wire-up).
            const std::int64_t dispatch_stale =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->aot_hotupdate_dispatch_stale_prevented_total.load(
                              std::memory_order_relaxed)
                    : 0;
            // multi-agent-reload: new foundation atomic
            // (0 until AC1 multi-agent reload wire-up).
            const std::int64_t multi_agent_reload =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->aot_hotupdate_multi_agent_reload_total.load(std::memory_order_relaxed)
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
            insert_kv("region-isolation", static_cast<std::int64_t>(region_isolation));
            insert_kv("dispatch-stale", static_cast<std::int64_t>(dispatch_stale));
            insert_kv("multi-agent-reload", static_cast<std::int64_t>(multi_agent_reload));
            insert_kv("schema", 590);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 30 (orig lines 4407-4467)
void ObservabilityPrims::register_eval_p30(PrimRegistrar add, Evaluator& ev) {

    // Issue #593: query:pattern-ir-hygiene-closed-loop-stats — AST→query→IR
    // MacroIntroduced hygiene closed-loop companion (non-duplicative with
    // #524 macro-production-hygiene-stats, #547 pattern-hygiene-stats,
    // #501 ir-hygiene-stats, #420 macro-hygiene-contract-stats).
    //
    // Fields (3 + sentinel):
    //   - capture-prevented   pattern_ir_capture_prevented_total
    //   - ir-post-mutate-violation  ir_hygiene_post_mutate_violation_total
    //   - tag-arity-delta     tag_arity_hygiene_query_delta_total
    //   - schema == 593
    ObservabilityPrims::register_stats_impl(
        "query:pattern-ir-hygiene-closed-loop-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t capture_prevented =
                m ? static_cast<std::int64_t>(
                        m->pattern_ir_capture_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t ir_violation =
                m ? static_cast<std::int64_t>(
                        m->ir_hygiene_post_mutate_violation_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t tag_delta =
                m ? static_cast<std::int64_t>(
                        m->tag_arity_hygiene_query_delta_total.load(std::memory_order_relaxed))
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
            insert_kv("capture-prevented", capture_prevented);
            insert_kv("ir-post-mutate-violation", ir_violation);
            insert_kv("tag-arity-delta", tag_delta);
            insert_kv("schema", 593);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 31 (orig lines 4468-4536)
void ObservabilityPrims::register_eval_p31(PrimRegistrar add, Evaluator& ev) {

    // Issue #596: query:guard-panic-reflect-stats — Guard + panic checkpoint +
    // reflect/schema validation + fiber resume closed-loop companion
    // (non-duplicative with #548 panic-checkpoint-lifecycle-stats,
    // #594 reflection-selfmod-stats, #592 fiber resume safety matrix).
    //
    // Fields (5 + sentinel):
    //   - checkpoints-committed   panic_checkpoint_commit_count_
    //   - restores-on-resume      guard_panic_reflect_restores_on_resume_total
    //   - validation-pass         schema_validation_pass_count_
    //   - validation-fail         schema_validation_fail_count_
    //   - boundary-violation-prevented
    //                             guard_panic_reflect_boundary_violation_prevented_total
    //   - schema == 596
    ObservabilityPrims::register_stats_impl(
        "query:guard-panic-reflect-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t commits =
                static_cast<std::int64_t>(ev.get_panic_checkpoint_commit_count());
            const std::int64_t restores_on_resume =
                m ? static_cast<std::int64_t>(m->guard_panic_reflect_restores_on_resume_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t validation_pass =
                static_cast<std::int64_t>(ev.get_schema_validation_pass_count());
            const std::int64_t validation_fail =
                static_cast<std::int64_t>(ev.get_schema_validation_fail_count());
            const std::int64_t boundary_prevented =
                m ? static_cast<std::int64_t>(
                        m->guard_panic_reflect_boundary_violation_prevented_total.load(
                            std::memory_order_relaxed))
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
            insert_kv("checkpoints-committed", commits);
            insert_kv("restores-on-resume", restores_on_resume);
            insert_kv("validation-pass", validation_pass);
            insert_kv("validation-fail", validation_fail);
            insert_kv("boundary-violation-prevented", boundary_prevented);
            insert_kv("schema", 596);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
