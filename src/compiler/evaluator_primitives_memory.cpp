// evaluator_primitives_memory.cpp — P0 step 19: coverage / gc / arena / dirty / memory-pressure
// extracted from Evaluator constructor.

module;

#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "runtime_shared.h"
#include "messaging_bridge.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

void register_memory_primitives(PrimRegistrar add, Evaluator& ev,
                                std::function<void()> destroy_defuse_index) {

    // ── coverage-report — 编译器路径覆盖率 ──────────────────
    add("coverage-report", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
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
    add("gc-stats", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        std::uint64_t root_count = 0;
        for (auto& [id, _] : ev.closures_) {
            if (id < ev.gc_safe_closure_id_)
                ++root_count;
        }
        auto result =
            std::format("string:{}/pairs:{}/cells:{}/err:{}/hash:{}/vec:{}/opq:{}/cls:{}/root:{}",
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
    add("gc-module-count", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(ev.modules_.size()));
    });

    // (type-registry-stats) — Issue #78: TypeRegistry observability.
    // Returns a hash with current size, generation, and predefined count.
    // Use this to monitor TypeRegistry growth in long-running sessions.
    add("type-registry-stats", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
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
        return make_int(static_cast<std::int64_t>(ev.arena_->compact()));
    });
    add("arena:compact-all", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.arena_group_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.arena_group_->auto_compact()));
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
    add("arena:estimate", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.arena_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.arena_->compact_estimate()));
    });
    // (arena:stats-json) — Issue #187: JSON snapshot of all managed
    // arenas (capacity, used, fragmentation, compaction count). For
    // dashboards and auto-tuners. Returns the JSON as a string.
    add("arena:stats-json", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        std::string out;
        if (ev.arena_group_) {
            out = ev.arena_group_->stats_json();
        } else if (ev.arena_) {
            // Single-arena fallback: emit a one-entry JSON manually.
            auto s = ev.arena_->stats();
            out = std::format("{{\"arenas\":[{{\"name\":\"main\",\"used\":{},\"capacity\":{},"
                              "\"peak_used\":{},\"allocs\":{},\"compaction_count\":{},"
                              "\"last_compaction_saved\":{},\"total_compaction_saved\":{},"
                              "\"fragmentation_ratio\":{:.3f}}}],\"compact_threshold\":0.5}}",
                              s.used, s.capacity, s.peak_used, s.allocation_count,
                              s.compaction_count, s.last_compaction_saved, s.total_compaction_saved,
                              s.fragmentation_ratio());
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
    add("string-pool:stats", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.workspace_pool_ && !ev.canonical_pool())
            return make_void();
        auto* pool = ev.workspace_pool_ ? ev.workspace_pool_ : ev.canonical_pool();
        std::size_t entries = pool->entry_count();
        std::size_t cap = pool->hash_capacity();
        double lf = pool->load_factor();
        std::size_t ds = pool->data_size();
        std::size_t hb = pool->hash_table_bytes();
        double frag = pool->buf_fragmentation();

        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
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
    //   0x08 = ownership, 0x10 = coercion. Returns 0 for clean nodes
    //   or out-of-range ids.
    add("dirty:reasons", [&ev, destroy_defuse_index](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        if (!ev.workspace_flat_)
            return make_int(0);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->dirty_reasons(id)));
    });
    // (dirty:counts) — Issue #188: aggregate per-reason dirty counts
    // across the workspace. Returns hash with 5 integer fields:
    //   general, constraint, occurrence, ownership, coercion, total
    //   (total is the number of dirty nodes, not the sum of bits).
    // Built inline using the same hash-build pattern as gc-arena-info.
    add("dirty:counts", [&ev, destroy_defuse_index](const auto&) -> EvalValue {
        if (!ev.workspace_flat_)
            return make_void();
        std::size_t gen = 0, con = 0, occ = 0, own = 0, coe = 0, total = 0;
        const auto& dirty = ev.workspace_flat_->dirty_column();
        for (std::size_t i = 0; i < dirty.size(); ++i) {
            auto b = dirty[i];
            if (b == 0)
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
        }
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
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
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"general", make_int(static_cast<std::int64_t>(gen))},
            {"constraint", make_int(static_cast<std::int64_t>(con))},
            {"occurrence", make_int(static_cast<std::int64_t>(occ))},
            {"ownership", make_int(static_cast<std::int64_t>(own))},
            {"coercion", make_int(static_cast<std::int64_t>(coe))},
            {"total", make_int(static_cast<std::int64_t>(total))},
        };
        return build_hash(kv);
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
        if (ev.eval_depth_ - ev.last_gc_temp_eval_depth_ > ev.memory_policy_.recent_gc_temp_window) {
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
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
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

}

} // namespace aura::compiler::primitives_detail
