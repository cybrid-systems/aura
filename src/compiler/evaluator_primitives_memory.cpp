// evaluator_primitives_memory.cpp — P0 step 19: coverage / gc / arena / dirty / memory-pressure
// aura.compiler.evaluator module partition; registered via evaluator_ctor.cpp.

module;

#include "runtime_shared.h"
#include "messaging_bridge.h"
#include "hash_meta.h" // FNV constants (#901)
#include "observability_metrics.h"
#include "render_telemetry.hh"
#include "core/arena_auto_policy_stats.h"
#include <limits>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

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

void register_memory_primitives(PrimRegistrar add, Evaluator& ev,
                                std::function<void()> destroy_defuse_index) {

    // ── coverage-report — 编译器路径覆盖率 ──────────────────
    ObservabilityPrims::register_stats_impl(
        "coverage-report", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            std::string result = "#(coverage";
            for (int i = 0; i < 16; i++) {
                if (ev.coverage_counters_[i] > 0) {
                    std::string name;
                    switch (i) {
                        case 0:
                            name = "parser";
                            break;
                        case 1:
                            name = "typecheck";
                            break;
                        case 2:
                            name = "eval";
                            break;
                        case 3:
                            name = "jit";
                            break;
                        case 4:
                            name = "macro";
                            break;
                        case 5:
                            name = "edsl-set-code";
                            break;
                        case 6:
                            name = "edsl-query";
                            break;
                        case 7:
                            name = "edsl-mutate";
                            break;
                        case 8:
                            name = "ffi";
                            break;
                        default:
                            name = "reserved-" + std::to_string(i);
                            break;
                    }
                    result += " " + name + ":" + std::to_string(ev.coverage_counters_[i]);
                }
            }
            result += ")";
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(result);
            return types::make_string(sidx);
        });

    // (gc) — Reset arena to reclaim memory between benchmark tasks
    // Saves current source, resets arena, re-parses source into fresh arena.
    add("gc", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        // Save current source
        std::string saved_src;
        if (ev.workspace_flat_ && ev.workspace_flat_->root != aura::ast::NULL_NODE) {
            auto src_fn = ev.primitives_.lookup("current-source");
            if (src_fn) {
                auto src = (*src_fn)({});
                if (types::is_string(src)) {
                    auto sidx = types::as_string_idx(src);
                    if (sidx < ev.string_heap_.size())
                        saved_src = ev.string_heap_[sidx];
                }
            }
        }

        // Reset arena (invalidates all arena-allocated state)
        // (ASAN fix #107 leak) delete the old index.
        destroy_defuse_index();
        ev.modules_.clear();
        ev.module_cache_.clear();
        ev.current_flat_ = nullptr;
        ev.current_pool_ = nullptr;
        ev.workspace_flat_ = nullptr;
        ev.workspace_pool_ = nullptr;
        if (aura::messaging::g_reset_arena && ev.compiler_service_) {
            aura::messaging::g_reset_arena(ev.compiler_service_);
        }

        // Re-parse saved source into fresh arena
        if (!saved_src.empty()) {
            auto set_fn = ev.primitives_.lookup("set-code");
            if (set_fn) {
                auto si = ev.string_heap_.size();
                ev.string_heap_.push_back(saved_src);
                (*set_fn)({types::make_string(si)});
            }
        }

        return types::make_bool(!saved_src.empty());
    });

    // (gc-heap) — Trigger GC or clear heap vectors.
    // When a GC collector is available (serve-async mode with
    // thread-safe GC), triggers a full GC cycle instead of
    // blindly clearing. Falls back to direct clear for stdin mode.
    add("gc-heap", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        // If GC collector is available, use it
        if (aura::messaging::g_gc_collect) {
            std::lock_guard<std::mutex> lock(ev.heap_mutex());
            return types::make_bool(aura::messaging::g_gc_collect());
        }
        // Fallback: direct clear (stdin mode)
        {
            std::lock_guard<std::mutex> lock(ev.heap_mutex());
            // Clear ev.short_str_cache_ BEFORE ev.string_heap_ so cached EvalValues
            // referencing old indices aren't returned after the heap shrinks.
            // Without this, the next LiteralString eval returns a stale
            // cached String EvalValue pointing past the end of ev.string_heap_,
            // and ev.string_heap_[idx] is UB (segfault on .data() access).
            ev.short_str_cache_.clear();
            ev.string_heap_.clear();
            ev.string_heap_.shrink_to_fit();
            ev.pairs_.clear();
            ev.pairs_.shrink_to_fit();
            ev.error_values_.clear();
            ev.error_values_.shrink_to_fit();
            for (auto* fht : g_hash_tables)
                FlatHashTable::destroy(fht);
            g_hash_tables.clear();
            g_hash_tables.shrink_to_fit();
            ev.vector_heap_.clear();
            ev.vector_heap_.shrink_to_fit();
            ev.opaque_heap_.clear();
            ev.opaque_heap_.shrink_to_fit();
            // gc-heap is a stronger reset than gc-temp; also record
            // the eval-depth snapshot so memory-pressure won't keep
            // suggesting "gc-temp" right after a gc-heap.
            ev.last_gc_temp_eval_depth_ = ev.eval_depth_;
        }
        return types::make_bool(true);
    });

    // (gc-freeze) — Mark current closure generation as "root".
    // The while loop's predicate/body closures are created before this
    // call (in persistent arena when in_task_context_=false).
    add("gc-freeze", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        ev.gc_safe_closure_id_ = ev.next_id_;
        return types::make_bool(true);
    });

    // (gc-temp) — Reset temp arena + clear temp closures + heap vectors.
    // Safe to call between benchmark tasks. Temp closures (those with
    // owner_arena == ev.temp_arena_) are erased, their arena memory freed O(1).
    // Module functions and while-loop closures (in persistent arena) survive.
    add("gc-temp", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.temp_arena_)
            return types::make_bool(false);

        // Erase closures in temp arena
        for (auto it = ev.closures_.begin(); it != ev.closures_.end();) {
            if (it->second.owner_arena == ev.temp_arena_)
                it = ev.closures_.erase(it);
            else
                ++it;
        }

        // Reset temp arena (O(1) — frees all cl_flat/cl_pool/copy_env)
        ev.temp_arena_->reset();
        // Record the eval-depth snapshot so memory-pressure knows
        // when to suggest "gc-temp" again.
        ev.last_gc_temp_eval_depth_ = ev.eval_depth_;

        // Clear heap vectors.
        // NOTE: ev.pairs_ and ev.string_heap_ are NOT cleared — result lists are
        // pair-based and contain string references. gc-temp is called
        // before the caller reads results. Use gc-heap separately to
        // clear strings/pairs when results are no longer needed.
        // ev.vector_heap_, ev.opaque_heap_ are safe to clear here.
        ev.error_values_.clear();
        ev.error_values_.shrink_to_fit();
        for (auto* fht : g_hash_tables)
            FlatHashTable::destroy(fht);
        g_hash_tables.clear();
        g_hash_tables.shrink_to_fit();
        ev.vector_heap_.clear();
        ev.vector_heap_.shrink_to_fit();
        ev.opaque_heap_.clear();
        ev.opaque_heap_.shrink_to_fit();

        return types::make_bool(true);
    });

    // (gc-stats) — Return formatted string of all heap sizes for telemetry.
    ObservabilityPrims::register_stats_impl(
        "gc-stats", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            std::uint64_t root_count = 0;
            for (auto& [id, _] : ev.closures_) {
                if (id < ev.gc_safe_closure_id_)
                    ++root_count;
            }
            auto result = std::format(
                "string:{}/pairs:{}/cells:{}/err:{}/hash:{}/vec:{}/opq:{}/cls:{}/root:{}",
                ev.string_heap_.size(), ev.pairs_.size(), ev.cells_.size(), ev.error_values_.size(),
                g_hash_tables.size(), ev.vector_heap_.size(), ev.opaque_heap_.size(),
                ev.closures_.size(), root_count);
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(result);
            return types::make_string(sidx);
        });

    // (gc-module "path") — Free a previously-loaded module's per-module
    // arena and remove it from the module cache. Returns #t on success,
    // #f if the path wasn't loaded. The path must match exactly what was
    // passed to (import) / (require) — for stdlib modules loaded via
    // AURA_PATH, this is the resolved absolute path.
    add("gc-module", [&ev, destroy_defuse_index](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return types::make_bool(false);
        auto sidx = types::as_string_idx(a[0]);
        if (sidx >= ev.string_heap_.size())
            return types::make_bool(false);
        return types::make_bool(ev.gc_module(ev.string_heap_[sidx]));
    });

    // (gc-module-count) — Number of modules currently in the module cache.
    ObservabilityPrims::register_stats_impl(
        "gc-module-count", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(ev.modules_.size()));
        });

    // (type-registry-stats) — Issue #78: TypeRegistry observability.
    // Returns a hash with current size, generation, and predefined count.
    // Use this to monitor TypeRegistry growth in long-running sessions.
    ObservabilityPrims::register_stats_impl(
        "type-registry-stats", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            if (!ev.type_registry_) {
                return make_void();
            }
            auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.push_back({"size", make_int(static_cast<std::int64_t>(treg.size()))});
            kv.push_back({"generation", make_int(static_cast<std::int64_t>(treg.generation()))});
            kv.push_back({"predefined-count", make_int(static_cast<std::int64_t>(
                                                  aura::core::TypeRegistry::kPredefinedCount))});
            kv.push_back(
                {"user-types", make_int(static_cast<std::int64_t>(
                                   treg.size() - aura::core::TypeRegistry::kPredefinedCount))});
            // Build a hash with the 4 keys.
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
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
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // (type-registry-compact) — Issue #78: reclaim all non-predefined
    // entries. Bumps the generation counter so any TypeId from the
    // previous generation becomes stale. Returns the number of entries
    // reclaimed.
    add("type-registry-compact", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.type_registry_) {
            return make_int(0);
        }
        auto& treg = *static_cast<aura::core::TypeRegistry*>(ev.type_registry_);
        std::uint32_t reclaimed = treg.compact();
        return make_int(static_cast<std::int64_t>(reclaimed));
    });

    // (arena:compact) — Issue #187 (P0): conservative arena buffer
    // compaction. Reclaims the unused tail of the main arena's pmr
    // buffer by rebuilding it at used-size + 25% headroom. Returns
    // the number of bytes reclaimed. Use (arena:compact-all) to
    // compact every per-module arena above the configured threshold.
    add("arena:compact", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.arena_)
            return make_int(0);
        // Issue #264: defer compaction while any fiber holds an
        // active MutationBoundaryGuard on this evaluator.
        if (ev.any_active_mutation_boundary()) {
            ev.compaction_paused_by_boundary_.fetch_add(1, std::memory_order_relaxed);
            return make_int(0);
        }
        return make_int(static_cast<std::int64_t>(ev.arena_->compact()));
    });
    // (arena:defrag) — Issue #300 (P1): sliding-reclaim the
    // unused tail of the arena's buffer. Same underlying
    // mechanism as compact() (no live-object move), but counted
    // as a defrag attempt via stats_.defrag_attempted_count /
    // last_defrag_saved. See ASTArena::defrag() comment for the
    // pool-backed follow-up scope.
    add("arena:defrag", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.arena_)
            return make_int(0);
        if (ev.any_active_mutation_boundary()) {
            ev.compaction_paused_by_boundary_.fetch_add(1, std::memory_order_relaxed);
            return make_int(0);
        }
        // Explicit Agent call always runs (soft-gate only applies to auto-compact #1320).
        auto saved = static_cast<std::int64_t>(ev.arena_->defrag());
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->arena_defrag_attempted_total.fetch_add(1, std::memory_order_relaxed);
            m->arena_defrag_saved_bytes_total.fetch_add(
                static_cast<std::uint64_t>(saved > 0 ? saved : 0), std::memory_order_relaxed);
        }
        return make_int(saved);
    });
    // (arena:request-defrag) — Issue #300 Phase 3 + #1397 atomic
    // CAS semantics: set the defrag_requested flag on the main
    // arena. The actual defrag runs when something observes the
    // flag and decides to act (typically the main thread or a
    // fiber coordinator at the next safe opportunity).
    //
    // Returns the request's *newly-set* signal (atomic CAS): #t
    // on the call that actually transitions the flag from false to
    // true; #f on every subsequent call until the flag is reset
    // by (arena:defrag) or (arena:request-defrag-reset). The
    // same call also still emits the one-shot "no safepoint"
    // warning if the arena has no safepoint hook installed.
    //
    // To check safepoint registration status independently
    // (which the old "observed = registered" semantics used to
    // conflate), call `(arena:safepoint-registered?)` separately.
    add("arena:request-defrag", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.arena_)
            return make_bool(false);
        const bool observed = ev.arena_->request_defrag();
        return make_bool(observed);
    });
    // (arena:safepoint-registered?) — Issue #1390: boolean
    // query for whether the GC has registered a safepoint
    // callback. Operators can call this before requesting
    // defrag to decide whether to fall back to (arena:defrag-now).
    ObservabilityPrims::register_stats_impl("arena:safepoint-registered?",
                                            [&ev](const auto&) -> EvalValue {
                                                if (!ev.arena_)
                                                    return make_bool(false);
                                                return make_bool(ev.arena_->safepoint_registered());
                                            });
    // (arena:warn-no-safepoint) — Issue #1390: returns true iff
    // the one-shot "no safepoint registered" stderr warning has
    // already fired (i.e., request_defrag() was called while no
    // safepoint was registered). Read-only side-effect: fires
    // the warning the first time it's true. Operators can poll
    // this primitive to detect the misconfiguration.
    ObservabilityPrims::register_stats_impl("arena:warn-no-safepoint",
                                            [](const auto&) -> EvalValue {
                                                bool was = aura::ast::was_no_safepoint_warned();
                                                return make_bool(was);
                                            });

    // ── Issue #1320 Phase 1: explicit live-defrag policy primitive ──
    // (arena:defrag-now) — always run defrag (even during render soft-gate
    // soft-gate is only for auto path; Agent explicit call always acts).
    add("arena:defrag-now", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.arena_)
            return make_int(0);
        if (ev.any_active_mutation_boundary()) {
            ev.compaction_paused_by_boundary_.fetch_add(1, std::memory_order_relaxed);
            return make_int(0);
        }
        auto saved = static_cast<std::int64_t>(ev.arena_->defrag());
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->arena_defrag_now_calls.fetch_add(1, std::memory_order_relaxed);
            m->arena_defrag_attempted_total.fetch_add(1, std::memory_order_relaxed);
            m->arena_defrag_saved_bytes_total.fetch_add(
                static_cast<std::uint64_t>(saved > 0 ? saved : 0), std::memory_order_relaxed);
        }
        return make_int(saved);
    });

    // Issue #1518: (arena:live-compact) — mark + freelist relocate + deopt-coord.
    // Explicit Agent call always runs (force=true). Returns live objects marked.
    add("arena:live-compact", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.arena_)
            return make_int(0);
        if (ev.any_active_mutation_boundary()) {
            ev.compaction_paused_by_boundary_.fetch_add(1, std::memory_order_relaxed);
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->arena_compact_soft_gated_boundary_total.fetch_add(1, std::memory_order_relaxed);
            return make_int(0);
        }
        const auto marked = static_cast<std::int64_t>(ev.arena_->live_compact(/*force=*/true));
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->arena_live_relocate_total.store(
                aura::core::arena_policy::live_relocate_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->arena_compact_deopt_triggered_total.store(
                aura::core::arena_policy::compact_deopt_triggered_total.load(
                    std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->arena_compact_deopt_throttled_total.store(
                aura::core::arena_policy::compact_deopt_throttled_total.load(
                    std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->arena_frag_post_compact_bp.store(
                aura::core::arena_policy::frag_post_compact_bp.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        return make_int(marked);
    });

    // Issue #1518: (query:arena-live-compact-stats) — production surface.
    ObservabilityPrims::register_stats_impl(
        "query:arena-live-compact-stats", [&ev](const auto&) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            auto load = [](const std::atomic<std::uint64_t>& a) {
                return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
            };
            // Refresh from process-wide policy + arena if present.
            if (m) {
                m->arena_live_relocate_total.store(
                    aura::core::arena_policy::live_relocate_total.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                m->arena_compact_deopt_triggered_total.store(
                    aura::core::arena_policy::compact_deopt_triggered_total.load(
                        std::memory_order_relaxed),
                    std::memory_order_relaxed);
                m->arena_compact_deopt_throttled_total.store(
                    aura::core::arena_policy::compact_deopt_throttled_total.load(
                        std::memory_order_relaxed),
                    std::memory_order_relaxed);
                m->arena_frag_post_compact_bp.store(
                    aura::core::arena_policy::frag_post_compact_bp.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                m->arena_compact_soft_gated_boundary_total.store(
                    aura::core::arena_policy::compact_soft_gated_boundary_total.load(
                        std::memory_order_relaxed),
                    std::memory_order_relaxed);
            }
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
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
            };
            std::int64_t live_attempted = 0;
            std::int64_t live_marked = 0;
            std::int64_t live_reloc = 0;
            if (ev.arena_) {
                live_attempted =
                    static_cast<std::int64_t>(ev.arena_->live_defrag_attempted_count_relaxed());
                live_marked =
                    static_cast<std::int64_t>(ev.arena_->live_objects_marked_total_relaxed());
                live_reloc = static_cast<std::int64_t>(ev.arena_->live_relocate_count_relaxed());
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"live-defrag-attempted", make_int(live_attempted)},
                {"live-objects-marked", make_int(live_marked)},
                {"live-relocate-count",
                 make_int(m ? load(m->arena_live_relocate_total) : live_reloc)},
                {"compact-deopt-triggered",
                 make_int(m ? load(m->arena_compact_deopt_triggered_total) : 0)},
                {"compact-deopt-throttled",
                 make_int(m ? load(m->arena_compact_deopt_throttled_total) : 0)},
                {"frag-post-compact-bp", make_int(m ? load(m->arena_frag_post_compact_bp) : 0)},
                {"soft-gated-boundary",
                 make_int(m ? load(m->arena_compact_soft_gated_boundary_total) : 0)},
                {"schema", make_int(1518)},
            };
            return build_hash(kv);
        });

    // ── Issue #1315 Phase 1: render-frame arena lifecycle ──
    // (arena-render-frame-reset) — quick per-frame boundary for TUI loops.
    // Phase 1: temp_arena_ reset when safe + metrics; full time-boxed compact
    // is follow-up. #1320: also sync soft-gate counters for Agent policy.
    add("arena-render-frame-reset", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (ev.any_active_mutation_boundary()) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->render_frame_reset_deferred.fetch_add(1, std::memory_order_relaxed);
            return make_int(0);
        }
        // Issue #1357: record inter-reset frame time into histogram.
        ev.mark_render_frame_boundary();
        std::int64_t reclaimed = 0;
        // Issue #1355: auto-commit any leftover lightweight frames at frame boundary.
        if (ev.workspace_flat_ && ev.workspace_flat_->render_lightweight_active()) {
            const auto n = ev.workspace_flat_->commit_all_render_lightweight_checkpoints();
            reclaimed += static_cast<std::int64_t>(n);
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->mutation_lightweight_frame_commit_total.fetch_add(static_cast<std::uint64_t>(n),
                                                                     std::memory_order_relaxed);
                m->mutation_lightweight_commit_total.fetch_add(static_cast<std::uint64_t>(n),
                                                               std::memory_order_relaxed);
            }
        }
        if (ev.temp_arena_) {
            // O(1) reset of temp arena used for frame scratch.
            ev.temp_arena_->reset();
            reclaimed = reclaimed > 0 ? reclaimed : 1;
        }
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->render_frame_reset_total.fetch_add(1, std::memory_order_relaxed);
            m->render_frame_reset_reclaimed.fetch_add(static_cast<std::uint64_t>(reclaimed),
                                                      std::memory_order_relaxed);
            // Mirror process-wide soft-gate / defrag policy counters.
            m->arena_compact_soft_gated_render.store(
                aura::core::arena_policy::compact_soft_gated_render_total.load(
                    std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->arena_defrag_attempted_total.store(
                aura::core::arena_policy::defrag_attempted_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            // Sync lightweight counters from FlatAST.
            if (ev.workspace_flat_) {
                m->mutation_lightweight_total.store(ev.workspace_flat_->lightweight_total(),
                                                    std::memory_order_relaxed);
                m->mutation_lightweight_commit_total.store(
                    ev.workspace_flat_->lightweight_commit_total(), std::memory_order_relaxed);
                m->mutation_lightweight_rollback_total.store(
                    ev.workspace_flat_->lightweight_rollback_total(), std::memory_order_relaxed);
            }
        }
        return make_int(reclaimed);
    });

    // Issue #1355: Agent/test query for lightweight checkpoint stats.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-lightweight-stats", [&ev](const auto&) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            auto load = [](const std::atomic<std::uint64_t>& a) {
                return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
            };
            std::int64_t total = 0, commit = 0, rollback = 0, records = 0, depth = 0, active = 0;
            if (ev.workspace_flat_) {
                total = static_cast<std::int64_t>(ev.workspace_flat_->lightweight_total());
                commit = static_cast<std::int64_t>(ev.workspace_flat_->lightweight_commit_total());
                rollback =
                    static_cast<std::int64_t>(ev.workspace_flat_->lightweight_rollback_total());
                records =
                    static_cast<std::int64_t>(ev.workspace_flat_->lightweight_records_total());
                depth = static_cast<std::int64_t>(ev.workspace_flat_->render_lightweight_depth());
                active = ev.workspace_flat_->render_lightweight_active() ? 1 : 0;
            } else if (m) {
                total = load(m->mutation_lightweight_total);
                commit = load(m->mutation_lightweight_commit_total);
                rollback = load(m->mutation_lightweight_rollback_total);
            }
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::format(
                "total={} commit={} rollback={} records={} depth={} active={} frame_commit={} "
                "schema=1355",
                total, commit, rollback, records, depth, active,
                m ? load(m->mutation_lightweight_frame_commit_total) : 0));
            return types::make_string(sidx);
        });

    // Issue #1355: test hooks for render hot path enter/exit (thin wrappers).
    add("render-hotpath-enter", [&ev](const auto&) -> EvalValue {
        ev.enter_render_hotpath();
        return make_bool(true);
    });
    add("render-hotpath-exit", [&ev](const auto&) -> EvalValue {
        ev.exit_render_hotpath();
        return make_bool(true);
    });
    ObservabilityPrims::register_stats_impl("render-hotpath-depth", [](const auto&) -> EvalValue {
        return make_int(
            static_cast<std::int64_t>(aura::core::arena_policy::g_render_hotpath_depth));
    });
    // Issue #1355: int probes for Agent/tests.
    ObservabilityPrims::register_stats_impl(
        "mutation-lightweight-total", [&ev](const auto&) -> EvalValue {
            if (!ev.workspace_flat_)
                return make_int(0);
            return make_int(static_cast<std::int64_t>(ev.workspace_flat_->lightweight_total()));
        });
    add("mutation-lightweight-commit", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->lightweight_commit_total()));
    });
    add("mutation-lightweight-rollback", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_int(0);
        return make_int(
            static_cast<std::int64_t>(ev.workspace_flat_->lightweight_rollback_total()));
    });
    ObservabilityPrims::register_stats_impl(
        "mutation-lightweight-records", [&ev](const auto&) -> EvalValue {
            if (!ev.workspace_flat_)
                return make_int(0);
            return make_int(
                static_cast<std::int64_t>(ev.workspace_flat_->lightweight_records_total()));
        });
    ObservabilityPrims::register_stats_impl("mutation-log-size", [&ev](const auto&) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->mutation_count()));
    });

    // ── Issue #1356: HotTierTable dispatch stats ──
    ObservabilityPrims::register_stats_impl(
        "query:prim-dispatch-stats", [&ev](const auto&) -> EvalValue {
            auto& p = ev.primitives();
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            const auto hot_sz = static_cast<std::int64_t>(p.hot_table_size());
            const auto hits = static_cast<std::int64_t>(p.hot_dispatch_hits());
            const auto hits_r = static_cast<std::int64_t>(p.hot_dispatch_hits_render());
            const auto cold = static_cast<std::int64_t>(p.cold_dispatch_fallback());
            if (m) {
                m->prim_hot_table_size.store(static_cast<std::uint64_t>(hot_sz),
                                             std::memory_order_relaxed);
                m->prim_hot_dispatch_hits.store(static_cast<std::uint64_t>(hits),
                                                std::memory_order_relaxed);
                m->prim_hot_dispatch_hits_render.store(static_cast<std::uint64_t>(hits_r),
                                                       std::memory_order_relaxed);
                m->prim_cold_dispatch_fallback.store(static_cast<std::uint64_t>(cold),
                                                     std::memory_order_relaxed);
            }
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::format(
                "hot_table_size={} hot_dispatch_hits={} hot_dispatch_hits_render={} "
                "cold_dispatch_fallback={} hot_meta_count={} schema=1356",
                hot_sz, hits, hits_r, cold, static_cast<std::int64_t>(p.hot_meta_count())));
            return types::make_string(sidx);
        });
    ObservabilityPrims::register_stats_impl("prim-hot-table-size", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(ev.primitives().hot_table_size()));
    });
    add("prim-hot-dispatch-hits", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(ev.primitives().hot_dispatch_hits()));
    });
    add("prim-cold-dispatch-fallback", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(ev.primitives().cold_dispatch_fallback()));
    });

    // ── Issue #1357: render prim latency + frame time histogram ──
    ObservabilityPrims::register_stats_impl(
        "query:render-prim-call-stats", [&ev](const auto&) -> EvalValue {
            namespace rt = aura::compiler::render_telemetry;
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            if (m)
                m->render_obs_query_hits.fetch_add(1, std::memory_order_relaxed);
            // Compact string of tracked hot prims with calls/mean/min/max.
            std::string out = "schema=1357 active=1 ";
            std::uint64_t tracked_calls = 0;
            for (std::size_t i = 0; i < rt::kTrackedRenderPrimCount; ++i) {
                const auto& s = rt::tracked_prim_stats(i);
                const auto c = s.call_count.load(std::memory_order_relaxed);
                if (c == 0)
                    continue;
                tracked_calls += c;
                const auto tot = s.total_ns.load(std::memory_order_relaxed);
                const auto mean = tot / c;
                auto mn = s.min_ns.load(std::memory_order_relaxed);
                if (mn == std::numeric_limits<std::uint64_t>::max())
                    mn = 0;
                const auto mx = s.max_ns.load(std::memory_order_relaxed);
                out += std::format("{}:calls={} mean_ns={} min_ns={} max_ns={} ",
                                   rt::kTrackedRenderPrims[i], c, mean, mn, mx);
            }
            // Also surface slot-table hits for this Evaluator.
            std::uint64_t slot_calls = 0;
            for (const auto& s : ev.prim_latency_table()) {
                if (s)
                    slot_calls += s->call_count.load(std::memory_order_relaxed);
            }
            out += std::format("tracked_calls={} slot_table_calls={} samples={}", tracked_calls,
                               slot_calls, m ? m->render_prim_latency_samples.load() : 0);
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::move(out));
            return types::make_string(sidx);
        });

    ObservabilityPrims::register_stats_impl(
        "query:render-frame-time-histogram", [&ev](const auto&) -> EvalValue {
            namespace rt = aura::compiler::render_telemetry;
            auto& ft = rt::global_frame_time_stats();
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            if (m)
                m->render_obs_query_hits.fetch_add(1, std::memory_order_relaxed);
            const auto frames = ft.total_frames.load(std::memory_order_relaxed);
            const auto tot = ft.total_ns.load(std::memory_order_relaxed);
            const auto mean = frames ? tot / frames : 0;
            auto mn = ft.min_ns.load(std::memory_order_relaxed);
            if (mn == std::numeric_limits<std::uint64_t>::max())
                mn = 0;
            const auto mx = ft.max_ns.load(std::memory_order_relaxed);
            std::string buckets;
            for (int i = 0; i < rt::kFrameTimeBuckets; ++i) {
                if (i)
                    buckets += ',';
                buckets += std::to_string(
                    ft.bucket_counts[static_cast<std::size_t>(i)].load(std::memory_order_relaxed));
            }
            // Frames under 16ms (buckets 0..8 cover 0-16ms)
            std::uint64_t under_16 = 0;
            for (int i = 0; i <= 8; ++i)
                under_16 +=
                    ft.bucket_counts[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::format(
                "schema=1357 total_frames={} mean_frame_ns={} min_ns={} max_ns={} under_16ms={} "
                "buckets=[{}] labels=0-1,1-2,2-4,4-6,6-8,8-10,10-12,12-14,14-16,16-20,20-33,>33ms",
                frames, mean, mn, mx, under_16, buckets));
            return types::make_string(sidx);
        });

    // Int probes for tests
    ObservabilityPrims::register_stats_impl(
        "render-prim-latency-samples", [&ev](const auto&) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            const auto global =
                aura::compiler::render_telemetry::g_render_prim_latency_samples.load(
                    std::memory_order_relaxed);
            if (m)
                m->render_prim_latency_samples.store(global, std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(global));
        });
    ObservabilityPrims::register_stats_impl(
        "render-frame-time-samples", [](const auto&) -> EvalValue {
            auto& ft = aura::compiler::render_telemetry::global_frame_time_stats();
            return make_int(
                static_cast<std::int64_t>(ft.total_frames.load(std::memory_order_relaxed)));
        });

    ObservabilityPrims::register_stats_impl(
        "query:render-arena-frame-stats", [&ev](const auto&) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            auto load = [](const std::atomic<std::uint64_t>& a) {
                return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
            };
            // Minimal hash: reuse string_heap + FlatHashTable via pair list of ints
            // through existing pattern — return a 3-tuple pair chain as simple ints
            // via string report for Phase 1 simplicity.
            auto resets = m ? load(m->render_frame_reset_total) : 0;
            auto deferred = m ? load(m->render_frame_reset_deferred) : 0;
            auto reclaimed = m ? load(m->render_frame_reset_reclaimed) : 0;
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::format("resets={} deferred={} reclaimed={} schema=1315",
                                                  resets, deferred, reclaimed));
            return types::make_string(sidx);
        });
    // (arena:defrag-requested?) — query the defrag request flag.
    // Returns #t if a defrag was requested and not yet acted on,
    // #f otherwise. Foundation for fiber-coordinated defrag —
    // the fiber safepoint can read this and yield if set.
    ObservabilityPrims::register_stats_impl("arena:defrag-requested?",
                                            [&ev, destroy_defuse_index](const auto&) -> EvalValue {
                                                if (!ev.arena_)
                                                    return make_bool(false);
                                                return make_bool(ev.arena_->defrag_requested());
                                            });
    add("arena:compact-all", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.arena_group_)
            return make_int(0);
        if (ev.any_active_mutation_boundary()) {
            ev.compaction_paused_by_boundary_.fetch_add(1, std::memory_order_relaxed);
            return make_int(0);
        }
        return make_int(static_cast<std::int64_t>(ev.arena_group_->auto_compact()));
    });
    // (arena:adaptive-compact [name]) — Issue #335: adaptive
    // variant of compact-all that uses the per-module
    // savings-EMA to lower the trigger threshold when
    // recent compactions were productive. name is
    // optional; when provided, only that module is
    // compacted. When omitted, all modules are checked
    // individually. Returns total bytes reclaimed.
    add("arena:adaptive-compact", [&ev, destroy_defuse_index](const auto& a) -> EvalValue {
        if (!ev.arena_group_)
            return make_int(0);
        if (ev.any_active_mutation_boundary()) {
            ev.compaction_paused_by_boundary_.fetch_add(1, std::memory_order_relaxed);
            return make_int(0);
        }
        if (a.size() == 1 && is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return make_int(0);
            const auto& name = ev.string_heap_[idx];
            return make_int(static_cast<std::int64_t>(ev.arena_group_->adaptive_compact(name)));
        }
        return make_int(static_cast<std::int64_t>(ev.arena_group_->adaptive_compact_all()));
    });
    // (arena:compact-with-policy name policy) — Issue #430:
    // manual policy override. policy is one of:
    //   "force" — always compact (no threshold check)
    //   "auto"  — consult adaptive threshold (default
    //             behavior of arena:adaptive-compact)
    //   "skip"  — never compact (bumps skip counter
    //             so observability can see the manual skip)
    // Returns bytes reclaimed. Use "force" sparingly —
    // compaction is O(capacity) and can stall the
    // worker thread. The intended use case is a
    // memory-pressure watchdog that has decided the
    // adaptive threshold is too lax for the current
    // workload.
    add("arena:compact-with-policy", [&ev, destroy_defuse_index](const auto& a) -> EvalValue {
        if (!ev.arena_group_)
            return make_int(0);
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void(); // bad-arg signal; same shape as other primitives in this file
        auto nidx = as_string_idx(a[0]);
        auto pidx = as_string_idx(a[1]);
        if (nidx >= ev.string_heap_.size() || pidx >= ev.string_heap_.size())
            return make_int(0);
        const auto& name = ev.string_heap_[nidx];
        const auto& policy = ev.string_heap_[pidx];
        aura::ast::ArenaGroup::CompactPolicy p;
        if (policy == "force")
            p = aura::ast::ArenaGroup::CompactPolicy::Force;
        else if (policy == "auto")
            p = aura::ast::ArenaGroup::CompactPolicy::Auto;
        else if (policy == "skip")
            p = aura::ast::ArenaGroup::CompactPolicy::Skip;
        else
            return make_void(); // unknown policy → no-op (no arena state change)
        return make_int(static_cast<std::int64_t>(ev.arena_group_->compact_with_policy(name, p)));
    });
    // (arena:should-auto-compact? name) — Issue #335: cheap
    // O(1) probe that returns #t when the per-module
    // fragmentation ratio is at or above the adaptive
    // threshold. Used by the memory_pressure sampling
    // loop to decide whether to compact before the
    // critical threshold.
    add("arena:should-auto-compact?", [&ev, destroy_defuse_index](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]) || !ev.arena_group_)
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        const auto& name = ev.string_heap_[idx];
        return make_bool(ev.arena_group_->should_auto_compact(name));
    });
    // (arena:adaptive-stats) — Issue #335: returns the
    // adaptive-compact counters as a 2-tuple
    // (trigger-count . skip-count). Stats-only.
    ObservabilityPrims::register_stats_impl(
        "arena:adaptive-stats", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            if (!ev.arena_group_)
                return make_pair(ev.pairs_.size()); // empty pair
            const auto trig = ev.arena_group_->auto_compact_trigger_count();
            const auto skip = ev.arena_group_->auto_compact_skip_count();
            // Issues #1072 / #1488: return (trigger . skip) as ints only —
            // never string_heap_.push_back discarded intermediates (pollution
            // on long-running observability poll loops).
            auto car_idx = ev.pairs_.size();
            ev.pairs_.push_back({make_int(static_cast<std::int64_t>(trig)),
                                 make_int(static_cast<std::int64_t>(skip))});
            return make_pair(car_idx);
        });
    add("arena:shrink-to-fit", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.arena_)
            return make_void();
        ev.arena_->shrink_to_fit();
        return make_void();
    });
    // (arena:set-compact-threshold pct) — Issue #187: configure the
    // fragmentation ratio at which (arena:compact-all) triggers a
    // compact. pct is 0-95 (clamped). 50 = default.
    add("arena:set-compact-threshold", [&ev, destroy_defuse_index](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !ev.arena_group_)
            return make_void();
        ev.arena_group_->set_compact_threshold(static_cast<double>(as_int(a[0])) / 100.0);
        return make_void();
    });
    // (arena:estimate) — Issue #187: bytes that could be reclaimed
    // by a (arena:compact). Cheap O(1) check, no side effects.
    ObservabilityPrims::register_stats_impl(
        "arena:estimate", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            if (!ev.arena_)
                return make_int(0);
            return make_int(static_cast<std::int64_t>(ev.arena_->compact_estimate()));
        });
    // (arena:defrag-stats) — Issue #300 (P1): live-object
    // defragmentation observability. Returns a 5-tuple:
    //   (compaction-count
    //    defrag-attempted-count      ; # of defrag() calls (0 in foundation)
    //    fragmentation-bp           ; (cap-used)/cap in basis points 0-10000
    //    wasted-bytes               ; alignment padding total
    //    compact-estimate-bytes)    ; upper bound on what compact() can save
    // All ints. Sum across arena_group_ if available, else main arena.
    // Issue #300 foundation: defrag-attempted-count is always 0; the
    // field exists so follow-up commits (B/C) can increment without
    // changing the primitive's contract.
    ObservabilityPrims::register_stats_impl(
        "arena:defrag-stats", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            std::size_t compact_count = 0;
            std::size_t defrag_count = 0;
            std::size_t wasted = 0;
            std::size_t cap = 0;
            std::size_t used = 0;
            std::size_t compact_est = 0;
            // Sum across the main arena (ev.arena_) AND any per-module
            // arenas (ev.arena_group_). Issue #300 foundation: the
            // (arena:defrag) primitive operates on ev.arena_, so we
            // must include its stats in the read path for the
            // defrag_attempted_count to reflect the increment.
            if (ev.arena_) {
                auto s = ev.arena_->stats();
                compact_count += s.compaction_count;
                defrag_count += s.defrag_attempted_count;
                wasted += s.wasted;
                cap += s.capacity;
                used += s.used;
                compact_est += ev.arena_->compact_estimate();
            }
            if (ev.arena_group_) {
                auto per_module = ev.arena_group_->module_stats();
                for (auto& [name, s] : per_module) {
                    (void)name;
                    compact_count += s.compaction_count;
                    defrag_count += s.defrag_attempted_count;
                    wasted += s.wasted;
                    cap += s.capacity;
                    used += s.used;
                }
                for (auto& [name, s] : per_module) {
                    (void)s;
                    compact_est += ev.arena_group_->module_arena(name).compact_estimate();
                }
            }
            std::int64_t frag_bp =
                (cap > 0) ? static_cast<std::int64_t>(((cap - used) * 10000) / cap) : 0;
            // Build 5-tuple via (e1 . (e2 . (e3 . (e4 . e5)))) pattern,
            // matching the issue body contract. Cell order:
            //   e1=compaction-count, e2=defrag-attempted-count,
            //   e3=fragmentation-bp, e4=wasted-bytes,
            //   e5=compact-estimate-bytes (terminal int).
            auto p4 = ev.pairs_.size();
            ev.pairs_.push_back({make_int(static_cast<std::int64_t>(wasted)),
                                 make_int(static_cast<std::int64_t>(compact_est))});
            auto p3 = ev.pairs_.size();
            ev.pairs_.push_back({make_int(frag_bp), make_pair(p4)});
            auto p2 = ev.pairs_.size();
            ev.pairs_.push_back({make_int(defrag_count), make_pair(p3)});
            auto p1 = ev.pairs_.size();
            ev.pairs_.push_back(
                {make_int(static_cast<std::int64_t>(compact_count)), make_pair(p2)});
            return make_pair(p1);
        });
    // (arena:stats-json) — Issue #187: JSON snapshot of all managed
    // arenas (capacity, used, fragmentation, compaction count). For
    // dashboards and auto-tuners. Returns the JSON as a string.
    ObservabilityPrims::register_stats_impl(
        "arena:stats-json", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            std::string out;
            if (ev.arena_group_) {
                out = ev.arena_group_->stats_json();
            } else if (ev.arena_) {
                // Single-arena fallback: emit a one-entry JSON manually.
                auto s = ev.arena_->stats();
                out = std::format("{{\"arenas\":[{{\"name\":\"main\",\"used\":{},\"capacity\":{},"
                                  "\"peak_used\":{},\"allocs\":{},\"compaction_count\":{},"
                                  "\"last_compaction_saved\":{},\"total_compaction_saved\":{},"
                                  "\"fragmentation_ratio\":{:.3f},"
                                  "\"defrag_attempted_count\":{},\"last_defrag_saved\":{}}}],"
                                  "\"compact_threshold\":0.5}}",
                                  s.used, s.capacity, s.peak_used, s.allocation_count,
                                  s.compaction_count, s.last_compaction_saved,
                                  s.total_compaction_saved, s.fragmentation_ratio(),
                                  s.defrag_attempted_count, s.last_defrag_saved);
            } else {
                out = "{\"arenas\":[]}";
            }
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(out);
            return types::make_string(sidx);
        });
    // (string-pool:compact) — Issue #187 (P0): rehash the workspace's
    // StringPool to the smallest power-of-2 capacity that still
    // holds all live entries. Reclaims hash_tbl_ memory. Returns
    // bytes reclaimed. SymIds are stable (buf_ is monotonic).
    add("string-pool:compact", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.workspace_pool_ && !ev.canonical_pool())
            return make_int(0);
        auto* pool = ev.workspace_pool_ ? ev.workspace_pool_ : ev.canonical_pool();
        return make_int(static_cast<std::int64_t>(pool->compact()));
    });
    // (string-pool:stats) — Issue #187: StringPool observability.
    // Returns hash {entries, capacity, load-factor, data-size,
    // hash-bytes, fragmentation}.
    // (Built inline using the same hash-build pattern as
    //  gc-arena-info above.)
    ObservabilityPrims::register_stats_impl(
        "string-pool:stats", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            if (!ev.workspace_pool_ && !ev.canonical_pool())
                return make_void();
            auto* pool = ev.workspace_pool_ ? ev.workspace_pool_ : ev.canonical_pool();
            std::size_t entries = pool->entry_count();
            std::size_t cap = pool->hash_capacity();
            double lf = pool->load_factor();
            std::size_t ds = pool->data_size();
            std::size_t hb = pool->hash_table_bytes();
            double frag = pool->buf_fragmentation();

            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(8);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
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
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"entries", make_int(static_cast<std::int64_t>(entries))},
                {"capacity", make_int(static_cast<std::int64_t>(cap))},
                {"load-factor", make_float(lf)},
                {"data-size", make_int(static_cast<std::int64_t>(ds))},
                {"hash-bytes", make_int(static_cast<std::int64_t>(hb))},
                {"fragmentation", make_float(frag)},
            };
            return build_hash(kv);
        });

    // (dirty:reasons node-id) — Issue #188: return the per-node
    // dirty-reason bitmask. Useful for the type checker to decide
    // which targeted re-analysis pass to run, and for diagnostics
    // to surface "why is this node dirty". Bit values:
    //   0x01 = general (re-infer), 0x02 = constraint, 0x04 = occurrence,
    //   0x08 = ownership, 0x10 = coercion, 0x20 = struct, 0x40 = defuse,
    //   0x80 = ppa-hint. Returns 0 for clean nodes or out-of-range ids.
    // Multi-arg (node-id) — public add(); not zero-arity stats.
    add("dirty:reasons", [&ev, destroy_defuse_index](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        if (!ev.workspace_flat_)
            return make_int(0);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->dirty_reasons(id)));
    });
    // (dirty:ppa-reasons node-id) — Issue #277: return the per-node
    // PPA dirty bitmask from the orthogonal ppa_dirty_ column.
    //   0x01 = timing, 0x02 = power, 0x04 = area, 0x08 = backend-hint.
    add("dirty:ppa-reasons", [&ev, destroy_defuse_index](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        if (!ev.workspace_flat_)
            return make_int(0);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->ppa_dirty_reasons(id)));
    });
    // (dirty:counts) — Issue #188/#262/#277: aggregate per-reason dirty counts
    // across the workspace. Returns hash with integer fields:
    //   general, constraint, occurrence, ownership, coercion,
    //   struct, defuse, ppa-hint, timing, power, area, backend-hint, total
    //   (total is the number of dirty nodes, not the sum of bits).
    // Built inline using the same hash-build pattern as gc-arena-info.
    ObservabilityPrims::register_stats_impl(
        "dirty:counts", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
            if (!ev.workspace_flat_)
                return make_void();
            std::size_t gen = 0, con = 0, occ = 0, own = 0, coe = 0;
            std::size_t str = 0, def = 0, ppa = 0, total = 0;
            std::size_t timing = 0, power = 0, area = 0, backend_hint = 0;
            const auto& dirty = ev.workspace_flat_->dirty_column();
            const auto& ppa_dirty = ev.workspace_flat_->ppa_dirty_column();
            const auto n = std::max(dirty.size(), ppa_dirty.size());
            for (std::size_t i = 0; i < n; ++i) {
                auto b = i < dirty.size() ? dirty[i] : 0;
                auto pb = i < ppa_dirty.size() ? ppa_dirty[i] : 0;
                if (b == 0 && pb == 0)
                    continue;
                ++total;
                if (b & 0x01)
                    ++gen;
                if (b & 0x02)
                    ++con;
                if (b & 0x04)
                    ++occ;
                if (b & 0x08)
                    ++own;
                if (b & 0x10)
                    ++coe;
                if (b & 0x20)
                    ++str;
                if (b & 0x40)
                    ++def;
                // Issue #277 follow-up: ppa-hint aggregate counts nodes
                // that have ANY bit set in the orthogonal ppa_dirty_ column,
                // not the dirty_ mirror (kPpaHintDirty = 0x80). The mirror
                // gets wiped by type-checker cleanup (clear_dirty(id))
                // in infer_flat_partial, which would under-count ppa-hint
                // for already-re-inferred nodes. Counting directly from
                // ppa_dirty_ gives the right value regardless of cleanup
                // state. The timing/power/area/backend-hint fields below
                // already use this pattern.
                if (pb != 0)
                    ++ppa;
                if (pb & 0x01)
                    ++timing;
                if (pb & 0x02)
                    ++power;
                if (pb & 0x04)
                    ++area;
                if (pb & 0x08)
                    ++backend_hint;
            }
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                // Issue #277: 13 fields — need capacity > 13 (power of 2).
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
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
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"general", make_int(static_cast<std::int64_t>(gen))},
                {"constraint", make_int(static_cast<std::int64_t>(con))},
                {"occurrence", make_int(static_cast<std::int64_t>(occ))},
                {"ownership", make_int(static_cast<std::int64_t>(own))},
                {"coercion", make_int(static_cast<std::int64_t>(coe))},
                {"struct", make_int(static_cast<std::int64_t>(str))},
                {"defuse", make_int(static_cast<std::int64_t>(def))},
                {"ppa-hint", make_int(static_cast<std::int64_t>(ppa))},
                {"timing", make_int(static_cast<std::int64_t>(timing))},
                {"power", make_int(static_cast<std::int64_t>(power))},
                {"area", make_int(static_cast<std::int64_t>(area))},
                {"backend-hint", make_int(static_cast<std::int64_t>(backend_hint))},
                {"total", make_int(static_cast<std::int64_t>(total))},
            };
            return build_hash(kv);
        });

    // Issue #278 follow-up #2: (dirty:summary reason-mask) —
    // return a compact per-reason summary of dirty nodes, where
    // reason-mask is a bitmask of the reason bits the caller
    // cares about (1=general, 2=constraint, 4=occurrence, etc.,
    // matching the bits in dirty:counts). Returns a hash with
    // one key per reason that has at least one matching node.
    //
    // Use case: AI agent asking "what's currently dirty and
    // why?" — gets a compact summary without having to
    // iterate all dirty nodes. Pass 0 to get all reasons
    // (default).
    ObservabilityPrims::register_stats_impl(
        "dirty:summary", [&ev, destroy_defuse_index](std::span<const EvalValue> a) -> EvalValue {
            if (!ev.workspace_flat_)
                return make_void();
            std::uint32_t mask = 0;
            if (a.size() >= 1 && is_int(a[0])) {
                mask = static_cast<std::uint32_t>(as_int(a[0]));
            }
            if (mask == 0)
                mask = 0xFFFF; // default: all reasons
            // Accumulate the unique reason bits present in the
            // workspace, plus a count of nodes per reason. This
            // gives a compact per-reason summary without the full
            // node-id list (which can be 100s of entries).
            std::uint32_t present_bits = 0;
            std::size_t total = 0;
            std::size_t gen = 0, con = 0, occ = 0, own = 0, coe = 0;
            std::size_t str = 0, def = 0, ppa = 0;
            const auto& dirty = ev.workspace_flat_->dirty_column();
            const auto& ppa_dirty = ev.workspace_flat_->ppa_dirty_column();
            const auto n = std::max(dirty.size(), ppa_dirty.size());
            for (std::size_t i = 0; i < n; ++i) {
                auto b = i < dirty.size() ? dirty[i] : 0;
                auto pb = i < ppa_dirty.size() ? ppa_dirty[i] : 0;
                if (b == 0 && pb == 0)
                    continue;
                ++total;
                if (b & 0x01) {
                    present_bits |= 0x01;
                    ++gen;
                }
                if (b & 0x02) {
                    present_bits |= 0x02;
                    ++con;
                }
                if (b & 0x04) {
                    present_bits |= 0x04;
                    ++occ;
                }
                if (b & 0x08) {
                    present_bits |= 0x08;
                    ++own;
                }
                if (b & 0x10) {
                    present_bits |= 0x10;
                    ++coe;
                }
                if (b & 0x20) {
                    present_bits |= 0x20;
                    ++str;
                }
                if (b & 0x40) {
                    present_bits |= 0x40;
                    ++def;
                }
                if (b & 0x80) {
                    present_bits |= 0x80;
                    ++ppa;
                }
            }
            // Build the result hash.
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"present-bits", make_int(static_cast<std::int64_t>(present_bits))},
                {"requested-mask", make_int(static_cast<std::int64_t>(mask))},
                {"total", make_int(static_cast<std::int64_t>(total))},
            };
            // Add per-reason counts only for reasons the caller
            // requested (mask) AND which are present.
            if ((mask & 0x01) && (present_bits & 0x01))
                kv.push_back({"general", make_int(static_cast<std::int64_t>(gen))});
            if ((mask & 0x02) && (present_bits & 0x02))
                kv.push_back({"constraint", make_int(static_cast<std::int64_t>(con))});
            if ((mask & 0x04) && (present_bits & 0x04))
                kv.push_back({"occurrence", make_int(static_cast<std::int64_t>(occ))});
            if ((mask & 0x08) && (present_bits & 0x08))
                kv.push_back({"ownership", make_int(static_cast<std::int64_t>(own))});
            if ((mask & 0x10) && (present_bits & 0x10))
                kv.push_back({"coercion", make_int(static_cast<std::int64_t>(coe))});
            if ((mask & 0x20) && (present_bits & 0x20))
                kv.push_back({"struct", make_int(static_cast<std::int64_t>(str))});
            if ((mask & 0x40) && (present_bits & 0x40))
                kv.push_back({"defuse", make_int(static_cast<std::int64_t>(def))});
            if ((mask & 0x80) && (present_bits & 0x80))
                kv.push_back({"ppa-hint", make_int(static_cast<std::int64_t>(ppa))});
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
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
    // (memory-pressure) — Assess overall memory pressure and suggest actions.
    //
    //   Returns hash:
    //     {
    //       level: "low" | "medium" | "high" | "critical",
    //       used-pct: 87,                  ; overall usage %
    //       total-used: 12.5,              ; MB
    //       total-capacity: 16.0,          ; MB
    //       top-arena: "json.aura",        ; highest-pct arena name (or "" if none)
    //       top-pct: 92,                   ; top arena's pct (or 0)
    //       suggestions: ["gc-module json.aura", "gc-temp"]  ; vector of strings
    //     }
    //
    //   Thresholds (percent of arena capacity used):
    //     low      < 60
    //     medium   60-79
    //     high     80-94
    //     critical >= 95
    //
    //   Suggestions: for each arena with used-pct >= 80, add "gc-module <name>".
    //   If no gc-temp has been called in the last 100 evaluations, also
    //   add "gc-temp".
    //
    //   Tie-breaking for top-arena: highest used-pct, then largest used
    //   bytes, then name (lexicographic) for determinism.
    add("memory-pressure", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        // Snapshot arena state.
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

        // Determine level from overall used-pct.
        const char* level = "low";
        if (overall >= 95)
            level = "critical";
        else if (overall >= 80)
            level = "high";
        else if (overall >= 60)
            level = "medium";

        // Find top-arena (highest used-pct, then largest used, then name asc).
        std::string top_name;
        int top_pct = 0;
        double top_used = 0.0;
        for (auto& s : snaps) {
            if (s.pct > top_pct || (s.pct == top_pct && s.used > top_used) ||
                (s.pct == top_pct && s.used == top_used && s.name < top_name)) {
                top_name = s.name;
                top_pct = s.pct;
                top_used = s.used;
            }
        }

        // Build suggestions: for each arena with used-pct >= 80, add a
        // "gc-module <name>" hint. If no recent gc-temp call (within the
        // last 100 evaluations), also add "gc-temp".
        std::vector<EvalValue> suggestions;
        for (auto& s : snaps) {
            if (s.pct >= 80) {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back("gc-module " + s.name);
                suggestions.push_back(make_string(sidx));
            }
        }
        if (ev.eval_depth_ - ev.last_gc_temp_eval_depth_ >
            ev.memory_policy_.recent_gc_temp_window) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("gc-temp");
            suggestions.push_back(make_string(sidx));
        }

        // Build the result hash. Inline Swiss-table construction (same
        // shape as gc-arena-info's build_hash, 8-slot capacity).
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto cap = ht->capacity;
        // Helper: insert a (string-key, EvalValue) pair into the hash.
        // String values are interned in ev.string_heap_ first.
        auto hput = [&](const std::string& k, const EvalValue& v) -> bool {
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
            auto kidx = ev.string_heap_.size();
            ev.string_heap_.push_back(k);
            EvalValue key_ev = make_string(kidx);
            for (std::size_t at = 0; at < cap; ++at) {
                auto idx = ((h >> 1) + at) & (cap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = key_ev.val;
                    vals[idx] = v.val;
                    ht->size++;
                    return true;
                }
            }
            return false;
        };

        // String values: intern the level and top_name, then build String EvalValues.
        auto level_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(level);
        auto top_name_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(top_name);

        // Suggestions vector
        auto sugg_vidx = ev.vector_heap_.size();
        ev.vector_heap_.push_back(std::move(suggestions));

        bool ok = true;
        ok = ok && hput("level", make_string(level_idx));
        ok = ok && hput("used-pct", make_int(overall));
        ok = ok && hput("total-used", make_float(total_used));
        ok = ok && hput("total-capacity", make_float(total_cap));
        ok = ok && hput("top-arena", make_string(top_name_idx));
        ok = ok && hput("top-pct", make_int(top_pct));
        ok = ok && hput("suggestions", make_vector(sugg_vidx));
        if (!ok) {
            FlatHashTable::destroy(ht);
            return make_void();
        }
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #623: production-harden arena auto-compact threshold.
    //
    // (arena:auto-compact-threshold) — read the current
    // fragmentation-ratio threshold below which auto-compact
    // does NOT trigger. Default 0.50 in the C++ side
    // (ArenaGroup::compact_threshold_); settable via the
    // primitive below. Returns -1 if no arena group is loaded,
    // else the threshold * 100 (integer percentage 0..95).
    //
    // (arena:set-auto-compact-threshold ratio) — write the
    // threshold. ratio is a percentage 0..95; out-of-range args
    // are clamped to the valid range by the C++ side
    // (std::clamp). Returns the previous value (also 0..95).
    // Bumps arena_auto_compact_threshold_set_total on every
    // call. The Agent's memory-pressure watchdog can tune this
    // at runtime to make auto-compact more aggressive under
    // sustained churn, more lax when fragmentation is stable.
    //
    // P0 ships these 2 primitives + 1 counter. The bigger
    // #623 work (auto-trigger wiring on allocate, basic live
    // defrag for small tiers, SoA dirty wiring) was already
    // done by #604 (fiber/GC coord) + #685 (alloc-path
    // policy counters) + #300 (defrag foundation). The
    // threshold-tunables are the production-harden layer on
    // top of the existing observability surface.
    add("arena:auto-compact-threshold", [&ev](const auto&) -> EvalValue {
        if (!ev.arena_group_)
            return make_int(-1);
        const double cur = ev.arena_group_->compact_threshold();
        return make_int(static_cast<std::int64_t>(cur * 100.0));
    });
    add("arena:set-auto-compact-threshold", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (!ev.arena_group_)
            return make_int(-1);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->arena_auto_compact_threshold_set_total.fetch_add(1, std::memory_order_relaxed);
        const double prev = ev.arena_group_->compact_threshold();
        // Default to no-op return of previous value (reads-only mode).
        if (a.empty() || !is_int(a[0]))
            return make_int(static_cast<std::int64_t>(prev * 100.0));
        const std::int64_t requested = as_int(a[0]);
        // Clamp 0..95 internally, same as the C++ std::clamp in
        // set_compact_threshold. Negative → 0, > 95 → 95.
        std::int64_t clamped = requested;
        if (clamped < 0)
            clamped = 0;
        else if (clamped > 95)
            clamped = 95;
        ev.arena_group_->set_compact_threshold(static_cast<double>(clamped) / 100.0);
        return make_int(static_cast<std::int64_t>(prev * 100.0));
    });

    // Issue #1361 / #1665: free closure by id.
    // - int id → JIT runtime table (aura_free_closure + g_closure_freed)
    // - Closure value → tree-walker Evaluator::closures_ erase (#1665
    //   durable free so scan_live_closures never iterates dead slots)
    add("closure:free!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_bool(false);
        if (is_int(a[0])) {
            aura_free_closure(as_int(a[0]));
            return make_bool(true);
        }
        // Issue #1665: erase TW map entry (was no-op; left dead slots in scan).
        if (is_closure(a[0])) {
            const auto cid = static_cast<ClosureId>(as_closure_id(a[0]));
            (void)ev.erase_active_closure(cid);
            return make_bool(true);
        }
        return make_bool(false);
    });

    // Issue #1361 probes: (closure:free-stats) → hash of free/reuse/live/slots
    ObservabilityPrims::register_stats_impl("closure:free-stats", [&ev](const auto&) -> EvalValue {
        auto* ht = FlatHashTable::create(16);
        if (!ht)
            return make_void();
        auto put = [&](const char* k, std::int64_t v) {
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (const char* p = k; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            auto kidx = ev.string_heap_.size();
            ev.string_heap_.push_back(k);
            EvalValue key_ev = make_string(kidx);
            EvalValue val_ev = make_int(v);
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = key_ev.val;
                    vals[idx] = val_ev.val;
                    ht->size++;
                    return;
                }
            }
        };
        put("free-total", static_cast<std::int64_t>(aura_closure_free_total()));
        put("reuse-total", static_cast<std::int64_t>(aura_closure_reuse_total()));
        put("live", static_cast<std::int64_t>(aura_closure_live_count()));
        put("slots", static_cast<std::int64_t>(aura_closure_slot_count()));
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

} // namespace aura::compiler::primitives_detail
