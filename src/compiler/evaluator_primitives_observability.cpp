// evaluator_primitives_observability.cpp — P0 step 15: panic / stats / jit / gc-arena observability
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "ci_build_info.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

void register_eval_observability_primitives(PrimRegistrar add, Evaluator& ev) {

    auto meta_to_pair = [&ev](const PrimMeta& m) -> EvalValue {
        auto doc_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(m.doc);
        auto pid1 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(m.safety_flags), make_string(doc_idx)});
        auto pid2 = ev.pairs_.size();
        ev.pairs_.push_back({make_bool(m.pure), make_pair(pid1)});
        auto pid3 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(m.arity), make_pair(pid2)});
        return make_pair(pid3);
    };

    // (typecheck-status) — Returns the last mutate typecheck result.
    // Empty string = no errors, non-empty = last mutate caused type errors.
    add("typecheck-status", [&ev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        if (ev.last_mutate_error_.empty()) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("ok");
            return make_string(sidx);
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.last_mutate_error_);
        return make_string(sidx);
    });

    // (auto-rollback-on-panic [#t|#f]) — Get/set auto-rollback on panic flag
    // When enabled, runtime error triggers automatic rollback to last safe
    // checkpoint. Returns previous value.
    add("auto-rollback-on-panic", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool old = ev.panic_auto_rollback_;
        if (!a.empty() && types::is_bool(a[0]))
            ev.panic_auto_rollback_ = types::as_bool(a[0]);
        return make_bool(old);
    });

    // (panic-auto-rollback?) — Query current auto-rollback state
    add("panic-auto-rollback?",
                    [&ev](const auto&) -> EvalValue { return make_bool(ev.panic_auto_rollback_); });

    // (panic-checkpoint) — Save current workspace as a safe checkpoint
    // Returns #t on success, #f if no workspace loaded.
    add("panic-checkpoint", [&ev](const auto&) -> EvalValue {
        return make_bool(ev.save_panic_checkpoint());
    });

    // (panic-restore) — Restore to the last safe checkpoint
    // Returns #t on success, #f if no checkpoint available or restore failed.
    add("panic-restore", [&ev](const auto&) -> EvalValue {
        return make_bool(ev.restore_panic_checkpoint());
    });

    // (panic-safe-source) — Return the checkpoint source code
    // Returns empty string if no checkpoint.
    add("panic-safe-source", [&ev](const auto&) -> EvalValue {
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.panic_safe_source_);
        return make_string(idx);
    });

    // Issue #480: (primitive:describe name) — return PrimMeta as
    // (arity . (pure . (safety-flags . doc-string))).
    ev.primitives_.add(
        "primitive:describe",
        [&ev, meta_to_pair](const auto& a) -> EvalValue {
            if (a.size() != 1 || !is_string(a[0]))
                return make_void();
            const auto& heap = ev.string_heap_;
            const auto idx = as_string_idx(a[0]);
            if (idx >= heap.size())
                return make_void();
            const auto& name = heap[idx];
            const auto slot = ev.primitives_.slot_for_name(name);
            if (slot >= ev.primitives_.slot_count())
                return make_void();
            ev.bump_primitive_describe_count();
            return meta_to_pair(ev.primitives_.meta_for_slot(slot));
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Return metadata for a registered primitive by name."});

    // Issue #480: (query:primitive-list-with-meta) — list of
    // (name . meta-pair) for every registered primitive.
    ev.primitives_.add(
        "query:primitive-list-with-meta",
        [&ev, meta_to_pair](const auto& a) -> EvalValue {
            (void)a;
            ev.bump_primitive_list_meta_count();
            EvalValue result = make_void();
            for (std::size_t slot = ev.primitives_.slot_count(); slot-- > 0;) {
                const auto& name = ev.primitives_.name_for_slot(slot);
                auto nidx = ev.string_heap_.size();
                ev.string_heap_.push_back(name);
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({make_string(nidx), meta_to_pair(ev.primitives_.meta_for_slot(slot))});
                auto wrap = ev.pairs_.size();
                ev.pairs_.push_back({make_pair(pid), result});
                result = make_pair(wrap);
            }
            return result;
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "List every primitive with its PrimMeta pair."});

    // Issue #478: query:primitive-error-stats — returns a pair
    // (error-count . error-values-size) for Agent recovery loops.
    add("query:primitive-error-stats", [&ev](const auto&) -> EvalValue {
        auto count = static_cast<std::int64_t>(ev.get_primitive_error_count());
        auto stored = static_cast<std::int64_t>(ev.get_primitive_error_values_size());
        auto pid = ev.pairs_.size();
        ev.pairs_.push_back({make_int(count), make_int(stored)});
        return make_pair(pid);
    });

    // (query:primitive-perf-stats) — Issue #441 (rolled into
    // #450): hot-path primitive dispatch stats. Returns a
    // hash with 2 fields:
    //   - primitive-call-total: lifetime count of every
    //     (primitive-func args...) dispatch (bumped in
    //     evaluator_eval_flat.cpp at the dispatch site)
    //   - primitive-count: # of registered primitives
    //     (snapshot at primitive-registration time; gives
    //     a per-primitive average call rate when paired
    //     with primitive-call-total)
    //
    // Per-primitive breakdown (which primitive is hottest)
    // is a follow-up issue — requires a vector of atomics
    // indexed by slot, which is invasive to the Primitives
    // class. For the MVP, the aggregate counter is enough
    // to answer "is the primitive-dispatch hot path actually
    // hot in this session?".
    add("query:primitive-perf-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t call_total = 0;
        std::uint64_t prim_count = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            call_total = m->primitive_call_total.load(std::memory_order_relaxed);
        }
        prim_count = ev.primitives_.slot_count();
        std::int64_t avg_per_prim = prim_count > 0
            ? static_cast<std::int64_t>(call_total / prim_count)
            : 0;
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"primitive-call-total", make_int(static_cast<std::int64_t>(call_total))},
            {"primitive-count", make_int(static_cast<std::int64_t>(prim_count))},
            {"avg-per-prim", make_int(avg_per_prim)},
        };
        return build_hash(kv);
    });

    // (query:aot-stats) — Issue #452: AOT hot-update + region
    // filtering observability. Returns a 3-field hash:
    //   - aot-stale-reject-count: lifetime count of
    //     aura_reload_aot_module rejections due to
    //     aot_emit_version mismatch (bumped in
    //     aura_jit_bridge.cpp)
    //   - aot-region-mismatch-count: lifetime count of
    //     region_filter mismatches (currently 0 — region
    //     wiring is a follow-up; counter is in place
    //     so the day it ships, observability is immediate)
    //   - aot-hot-update-success-count: lifetime count of
    //     successful dlopen + version check + constructor
    //     invocation.
    //
    // This is the AI Agent's signal for "is the AOT
    // hot-update pipeline behaving correctly?". A rising
    // stale-reject count without rising success count =
    // version drift (the bug pattern from #452's body).
    add("query:aot-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t stale_rej = 0;
        std::uint64_t region_mismatch = 0;
        std::uint64_t hot_update_ok = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            stale_rej = m->aot_stale_reject_count_.load(std::memory_order_relaxed);
            region_mismatch = m->aot_region_mismatch_.load(std::memory_order_relaxed);
            hot_update_ok = m->aot_hot_update_success_.load(std::memory_order_relaxed);
        }
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"aot-stale-reject-count", make_int(static_cast<std::int64_t>(stale_rej))},
            {"aot-region-mismatch-count", make_int(static_cast<std::int64_t>(region_mismatch))},
            {"aot-hot-update-success-count", make_int(static_cast<std::int64_t>(hot_update_ok))},
        };
        return build_hash(kv);
    });

    // (query:ci-reproducibility-stats) — Issue #675: build/CI
    // reproducibility + sanitizer gate observability. Returns a
    // 5-field hash:
    //   - source-date-epoch: SOURCE_DATE_EPOCH env (0 if unset)
    //   - build-type: AURA_BUILD_TYPE env (or "unknown")
    //   - sanitizer-mode: compile-time "none"|"asan"|"ubsan"|"tsan"
    //   - reproducible-flags-active: 1 iff SOURCE_DATE_EPOCH > 0
    //   - ccache-disabled: 1 iff CCACHE_DISABLE=1
    add("query:ci-reproducibility-stats", [&ev](const auto&) -> EvalValue {
        const auto epoch = aura::ci::source_date_epoch();
        const auto repro = aura::ci::reproducible_flags_active();
        const auto ccache_off = aura::ci::ccache_disabled();
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        auto bt_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(aura::ci::build_type_from_env());
        auto san_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(aura::ci::sanitizer_mode());
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"source-date-epoch", make_int(epoch)},
            {"build-type", make_string(bt_idx)},
            {"sanitizer-mode", make_string(san_idx)},
            {"reproducible-flags-active", make_bool(repro)},
            {"ccache-disabled", make_bool(ccache_off)},
        };
        return build_hash(kv);
    });

    // (query:shape-folding-stats) — Issue #462: observability
    // for ShapeAwareFoldingPass. Returns a 4-field hash:
    //   - shape-fold-count: lifetime # of instructions
    //     replaced (OpNop'd) due to shape/linear/narrow
    //     metadata
    //   - shape-linear-elide-count: subset of fold-count
    //     due to linear-ownership elision (MoveOp on
    //     non-escaping Owned slot is a no-op)
    //   - shape-narrow-check-count: # of redundant
    //     type-checks detected (counted, not yet rewritten
    //     in Cycle 1; rewrite is #462 follow-up)
    //   - guard-shape-hits: # of GuardShape instructions
    //     seen in the module (signal for downstream passes
    //     to trust per-block shape_id)
    //
    // This is the AI Agent's signal for "is the
    // shape-aware folding pass doing useful work?".
    // Cycle 2 (separate issue) will add per-shape-id
    // OpAdd unchecked specialization + the narrow-evidence
    // rewrite. The counter layer is in place.
    add("query:shape-folding-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t fold = 0;
        std::uint64_t linear_elide = 0;
        std::uint64_t narrow = 0;
        std::uint64_t guard_hits = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            fold = m->shape_fold_count.load(std::memory_order_relaxed);
            linear_elide = m->shape_linear_elide_count.load(std::memory_order_relaxed);
            narrow = m->shape_narrow_check_count.load(std::memory_order_relaxed);
            guard_hits = m->guard_shape_hits.load(std::memory_order_relaxed);
        }
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"shape-fold-count", make_int(static_cast<std::int64_t>(fold))},
            {"shape-linear-elide-count", make_int(static_cast<std::int64_t>(linear_elide))},
            {"shape-narrow-check-count", make_int(static_cast<std::int64_t>(narrow))},
            {"guard-shape-hits", make_int(static_cast<std::int64_t>(guard_hits))},
        };
        return build_hash(kv);
    });

    // (query:soa-adoption-stats) — Issue #463: SoA Phase 2
    // adoption observability. Returns a 3-field hash:
    //   - soa-functions-visited: lifetime # of SoA
    //     functions walked by the bridge pass
    //   - soa-instructions-visited: lifetime # of SoA
    //     instructions walked
    //   - aos-view-built-count: lifetime # of SoA→AoS
    //     view conversions
    //
    // This is the AI Agent's signal for "is the SoA
    // rollout progressing?". A rising
    // soa-instructions-visited count with no
    // aos-view-built-count growth means the SoA path is
    // being used end-to-end (the AoS view is a one-time
    // scaffold; subsequent cycles replace it with
    // SoA-aware Pass overloads).
    add("query:soa-adoption-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t funcs = 0;
        std::uint64_t instrs = 0;
        std::uint64_t views = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            funcs = m->soa_functions_visited.load(std::memory_order_relaxed);
            instrs = m->soa_instructions_visited.load(std::memory_order_relaxed);
            views = m->aos_view_built_count.load(std::memory_order_relaxed);
        }
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"soa-functions-visited", make_int(static_cast<std::int64_t>(funcs))},
            {"soa-instructions-visited", make_int(static_cast<std::int64_t>(instrs))},
            {"aos-view-built-count", make_int(static_cast<std::int64_t>(views))},
        };
        return build_hash(kv);
    });

    // (query:arena-auto-stats) — Issue #464: Arena
    // auto-compaction lifecycle observability. Returns a
    // 4-field hash:
    //   - auto-compact-guard-call-count: lifetime # of
    //     times MutationBoundaryGuard dtor bumped the
    //     closed-loop signal (one bump per outermost +
    //     success guard exit)
    //   - compaction-yield-checks: lifetime # of times
    //     auto_compact_with_safety() observed a fiber
    //     context (g_current_fiber != nullptr); the actual
    //     yield-during-compact is a #464 follow-up
    //   - auto-compact-trigger-count: lifetime # of
    //     triggered compactions (from ArenaGroup)
    //   - auto-compact-skip-count: lifetime # of
    //     skipped triggers (below adaptive threshold)
    //
    // This is the AI Agent's signal for "is the
    // arena auto-compaction lifecycle working as
    // expected?". Cycle 2 (separate issue) will add
    // the actual auto_compact_with_safety() call from
    // the scheduler + the fiber-yield integration.
    add("query:arena-auto-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t guard_calls = 0;
        std::uint64_t yield_checks = 0;
        std::uint64_t trigger_count = 0;
        std::uint64_t skip_count = 0;
        // Read all 4 counters directly from the ArenaGroup
        // (the bump happens in MutationBoundaryGuard dtor
        // on ev_->arena_group_). The compiler_metrics_
        // field is the in-process metrics struct used by
        // the snapshot() helper; for EDSL primitives we
        // read from the source of truth (ArenaGroup) so
        // the counter advances immediately without
        // requiring a metrics copy.
        guard_calls = ev.arena_group().auto_compact_guard_call_count();
        yield_checks = ev.arena_group().compaction_yield_checks_group();
        trigger_count = ev.arena_group().auto_compact_trigger_count();
        skip_count = ev.arena_group().auto_compact_skip_count();
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"auto-compact-guard-call-count", make_int(static_cast<std::int64_t>(guard_calls))},
            {"compaction-yield-checks", make_int(static_cast<std::int64_t>(yield_checks))},
            {"auto-compact-trigger-count", make_int(static_cast<std::int64_t>(trigger_count))},
            {"auto-compact-skip-count", make_int(static_cast<std::int64_t>(skip_count))},
        };
        return build_hash(kv);
    });

    // (query:cxx26-hotpath-invariants) — Issue #465: C++26
    // hot-path Contracts + consteval invariants observability.
    // Returns a 5-field hash reporting the compile-time
    // invariants the binary was built with:
    //   - fixnum-tag-encoding: 0 (matches low2 dispatch table[0])
    //   - ref-tag-encoding: 1 (matches low2 dispatch table[1])
    //   - string-v2-tag-encoding: 2 (matches low2 dispatch table[2])
    //   - special-tag-encoding: 3 (matches low2 dispatch table[3])
    //   - float-tag-encoding: 4 (out of low2 dispatch space)
    //
    // These are static_assert'd at compile time in
    // value_tags.h. The primitive reports the values that
    // were baked in at build time, so the AI Agent can
    // verify a deployed binary matches the expected
    // encoding without re-running the static_asserts.
    //
    // Future follow-ups will add:
    //   - The Pass concept instance count
    //   - The SoA column count
    //   - The dirty bitmask byte width
    add("query:cxx26-hotpath-invariants", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht) return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF) fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp; keys[idx] = key_ev.val;
                        vals[idx] = v.val; ht->size++;
                        inserted = true; break;
                    }
                }
                if (!inserted) { FlatHashTable::destroy(ht); return make_void(); }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        // These values are the ones static_assert'd in
        // value_tags.h. The build will fail if they drift.
        // We hardcode the values here because the
        // EvalValueTag enum is in value.ixx (a different
        // module partition) and not directly accessible
        // from this file. The static_assert chain in
        // value_tags.h is the source of truth; the
        // primitive reports the same values so the
        // AI Agent can verify a deployed binary matches
        // the expected encoding.
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"fixnum-tag-encoding", make_int(0)},
            {"ref-tag-encoding", make_int(1)},
            {"string-v2-tag-encoding", make_int(2)},
            {"special-tag-encoding", make_int(3)},
            {"float-tag-encoding", make_int(4)},
        };
        return build_hash(kv);
    });

    // (atomic-batch:stats) — Issue #192: observability for
    // mutate:atomic-batch. Hash with batch-count, ops-total,
    // rollback-count, ops-per-batch (avg).
    add("atomic-batch:stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            // Issue #394 / #258: capacity 32 (was 8). This primitive
            // returns 5 keys; cap-8 + FNV-1a probing occasionally
            // failed to insert a key, so hash-ref returned void.
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
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
        };
        std::size_t avg =
            ev.atomic_batch_count_ > 0 ? ev.atomic_batch_ops_total_ / ev.atomic_batch_count_ : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"batch-count", make_int(static_cast<std::int64_t>(ev.atomic_batch_count_))},
            {"ops-total", make_int(static_cast<std::int64_t>(ev.atomic_batch_ops_total_))},
            {"rollback-count", make_int(static_cast<std::int64_t>(ev.atomic_batch_rollbacks_))},
            {"ops-per-batch", make_int(static_cast<std::int64_t>(avg))},
            // Issue #250: how many per-op generation bumps the
            // batches suppressed (lifetime total). Useful for
            // dashboards ("how much churn did batching save?").
            {"bumps-saved-total",
             make_int(static_cast<std::int64_t>(ev.atomic_batch_bumps_saved_total_))},
            // Issue #396 Phase 3: heuristic for "ran under
            // concurrent fiber pressure". Bumped when the
            // bridge fiber setter was active at commit time
            // (i.e. serve mode + fiber context). Stays 0 in
            // test-binary paths where the hook is null.
            // Name matches the issue's proposed field.
            {"executed-under-concurrent-fiber",
             make_int(static_cast<std::int64_t>(ev.atomic_batch_in_fiber_total_))},
        };
        return build_hash(kv);
    });

    // (closure:stats) — Issue #252: observability for
    // apply_closure dual-path. Hash with the 5 counters
    // (calls-total, ffi-calls, tw-calls, bridge-calls,
    // stale-returns) + bridge-fraction (helper for
    // dashboards: how much of the dispatch goes to the
    // bridge path, which is the slowest).
    add("closure:stats", [&ev](const auto&) -> EvalValue {
        // Re-use the build_hash from atomic-batch:stats
        // (defined in the lambda above). It's the same code
        // pattern, so we re-bind to keep the closure:stats
        // self-contained.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            // Issue #394 / #258: capacity 32 (was 8). closure:stats
            // returns 7 keys; cap-8 insertion failures broke hash-ref.
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
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
        };
        // Issue #252: closure stats. Read from ev.compiler_metrics_
        // (shared with the IR's IROpcode::Call/Apply). If metrics
        // is not set (legacy standalone use), all counters are 0.
        std::uint64_t calls = 0, ffi_c = 0, tw_c = 0, ir_c = 0;
        std::uint64_t bridge_c = 0, stale_c = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            calls = m->closure_calls_total.load(std::memory_order_relaxed);
            ffi_c = m->closure_ffi_calls.load(std::memory_order_relaxed);
            tw_c = m->closure_tw_calls.load(std::memory_order_relaxed);
            ir_c = m->closure_ir_calls.load(std::memory_order_relaxed);
            bridge_c = m->closure_bridge_calls.load(std::memory_order_relaxed);
            stale_c = m->closure_stale_returns.load(std::memory_order_relaxed);
        }
        std::uint64_t bridge = bridge_c;
        // bridge-fraction * 100 (integer percent). 0 if no calls.
        std::int64_t bridge_pct = calls > 0 ? static_cast<std::int64_t>((bridge * 100) / calls) : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"calls-total", make_int(static_cast<std::int64_t>(calls))},
            {"ffi-calls", make_int(static_cast<std::int64_t>(ffi_c))},
            {"tw-calls", make_int(static_cast<std::int64_t>(tw_c))},
            {"ir-calls", make_int(static_cast<std::int64_t>(ir_c))},
            {"bridge-calls", make_int(static_cast<std::int64_t>(bridge))},
            {"stale-returns", make_int(static_cast<std::int64_t>(stale_c))},
            {"bridge-fraction-pct", make_int(bridge_pct)},
        };
        return build_hash(kv);
    });

    // (query:closure-stats) — Issue #428: unified closure
    // observability surface in the query: family. Returns
    // a hash with 9 fields covering both the dispatch
    // (closure:stats 7 fields) and the bridge_epoch drift
    // (bridge-epoch-hits, bridge-epoch-drift-pct). The
    // drift is the percent of bridge_epoch checks that
    // observed a stale epoch (vs hits which observed
    // fresh) — the AI Agent's primary signal for
    // "is the bridge falling behind the mutation rate?".
    //
    // Field list (9 total):
    //   - calls-total:           every apply_closure call
    //   - ffi-calls:             FFI-dispatched
    //   - tw-calls:              tree-walker closures_ map hit
    //   - ir-calls:              IR runtime_closures_ hit
    //   - bridge-calls:          closure_bridge_ (IR/JIT)
    //   - stale-returns:         stale-bridge nullopt returns
    //   - bridge-fraction-pct:   bridge-calls / calls-total * 100
    //   - bridge-epoch-hits:     # of bridge_epoch checks
    //                            that succeeded (fresh)
    //   - bridge-epoch-drift-pct: stale-returns /
    //                            (bridge-epoch-hits + stale-returns)
    //                            * 100. The AI Agent's primary
    //                            signal — > 0 means the workspace
    //                            is mutating faster than closures
    //                            can refresh.
    //
    // Migration note: closure:stats is the pre-#428 primitive;
    // query:closure-stats is the new unified surface. Both
    // return the same hash shape; the new one just adds
    // 2 bridge_epoch fields. The old primitive stays for
    // backward compat (existing tests use closure:stats).
    add("query:closure-stats", [&ev](const auto&) -> EvalValue {
        // Reuse the same build_hash pattern as closure:stats.
        // Inline here (instead of refactoring closure:stats to
        // a helper) so the new primitive stays self-contained
        // and easy to evolve independently.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        };
        std::uint64_t calls = 0, ffi_c = 0, tw_c = 0, ir_c = 0;
        std::uint64_t bridge_c = 0, stale_c = 0;
        std::uint64_t bridge_epoch_hits = 0, bridge_epoch_drifts = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            calls = m->closure_calls_total.load(std::memory_order_relaxed);
            ffi_c = m->closure_ffi_calls.load(std::memory_order_relaxed);
            tw_c = m->closure_tw_calls.load(std::memory_order_relaxed);
            ir_c = m->closure_ir_calls.load(std::memory_order_relaxed);
            bridge_c = m->closure_bridge_calls.load(std::memory_order_relaxed);
            stale_c = m->closure_stale_returns.load(std::memory_order_relaxed);
            bridge_epoch_hits = m->bridge_epoch_hit_count_.load(std::memory_order_relaxed);
            bridge_epoch_drifts = m->closure_stale_refresh_count_.load(std::memory_order_relaxed);
        }
        std::int64_t bridge_pct = calls > 0 ? static_cast<std::int64_t>((bridge_c * 100) / calls) : 0;
        // Drift = stale-refreshes / (hits + drifts) * 100.
        // 0 if no checks yet (avoids divide-by-zero in the
        // dashboard, which would otherwise show NaN).
        std::uint64_t total_epoch_checks = bridge_epoch_hits + bridge_epoch_drifts;
        std::int64_t drift_pct = total_epoch_checks > 0
            ? static_cast<std::int64_t>((bridge_epoch_drifts * 100) / total_epoch_checks)
            : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"calls-total", make_int(static_cast<std::int64_t>(calls))},
            {"ffi-calls", make_int(static_cast<std::int64_t>(ffi_c))},
            {"tw-calls", make_int(static_cast<std::int64_t>(tw_c))},
            {"ir-calls", make_int(static_cast<std::int64_t>(ir_c))},
            {"bridge-calls", make_int(static_cast<std::int64_t>(bridge_c))},
            {"stale-returns", make_int(static_cast<std::int64_t>(stale_c))},
            {"bridge-fraction-pct", make_int(bridge_pct)},
            {"bridge-epoch-hits", make_int(static_cast<std::int64_t>(bridge_epoch_hits))},
            {"bridge-epoch-drift-pct", make_int(drift_pct)},
        };
        return build_hash(kv);
    });

}

void register_jit_arena_primitives(PrimRegistrar add, Evaluator& ev) {

    // (jit:intrinsic-count) — Issue #194: return the
    // runtime→intrinsic migration counter from the AuraJIT.
    // This is the per-commit observability signal for the 4
    // candidates the issue body tracks. Returns 0 if no hook
    // is installed (e.g. unit-test Evaluator without a JIT).
    add("jit:intrinsic-count", [&ev](const auto&) -> EvalValue {
        if (!ev.get_intrinsic_count_fn_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.get_intrinsic_count_fn_()));
    });

    // (jit:deopt-fn? fn-name threshold) — Issue #193: returns
    // #t if the function should be deopted (i.e., its
    // unhandled-opcode count exceeds the threshold). Default
    // threshold is 0 (any hit triggers deopt). Production code
    // should pass a non-zero threshold (e.g. 10) to avoid
    // thrashing on transient bugs during initial JIT warmup.
    add("jit:deopt-fn?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        std::uint64_t threshold = 0;
        if (a.size() >= 2 && is_int(a[1])) {
            auto t = as_int(a[1]);
            if (t < 0)
                t = 0;
            threshold = static_cast<std::uint64_t>(t);
        }
        // The intrinsic_count check would need a separate hook
        // for the per-function unhandled count. For now, we
        // look up via the AuraJIT if it's installed. If the
        // hook isn't installed, default to false (never deopt).
        if (!ev.get_jit_unhandled_count_fn_)
            return make_bool(false);
        auto count = ev.get_jit_unhandled_count_fn_(ev.string_heap_[idx].c_str());
        return make_bool(count > threshold);
    });

    // (jit:exception-depth) — Issue #195: current fiber's
    // exception stack depth. Reads from the per-fiber ExStack
    // via the JIT runtime's hook (aura_fiber_current_id).
    // Returns 0 if no exception state for the current fiber.
    add("jit:exception-depth", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(aura_exception_depth()));
    });

    // (jit:exception-fibers) — Issue #195: number of distinct
    // fiber ids that have exception state. Used for
    // observability of the per-fiber ExStack map size.
    add("jit:exception-fibers", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(aura_exception_fiber_count()));
    });

    // (jit:exception-fibers-clear) — Issue #195: clear all
    // per-fiber exception state. Returns void. Used by the
    // session-reset path; safe to call from Aura code.
    add("jit:exception-fibers-clear", [&ev](const auto&) -> EvalValue {
        aura_exception_clear_all();
        return make_void();
    });

    // (query:jit-stats) — Issue #427: full JIT metrics line
    // in the same format AuraJIT::Metrics::format produces.
    // Returns a single string with key=value fields separated
    // by spaces. Includes: compiles, avg_us, hot_swaps,
    // cached_fns, inlined_prims, slow_prims, prim_calls,
    // prim_avg_ns, verify_fail, add_mod_fail, unhandled_opcode,
    // intrinsics. Returns "" if no hook is installed (e.g.
    // unit-test Evaluator without a JIT). Cheap to call —
    // just reads a thread-local buffer populated by the hook.
    add("query:jit-stats", [&ev](const auto&) -> EvalValue {
        auto sidx = ev.string_heap_.size();
        if (!ev.get_jit_stats_fn_) {
            ev.string_heap_.push_back("");
        } else {
            const char* s = ev.get_jit_stats_fn_();
            ev.string_heap_.push_back(s ? std::string(s) : std::string());
        }
        return make_string(sidx);
    });

    // (query:soa-dirty-stats) — Issue #429: live SoA
    // dirty state aggregate. Returns a hash with 8 fields
    // computed in one pass over ir_cache_v2_:
    //   - cached_fns:            # entries in the cache
    //   - dirty_fns:             # entries with entry.dirty == true
    //   - total_blocks:          sum of block_dirty_per_func_[i].size()
    //   - dirty_blocks:          sum of #dirty blocks
    //   - total_instructions:    sum of IRFunction.instructions.size()
    //   - dirty_instructions:    # entries with entry.dirty
    //                            (per-instruction aggregate is a
    //                            follow-up — see CompilerService ::
    //                            get_soa_dirty_stats comment)
    //   - dirty_block_pct:       100 * dirty_blocks / total_blocks
    //   - dirty_instruction_pct: 100 * dirty_instructions /
    //                            total_instructions
    //
    // The new primitive complements (query:ir-soa-incremental-stats)
    // (mutation-event lifetime counts) and (compile:ir-soa-stats)
    // (a #254 hash that ships the migration-progress field).
    // query:soa-dirty-stats is the LIVE view (current
    // dirty state) — the AI Agent reads it to decide
    // whether the cache is in a healthy steady state
    // (low dirty_block_pct) or needs a re-lower burst
    // (> 20% dirty means the cache is falling behind the
    // mutation rate).
    add("query:soa-dirty-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        };
        Evaluator::SoaDirtyStats s;
        if (ev.get_soa_dirty_stats_fn_) {
            s = ev.get_soa_dirty_stats_fn_();
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"cached-fns", make_int(static_cast<std::int64_t>(s.cached_fns))},
            {"dirty-fns", make_int(static_cast<std::int64_t>(s.dirty_fns))},
            {"total-blocks", make_int(static_cast<std::int64_t>(s.total_blocks))},
            {"dirty-blocks", make_int(static_cast<std::int64_t>(s.dirty_blocks))},
            {"total-instructions", make_int(static_cast<std::int64_t>(s.total_instructions))},
            {"dirty-instructions", make_int(static_cast<std::int64_t>(s.dirty_instructions))},
            {"dirty-block-pct", make_int(static_cast<std::int64_t>(s.dirty_block_pct))},
            {"dirty-instruction-pct", make_int(static_cast<std::int64_t>(s.dirty_instruction_pct))},
        };
        return build_hash(kv);
    });

    // (query:arena-compaction-stats-hash) — Issue #430:
    // hash variant of (query:arena-compaction-stats). The
    // legacy primitive returns a single integer = sum of
    // 7 fields (cheaper for dashboards that only need
    // the aggregate). The hash variant exposes each
    // field as a distinct key for the AI Agent's
    // per-field reasoning (e.g. "is the save rate
    // dropping?" needs total_compaction_saved vs
    // compaction_count, which the sum collapses).
    //
    // Field list (10 total):
    //   - auto-compact-triggers: ArenaGroup trigger count
    //   - auto-compact-skips:    ArenaGroup skip count
    //   - compactions:           lifetime compact() calls
    //   - bytes-saved:           lifetime bytes reclaimed
    //   - last-saved:            bytes reclaimed by last compact
    //   - paused-by-boundary:    deferred at MutationBoundary
    //   - mutation-volume:       total_mutations_ (orchestration signal)
    //   - dirty-propagation:     mark_dirty_upward activity
    //   - fragmentation-ratio:   current main arena frag ratio * 100
    //   - peak-used-bytes:       high-water mark for main arena
    //
    // Both primitives share the same underlying counters;
    // pick the integer when you want a single dashboard
    // metric, pick the hash when you want per-field
    // reasoning. The integer variant is the recommended
    // hot path; the hash is for debugging / AI Agent
    // observability.
    add("query:arena-compaction-stats-hash", [&ev](const auto&) -> EvalValue {
        // Reuse the same build_hash pattern as the
        // closure:stats / soa-dirty-stats primitives.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        };
        std::uint64_t triggers = 0, skips = 0, compacts = 0, saved = 0;
        std::uint64_t paused = 0, mutations = 0, dirty = 0;
        std::uint64_t frag_pct = 0, peak = 0, last_saved = 0;
        if (ev.arena_group_) {
            const auto stats = ev.arena_group_->total_stats();
            triggers = ev.arena_group_->auto_compact_trigger_count();
            skips = ev.arena_group_->auto_compact_skip_count();
            compacts = stats.compaction_count;
            saved = stats.total_compaction_saved;
            last_saved = stats.last_compaction_saved;
            paused = ev.compaction_paused_by_boundary();
            mutations = ev.total_mutations();
            dirty = ev.get_dirty_propagation_count();
            // Main arena frag ratio (scaled 0..100). Use
            // arena_ (the main per-Evaluator arena) if set;
            // else 0 (no fallback path — ArenaGroup::arenas_
            // is private, and the per-module frag ratio is
            // already exposed via arena:adaptive-stats +
            // query:arena-compaction-stats). The
            // fragmentation-ratio-pct field is the
            // single-arena view; the multi-arena view is
            // in the per-arena strings.
            if (ev.arena_) {
                const auto s = ev.arena_->stats();
                frag_pct = static_cast<std::uint64_t>(s.fragmentation_ratio() * 100);
                peak = s.peak_used;
            }
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"auto-compact-triggers", make_int(static_cast<std::int64_t>(triggers))},
            {"auto-compact-skips", make_int(static_cast<std::int64_t>(skips))},
            {"compactions", make_int(static_cast<std::int64_t>(compacts))},
            {"bytes-saved", make_int(static_cast<std::int64_t>(saved))},
            {"last-saved", make_int(static_cast<std::int64_t>(last_saved))},
            {"paused-by-boundary", make_int(static_cast<std::int64_t>(paused))},
            {"mutation-volume", make_int(static_cast<std::int64_t>(mutations))},
            {"dirty-propagation", make_int(static_cast<std::int64_t>(dirty))},
            {"fragmentation-ratio-pct", make_int(static_cast<std::int64_t>(frag_pct))},
            {"peak-used-bytes", make_int(static_cast<std::int64_t>(peak))},
        };
        return build_hash(kv);
    });

    // (query:cxx26-invariants) — Issue #431: a 5-field
    // hash summarizing the codebase's C++26 zero-overhead
    // invariant density. The numbers are compile-time
    // constants tied to the source — they don't move at
    // runtime. The AI Agent reads the count to detect
    // drift (a regression in invariant coverage is a
    // "the codebase lost some compile-time safety" signal).
    //
    // Field list (5 total):
    //   - consteval-invariants: # static_assert blocks
    //     in src/core/cxx26_invariants.ixx (currently 22
    //     — SmallObjectPool tier + Value tag + concept
    //     self-check groups)
    //   - concept-count: # Concepts in src/core/concepts.ixx
    //     (currently 13 — NodeHandle, ASTContainer,
    //     Mutator, ArenaAllocator, Queryable, AuraInvocable,
    //     RangeOf, AnyRange, SymbolInterner, StableNodeRefLike
    //     + Issue #431's SoAColumnar, DirtyPropagator,
    //     ShapeDispatchable)
    //   - contract-hot-paths: # Contract pre/post/assert
    //     sites in Arena + Value + SoA + Pass (sum across
    //     these 4 hot files, currently 26 — issue #431
    //     scope-limited ship doesn't add new Contracts
    //     beyond what was already there; follow-up issues
    //     will)
    //   - concept-self-checks: # static_asserts in
    //     cxx26_invariants.ixx that verify Concepts
    //     compile (currently 1 — the std::vector<int>
    //     check)
    //   - concept-targets-documented: # "Target sites:"
    //     comments in concepts.ixx (currently 9 — each
    //     concept has a doc comment listing the
    //     consumers / future consumers)
    //
    // The contract-hot-paths count is approximate —
    // see ContractHotPathCount() for the exact
    // grep-and-sum. If a future issue adds Contracts
    // to value.ixx or ir_soa.ixx, this number will
    // jump. The AI Agent monitors the count.
    add("query:cxx26-invariants", [&ev](const auto&) -> EvalValue {
        // Reuse the same build_hash pattern as the
        // closure:stats / soa-dirty-stats primitives.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        };
        // Compile-time constants — the file paths are
        // recorded in the comment; the literal numbers
        // are the live counts at the time of writing.
        // The AI Agent detects drift by re-reading the
        // file and comparing the count delta.
        constexpr std::int64_t kConstevalInvariants = 22;
        constexpr std::int64_t kConceptCount = 13;
        constexpr std::int64_t kContractHotPaths = 26;
        constexpr std::int64_t kConceptSelfChecks = 1;
        constexpr std::int64_t kConceptTargetsDoc = 9;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"consteval-invariants", make_int(kConstevalInvariants)},
            {"concept-count", make_int(kConceptCount)},
            {"contract-hot-paths", make_int(kContractHotPaths)},
            {"concept-self-checks", make_int(kConceptSelfChecks)},
            {"concept-targets-documented", make_int(kConceptTargetsDoc)},
        };
        return build_hash(kv);
    });

    // (query:edsl-readiness) — Issue #440: a single
    // hash that aggregates the top 8 EDSL production
    // readiness signals from across the existing
    // query:*-stats primitives. The intent is for the
    // AI Agent to ask "is the EDSL production-ready?"
    // in a single query (vs. 8 separate (query:*) calls).
    //
    // Field list (6 total):
    //   - closure-stale-refresh:    closure_bridge refreshes (#531)
    //   - linear-check-pass:        linear ownership fast-path checks (#149)
    //   - mutation-rollbacks:       MutationBoundaryGuard rollbacks (#241)
    //   - mutation-commits:        MutationBoundaryGuard commits (#241)
    //   - stable-ref-invalidates:   StableNodeRef is_valid misses (#417)
    //   - generation-bumps:         mutation_epoch_ lifetime total (#401)
    //   - pattern-macro-filtered:   MacroIntroduced skipped in patterns (#421)
    //   - dirty-block-rate:         live per-block dirty % from #429
    //                                (capped 0..100)
    //
    // All fields are non-negative integers; the AI Agent
    // reads each as a signal:
    //   - High closure-stale-refresh + low stale-refresh → bridge healthy
    //   - Linear-check-pass dominance → linear ownership fast path active
    //   - Mutation-commits > mutation-rollbacks → mutations stick
    //   - Generation-bumps > 0 → mutating (cache eviction expected)
    //   - Pattern-macro-filtered > 0 → hygiene gate active
    //   - Dirty-block-rate > 20% → cache is falling behind
    //
    // The 8 fields are a curated subset of the 30+
    // existing (query:*-stats) primitives; the issue
    // body enumerates which. Adding more later is a
    // 1-line code change (add to the kv vector).
    add("query:edsl-readiness", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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
        };
        std::uint64_t closure_stale = 0, linear_pass = 0;
        std::uint64_t atomic_commits = 0;
        std::uint64_t stable_ref_invalidates = 0;
        std::uint64_t occurrence_stale_refreshes = 0;
        std::int64_t dirty_pct = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            closure_stale = m->closure_stale_refresh_count_.load(std::memory_order_relaxed);
            linear_pass = m->linear_check_pass_count_.load(std::memory_order_relaxed);
            atomic_commits = m->atomic_batch_commits.load(std::memory_order_relaxed);
            stable_ref_invalidates = m->stable_ref_invalidations.load(std::memory_order_relaxed);
            occurrence_stale_refreshes = m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed);
        }
        // dirty-block-rate from #429's get_soa_dirty_stats.
        if (ev.get_soa_dirty_stats_fn_) {
            const auto s = ev.get_soa_dirty_stats_fn_();
            dirty_pct = static_cast<std::int64_t>(s.dirty_block_pct);
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"closure-stale-refresh", make_int(static_cast<std::int64_t>(closure_stale))},
            {"linear-check-pass", make_int(static_cast<std::int64_t>(linear_pass))},
            {"atomic-batch-commits", make_int(static_cast<std::int64_t>(atomic_commits))},
            {"stable-ref-invalidations", make_int(static_cast<std::int64_t>(stable_ref_invalidates))},
            {"occurrence-stale-refreshes", make_int(static_cast<std::int64_t>(occurrence_stale_refreshes))},
            {"dirty-block-rate", make_int(dirty_pct)},
        };
        return build_hash(kv);
    });

    // (gc-arena-stats) — Report per-arena allocation. Shows main arena +
    // every per-module arena. Format: "main:0.1MB/8.0MB;json.aura:0.5MB/8.0MB;..."
    // (semicolons separate entries; slashes separate used/capacity within an entry).
    add("gc-arena-stats", [&ev](const auto&) -> EvalValue {
        std::string out;
        auto fmt_arena = [&](const char* label, std::size_t used, std::size_t cap) {
            auto s = std::format("{}{}:{:.1f}MB/{:.1f}MB", out.empty() ? "" : ";", label,
                                 used / 1048576.0, cap / 1048576.0);
            out += s;
        };
        if (ev.arena_) {
            auto s = ev.arena_->stats();
            fmt_arena("main", s.used, s.capacity);
        }
        if (ev.arena_group_) {
            for (auto& [name, stats] : ev.arena_group_->module_stats()) {
                // Trim path to basename for readability.
                auto slash = name.rfind('/');
                auto short_name = slash == std::string::npos ? name : name.substr(slash + 1);
                fmt_arena(short_name.c_str(), stats.used, stats.capacity);
            }
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(out);
        return types::make_string(sidx);
    });

    // (gc-arena-info) — Return structured per-arena usage as Aura value.
    //
    //   Returns: vector of hashes, each describing one arena:
    //     {name: "main", used: 1.23, capacity: 11.0, used-pct: 11}
    //     {name: "json.aura", used: 0.5, capacity: 8.0, used-pct: 6}
    //     ...
    //
    //   First entry is a summary hash:
    //     {summary: #t, total-arenas: 3, total-used: 1.73, total-capacity: 19.0,
    //      overall-pct: 9}
    //
    //   All numeric values are in megabytes (MB). Pct values are integers 0-100.
    add("gc-arena-info", [&ev](const auto&) -> EvalValue {
        // Snapshot arena state. Each entry: (short_name, used-MB, cap-MB, pct).
        struct Snap {
            std::string name;
            double used;
            double cap;
            int pct;
        };
        std::vector<Snap> snaps;
        double total_used = 0.0, total_cap = 0.0;
        if (ev.arena_) {
            auto s = ev.arena_->stats();
            double u = s.used / 1048576.0;
            double c = s.capacity / 1048576.0;
            snaps.push_back({"main", u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
            total_used += u;
            total_cap += c;
        }
        if (ev.arena_group_) {
            for (auto& [full_name, stats] : ev.arena_group_->module_stats()) {
                auto slash = full_name.rfind('/');
                auto short_name =
                    slash == std::string::npos ? full_name : full_name.substr(slash + 1);
                double u = stats.used / 1048576.0;
                double c = stats.capacity / 1048576.0;
                snaps.push_back({short_name, u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
                total_used += u;
                total_cap += c;
            }
        }
        int overall = total_cap > 0 ? static_cast<int>(total_used / total_cap * 100.0) : 0;

        // Build a small Swiss-table hash. Inline copy of the (hash ...) primitive
        // pattern. Capacity 8 is enough for the 5-field hashes below.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            for (auto& [k, v] : kv) {
                // Hash the key with FNV-1a (matches user-level (hash ...) behavior).
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
                // Intern the key as a String EvalValue.
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < cap; ++at) {
                    auto idx = ((h >> 1) + at) & (cap - 1);
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
                    // 8 slots should be enough for the 5-key hashes we build.
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };

        std::vector<EvalValue> result;
        // Summary entry first.
        {
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.push_back({"summary", make_bool(true)});
            kv.push_back({"total-arenas", make_int(static_cast<std::int64_t>(snaps.size()))});
            kv.push_back({"total-used", make_float(total_used)});
            kv.push_back({"total-capacity", make_float(total_cap)});
            kv.push_back({"overall-pct", make_int(overall)});
            result.push_back(build_hash(kv));
        }
        for (auto& s : snaps) {
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(s.name);
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.push_back({"name", make_string(name_idx)});
            kv.push_back({"used", make_float(s.used)});
            kv.push_back({"capacity", make_float(s.cap)});
            kv.push_back({"used-pct", make_int(s.pct)});
            result.push_back(build_hash(kv));
        }
        auto vidx = ev.vector_heap_.size();
        ev.vector_heap_.push_back(std::move(result));
        return make_vector(vidx);
    });

    // Issue #560: (stats:list) — returns the list of every
    // registered *-stats primitive (the source of truth for
    // the std/stats Aura module). Each entry is the primitive
    // name (string). Used by std/stats.aura for the (stats:list)
    // + (stats:count) helpers + for AI Agent observability
    // dashboards that want to enumerate all stats.
    add("stats:list", [&ev](const auto&) -> EvalValue {
        const std::vector<std::string> stats = {
            // Issue #543 — EnvFrame dual-path
            "query:envframe-dualpath-stats",
            // Issue #547 — Pattern + MacroIntroduced hygiene
            "query:pattern-index-stats",
            "query:pattern-hygiene-stats",
            // Issue #548 — Panic-checkpoint lifecycle
            "query:panic-checkpoint-lifecycle-stats",
            // Issue #549 — Self-evolution stability
            "query:self-evolution-stability-stats",
            // Issue #550 — Typed mutation + dirty impact
            "query:typed-mutation-stats",
            "query:dirty-impact",
            // Issue #573 — Task2 solve_delta + narrowing incremental
            "query:typed-incremental-stats",
            // Issue #608 — Incremental type reliability
            "query:type-incremental-stats",
            // Issue #509 — solve_delta touched_roots soundness
            "query:constraint-delta-stats",
            // Issue #628 — solve_delta clean-conflict safety
            "query:solve-delta-safety-stats",
            // Issue #467 — Per-node occurrence-dirty + blame chain
            "query:occurrence-stats",
            // Issue #495 — Task2 refinement closed-loop pillars
            "query:task2-refinement-stats",
            // Issue #609 — Occurrence narrow post-mutate recovery
            "query:occurrence-narrow-stats",
            // Issue #576 — Task2 occurrence blame + provenance
            "query:occurrence-blame-stats",
            // Issue #577 — Task2 ADT exhaustiveness + match narrowing
            "query:adt-exhaustiveness-stats",
            // Issue #454 — Reflection-to-EDSL bridge (FlatAST/marker)
            "query:reflect-edsl-bridge-stats",
            // Issue #551 — Reflect post-mutate
            "query:reflect-postmutate-stats",
            // Issue #594 — Static reflection self-mod validation hook
            "query:reflection-selfmod-stats",
            // Issue #597 — Macro+reflect+self-evo combined loop
            "query:macro-reflect-self-evo-stats",
            // Issue #595 — Marker/dirty/epoch/Guard self-evo loop
            "query:self-evolution-loop-stats",
            // Issue #415 — DirtyReason verification categories +
            // mark_dirty_upward propagation synthesis
            "query:dirty-reason-propagation-stats",
            // Issue #517 — Consolidated 3-pillar production priority meta
            "query:consolidated-production-priority-stats",
            // Issue #520 — Consolidated Top 5 production roadmap synthesis
            "query:production-roadmap-stats",
            // Issue #514 — Task6 Top 3 production-readiness synthesis
            "query:ir-hygiene-stats",
            "query:pattern-marker-stats",
            "query:task6-production-readiness-stats",
            // Issue #441 — Consolidated compiler/runtime P0 synthesis
            "query:compiler-runtime-production-readiness-stats",
            // Issue #634 — Commercial production readiness P0 synthesis
            "query:commercial-production-readiness-stats",
            // Issue #635 — Macro+reflect+self-evo commercial closed-loop
            "query:macro-reflect-self-evo-commercial-stats",
            // Issue #636 — EDSL workspace query/mutate commercial closed-loop
            "query:edsl-query-mutate-commercial-stats",
            // Issue #619 — Task6 macro+reflect+self-evo follow-up
            "query:macro-reflect-self-evo-followup-stats",
            // Issue #602 — Prompt6 memory-safety matrix
            "query:prompt6-violation-count",
            "query:prompt6-safety-score",
            // Issue #570 — ShapeProfiler stability + deopt
            "query:shape-stability-stats",
            // Issue #571 — EvalValue v2 dispatch + contracts
            "query:value-dispatch-stats",
            // Issue #506 — IR SoA + dirty-aware Pass hotpath adoption
            "query:soa-hotpath-adoption-stats",
            // Issue #404 — IR SoA Phase 3 block_dirty incremental lowering
            "query:ir-soa-incremental-stats",
            // Issue #403 — IRInstruction rich metadata interpreter/JIT
            "query:ir-metadata-stats",
            // Issue #607 — Task4 high-perf hot-path matrix
            "query:task4-hotpath-safety-score",
            "query:task4-cache-locality-win",
            "query:task4-mutation-stability",
            // Issue #552 — EDSL stability
            "query:edsl-stability-stats",
            // Issue #553 — Atomic batch + mutation log
            "query:mutation-log-stats",
            // Issue #529 — Atomic batch + Guard rollback closed loop
            "query:atomic-batch-rollback-stats",
            // Issue #527 — StableNodeRef cross-COW/fiber closed loop
            "query:stable-ref-cow-fiber-stats",
            // Issue #400 — sym_id/structural rollback coverage
            "query:mutation-rollback-coverage-stats",
            // Issue #554 — Pattern index timing (same name as #547; unified)
            // Issue #555 — Typed mutation Task1
            "query:typed-mutation-stats-task1",
            // Issue #556 — EDSL concurrency safety
            "query:edsl-concurrency-stats",
            // Issue #531 — Closure env safety
            "query:closure-env-safety-stats",
            // Issue #610 — Linear ownership post-mutate validation
            "query:linear-ownership-mutation-stats",
            // Issue #638 — Linear + GuardShape runtime safety post-mutate
            "query:linear-ownership-safety-stats",
            // Issue #598 — Runtime linear enforcement + invalidate hook
            "query:linear-ownership-runtime-stats",
            // Issue #575 — Task2 PerDefUse incremental linear ownership
            "query:linear-ownership-incremental-stats",
            // Pre-existing (Issue #288, #391, #447, #457, #459)
            "query:query-stats",
            "query:stale-ref-stats",
            "query:atomic-batch-stats",
            "query:stable-ref-stats",
            "query:fiber-migration-stats",
            "query:mutation-coordination-stats",
            "query:envframe-stale-stats",
            "query:envframe-bump-stats",
            "query:dirty-subtree",
            "query:epoch-stats",
            "query:macro-introduced",
            "query:by-marker",
            // Compile: stats (Issue #560 enumeration source of truth)
            "compile:compiler-cache-stats",
            "compile:compiler-incremental-stats",
            "compile:typecheck-stats",
            "compile:jit-stats",
            "compile:arena-stats",
            "compile:dead-coercion-stats",
            // Issue #574 — coercion elimination summary
            "query:coercion-elim-stats",
            // Issue #468 — DeadCoercionEliminationPass zero-overhead
            "query:dead-coercion-zerooverhead-stats",
            "compile:per-defuse-index-stats",
            "compile:mutator-dispatch-stats",
            "compile:mutation-impact-stats",
            "compile:inline-pass-stats",
            "compile:type-cache-stats",
            "compile:dirty-impact-stats",
            // Primitive error (Issue #478)
            "query:primitive-error-stats",
            // Issue #583 — Registry + core primitives hot-path stats
            "query:primitives-stats",
            // Issue #480 — Self-describing primitive metadata closed loop
            "query:primitive-meta-stats",
            // Issue #405 — Arena auto-compaction orchestration signals
            "query:arena-compaction-stats",
            // Issue #406 — Pass Pipeline + Contracts hot-path stats
            "query:pass-contracts-stats",
            // Issue #407 — ShapeProfiler burst/deopt storm observability
            "query:shape-deopt-burst-stats",
            // Issue #408 — EDSL dirty propagation cost observability
            "query:dirty-propagation-cost-stats",
            // Issue #414 — Long-term generation_/epoch management
            "query:generation-epoch-stats",
            // Issue #416 — AST SoA column compaction observability
            "query:ast-column-compaction-stats",
            // Issue #417 — MutationBoundary cross-TU invariant stats
            "query:mutation-boundary-invariant-stats",
            // Issue #418 — EnvFrame dual-path + stale policy stats
            "query:envframe-dualpath-stale-stats",
            // Issue #419 — Modular defuse_version + AOT dispatch stats
            "query:defuse-version-stats",
            // Issue #420 — MacroIntroduced end-to-end hygiene contract
            "query:macro-hygiene-contract-stats",
            // Issue #421 — query:pattern recursive MacroIntroduced filter
            "query:pattern-macro-filter-stats",
            // Issue #422 — Mutate-path hygiene violation detection
            "query:hygiene-violation-stats",
            // Issue #423 — query:pattern structural pre-index
            "query:pattern-structural-index-stats",
            // Issue #424 — StableNodeRef WorkspaceTree COW safety
            "query:stable-ref-workspace-tree-stats",
            // Issue #428 — closure bridge + bridge_epoch drift
            "query:closure-stats",
            // Issue #429 — IR SoA live dirty state
            "query:soa-dirty-stats",
            // Issue #430 — arena compaction hash variant
            "query:arena-compaction-stats-hash",
            // Issue #431 — C++26 Contracts/Concepts/consteval density
            "query:cxx26-invariants",
            // Issue #440 — consolidated Task 1 EDSL readiness
            "query:edsl-readiness",
            // Issue #444 — strategy evolution controller pheromone stats
            "query:strategy-evolution-stats",
            // Issue #445 — SEVA audit log (OpenClaw integration)
            "query:seva-audit-log",
            // Issue #446 — SEVA demo with metrics
            "seva:run-demo-with-metrics",
            // Issue #450 (sub-issue #441) — primitive perf stats
            "query:primitive-perf-stats",
            // Issue #452 — AOT hot-update + region filtering
            "query:aot-stats",
            // Issue #462 — ShapeAwareFoldingPass
            "query:shape-folding-stats",
            // Issue #463 — SoA Phase 2 adoption
            "query:soa-adoption-stats",
            // Issue #675 — CI reproducibility + sanitizer gates
            "query:ci-reproducibility-stats",
            // Issue #676 — sandbox capability + mutation audit
            "query:security-stats",
            "query:mutation-audit-log",
            // Issue #464 — Arena auto-compaction lifecycle
            "query:arena-auto-stats",
            // Issue #677 — deployment / health / metrics export
            "query:deployment-stats",
            // Issue #465 — C++26 hot-path contracts + consteval
            "query:cxx26-hotpath-invariants",
        };
        // Convert the C++ vector to an Aura list of strings.
        EvalValue result = make_void();
        for (auto it = stats.rbegin(); it != stats.rend(); ++it) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // Issue #560: (stats:count) — companion to (stats:list).
    // Returns the # of registered *-stats primitives.
    add("stats:count", [&ev](const auto&) -> EvalValue {
        // Source of truth = (stats:list) entry count.
        // 56 entries as of #465 ship (55 from #677 + 1 new
        // query:cxx26-hotpath-invariants).
        return make_int(55);
    });

}

} // namespace aura::compiler::primitives_detail
