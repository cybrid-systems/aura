// evaluator_primitives_agent.cpp — P0 step 18: agent / synthesize / intend / strategy
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.
//
// Issue #2627: auto-evolve-* convenience prims removed; agent:tick is the loop surface.
// Issue #1973: strategy:* (4 names) gated by AURA_ENABLE_STRATEGY in
// register_auto_evolve_primitives; query:strategy-evolution-stats stays on.
// See docs/strategy.md.
// Issue #1974: synthesize:* (4 names) gated by AURA_ENABLE_SYNTHESIZE in
// register_synthesize_primitives; query:templates stays always on.
// See docs/synthesize.md.

module;

#include "runtime_shared.h"
// Issue #444: CompilerMetrics is the central observability
// struct; the strategy pheromone counters live there.
// observability_metrics.h is a plain header (not a
// module), so we include it directly here in the module
// preamble (avoids the import-only restriction on .h).
#include "observability_metrics.h"
#include "hash_meta.h" // FNV constants (#901)
#include "core/gc_hooks.h"
#include "security_capabilities.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "serve/parallel_orch.h"
// Issue #2078: header-only AgentNameTable definition (see .h for why
// not in evaluator.ixx's global fragment).
#include "compiler/agent_name_table.h"
#include "compiler/aot_hot_update_health.hh" // Issue #2543: hot-update health throttle
#include "orch/orch.h"
#include "orch/security_schedule_gate.h" // #2660 make_security_schedule_input_live + admit_security_schedule
#include "compiler/typed_mutation_audit.h" // #2660 typed_audit::production_defaults_active
#include <atomic>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

// Default ON when the TU is compiled outside the CMake graph (tools/IDE).
#ifndef AURA_ENABLE_SYNTHESIZE
#define AURA_ENABLE_SYNTHESIZE 1
#endif
#ifndef AURA_ENABLE_STRATEGY
#define AURA_ENABLE_STRATEGY 1
#endif

module aura.compiler.evaluator;

import std;
import aura.core.ast;
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
using types::make_keyword;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Issue #1720 helpers as Evaluator members (implemented below namespace).
// Declared in evaluator.ixx.

namespace {
    std::vector<std::pair<std::string, std::string>> g_template_patterns;
    std::vector<std::vector<std::string>> g_template_params;

    // Issue #1713 / #1719: ClosureId live before apply_closure.
    // TW: find_active_closure; JIT: exists && !is_freed. Never call
    // aura_closure_is_freed alone on TW ids (OOR ⇒ "freed").
    bool agent_cid_live(Evaluator& ev, std::uint64_t raw) {
        if (raw == 0)
            return false;
        if (ev.find_active_closure(static_cast<ClosureId>(raw)).has_value())
            return true;
        const auto id = static_cast<std::int64_t>(raw);
        return aura_closure_exists(id) != 0 && aura_closure_is_freed(id) == 0;
    }
    void agent_note_closure_freed_tick(Evaluator& ev) {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->agent_closure_freed_during_tick.fetch_add(1, std::memory_order_relaxed);
    }
    void agent_note_closure_freed_call(Evaluator& ev) {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->agent_closure_freed_during_call.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #1716: thread-local PRNG for synthesize:optimize GA mutations.
    // std::rand() is not thread-safe and races under concurrent fibers.
    std::mt19937& agent_prng() {
        thread_local std::mt19937 rng{std::random_device{}()};
        return rng;
    }
    unsigned agent_rand_below(unsigned n) {
        if (n == 0)
            return 0;
        return std::uniform_int_distribution<unsigned>(0, n - 1)(agent_prng());
    }
    double agent_rand_unit() {
        return std::uniform_real_distribution<double>(0.0, 1.0)(agent_prng());
    }

    // Issue #1717: RAII swap of evaluator workspace onto a temporary
    // WorkspaceTree child. Restores flat/pool and delete_child on scope exit
    // (exception-safe; closes the bare-swap UAF / leak window).
    class WorkspaceSwapGuard {
    public:
        WorkspaceSwapGuard(Evaluator& ev, WorkspaceTree& tree, const char* child_name)
            : ev_(ev)
            , tree_(tree)
            , saved_flat_(ev.workspace_flat())
            , saved_pool_(ev.workspace_pool())
            , ws_id_(tree.create_child(child_name, tree.active_idx(), saved_flat_, saved_pool_))
            , valid_(ws_id_ > 0) {
            if (!valid_)
                return;
            tree_.ensure_local_flat(ws_id_);
            auto& ws = tree_.nodes_[ws_id_];
            // Public setters (guard lives outside Evaluator friend body).
            ev_.set_workspace_flat(ws.flat);
            ev_.set_workspace_pool(ws.pool);
        }
        ~WorkspaceSwapGuard() { release(); }
        WorkspaceSwapGuard(const WorkspaceSwapGuard&) = delete;
        WorkspaceSwapGuard& operator=(const WorkspaceSwapGuard&) = delete;
        [[nodiscard]] bool valid() const noexcept { return valid_; }
        void release() noexcept {
            if (!valid_)
                return;
            ev_.set_workspace_flat(saved_flat_);
            ev_.set_workspace_pool(saved_pool_);
            tree_.delete_child(ws_id_);
            valid_ = false;
        }

    private:
        Evaluator& ev_;
        WorkspaceTree& tree_;
        ast::FlatAST* saved_flat_;
        ast::StringPool* saved_pool_;
        std::uint32_t ws_id_;
        bool valid_;
    };

    // Issue #1236: JSON string escape for LLM payload construction.
    std::string json_escape(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned>(static_cast<unsigned char>(c)));
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
                    break;
            }
        }
        return out;
    }

    // Issue #1249: bounds-checked string-heap load — never OOB-subscript.
    // Takes the heap by ref so call sites (friend lambdas) pass ev.string_heap_.
    template <typename Heap>
    [[nodiscard]] std::string heap_str_from(const Heap& heap, const EvalValue& v) {
        if (!types::is_string(v))
            return {};
        const auto idx = types::as_string_idx(v);
        if (idx >= heap.size())
            return {};
        return heap[idx];
    }

} // namespace

void register_auto_evolve_primitives(PrimRegistrar add_raw, Evaluator& ev) {
    // Issue #1232 Phase 1: gate all auto-evolve primitives with kCapSelfEvo.
    // Local lambda (not free function) so private Evaluator heaps are reachable
    // the same way other agent register_* lambdas already do.
    // Also wraps agent:/strategy: adds co-registered in this function (same
    // self-evo capability surface historically).
    auto add = [&ev, add_raw = std::move(add_raw)](std::string name, PrimFn fn) {
        add_raw(std::move(name),
                PrimFn{[&ev, fn = std::move(fn)](std::span<const EvalValue> a) -> EvalValue {
                    if (ev.sandbox_mode() &&
                        !ev.has_capability(aura::compiler::security::kCapSelfEvo) &&
                        !ev.has_capability(aura::compiler::security::kCapWildcard)) {
                        ev.bump_capability_denial();
                        return make_primitive_error(ev.string_heap_, ev.error_values_,
                                                    "capability denied: self-evo required",
                                                    ev.primitive_error_counter_ptr());
                    }
                    return fn(a);
                }});
    };

    // Issue #2627: auto-evolve-* convenience prims removed; agent:tick keeps loop state.

    // ── Issues #1327 Phase 1: agent service bridge (single entry points) ──
    // Full SelfEvolutionService C++ move is multi-week; Phase 1 exposes
    // agent:tick / agent:running? as the stable Aura surface that forwards
    // to the existing auto-evolve machinery (legacy names still work).
    // Always registered: when AURA_ENABLE_AUTO_EVOLVE=0, agent:tick degrades
    // (lookup of auto-evolve-tick/once fails → false / 0).
    add("agent:running?", [&ev](const auto&) -> EvalValue {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->agent_legacy_auto_evolve_hits.fetch_add(1, std::memory_order_relaxed);
        return make_bool(ev.auto_evolve_running_);
    });

    // Issue #2627: one-shot / loop body inlined (was auto-evolve-tick / once).
    add("agent:tick", [&ev](const auto& a) -> EvalValue {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->agent_tick_total.fetch_add(1, std::memory_order_relaxed);
        auto run_once = [&](std::int64_t detect_cid, std::int64_t fix_cid) -> EvalValue {
            if (!agent_cid_live(ev, detect_cid) || !agent_cid_live(ev, fix_cid)) {
                agent_note_closure_freed_tick(ev);
                return make_int(0);
            }
            auto detect_result = ev.apply_closure(detect_cid, {});
            if (!detect_result)
                return make_int(0);
            EvalValue current = *detect_result;
            std::int64_t fixed = 0;
            while (is_pair(current)) {
                auto idx = as_pair_idx(current);
                if (idx >= ev.pairs_.size())
                    break;
                auto gap = ev.pairs_[idx].car;
                if (!agent_cid_live(ev, fix_cid)) {
                    agent_note_closure_freed_tick(ev);
                    break;
                }
                auto fix_result = ev.apply_closure(fix_cid, {gap});
                if (fix_result && is_bool(*fix_result) && as_bool(*fix_result))
                    ++fixed;
                current = ev.pairs_[idx].cdr;
            }
            return make_int(fixed);
        };
        // Background loop state (legacy auto_evolve_* fields).
        if (ev.auto_evolve_running_) {
            if (ev.auto_evolve_detect_closure_ == 0 || ev.auto_evolve_fix_closure_ == 0)
                return make_bool(false);
            if (!agent_cid_live(ev, ev.auto_evolve_detect_closure_) ||
                !agent_cid_live(ev, ev.auto_evolve_fix_closure_)) {
                ev.auto_evolve_running_ = false;
                ev.auto_evolve_detect_closure_ = 0;
                ev.auto_evolve_fix_closure_ = 0;
                agent_note_closure_freed_tick(ev);
                return make_bool(false);
            }
            if (auto* fn = aura::gc_hooks::g_arena_safepoint_check.load(std::memory_order_relaxed))
                fn();
            ++ev.auto_evolve_cycle_count_;
            auto detect_result = ev.apply_closure(ev.auto_evolve_detect_closure_, {});
            if (!detect_result)
                return make_bool(true);
            EvalValue current = *detect_result;
            while (is_pair(current)) {
                auto idx = as_pair_idx(current);
                if (idx >= ev.pairs_.size())
                    break;
                auto gap = ev.pairs_[idx].car;
                if (!agent_cid_live(ev, ev.auto_evolve_fix_closure_)) {
                    ev.auto_evolve_running_ = false;
                    ev.auto_evolve_detect_closure_ = 0;
                    ev.auto_evolve_fix_closure_ = 0;
                    agent_note_closure_freed_tick(ev);
                    return make_bool(false);
                }
                auto fix_result = ev.apply_closure(ev.auto_evolve_fix_closure_, {gap});
                if (fix_result && is_bool(*fix_result) && as_bool(*fix_result))
                    ++ev.auto_evolve_total_fixed_;
                current = ev.pairs_[idx].cdr;
            }
            return make_bool(true);
        }
        if (a.size() >= 2 && is_closure(a[0]) && is_closure(a[1]))
            return run_once(as_closure_id(a[0]), as_closure_id(a[1]));
        return make_int(0);
    });

#if AURA_ENABLE_STRATEGY
    // ── Issue #444: strategy evolution controller ────────
    //
    // 3 built-in strategies. Each mutation success bumps
    // the strategy's "successes" counter; a plateau
    // bumps "escalations" + switches the active strategy.
    //
    //   "coverage-greedy" — pick the node with the most
    //     coverage gaps (highest density). Best when the
    //     verify-dirty count is high.
    //   "bug-fix-priority" — pick the node that's been
    //     failing assertions. Best when verify-dirty has
    //     kAssertionDirty set on many nodes.
    //   "minimal-mutation" — pick the smallest mutation
    //     that might help (single-line change). Best when
    //     prior mutations caused regressions.
    //
    // The controller is rule-based (no PID in the MVP —
    // the PID + pheromone accumulation comes in a follow-
    // up issue once the baseline coverage curves are
    // established). The escalation rule:
    //   if (coverage_delta < threshold) &&
    //      (successes_in_window == 0):
    //     escalate (e.g. coverage-greedy → bug-fix-priority)
    // Issue #1714: invalid name / bad args return tagged make_merr pairs
    // (not silent make_void) so EDSL callers can distinguish typos from
    // success (int) and from deliberate void returns elsewhere.
    // Issue #1973: commercial strategy: vertical (AURA_ENABLE_STRATEGY).
    add("strategy:set-strategy", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return ev.make_merr("bad-arg", "usage: (strategy:set-strategy strategy-name)");
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return ev.make_merr("bad-arg", "string index out of range");
        const std::string& name = ev.string_heap_[idx];
        if (name != "coverage-greedy" && name != "bug-fix-priority" && name != "minimal-mutation") {
            return ev.make_merr(
                "unknown-strategy",
                std::string("unknown strategy: \"") + name +
                    "\" (expected: coverage-greedy | bug-fix-priority | minimal-mutation)");
        }
        {
            // Issue #1720 / #1722: guard active_strategy_ with strategies_mtx_.
            std::unique_lock<std::shared_mutex> lk(ev.strategies_mtx_);
            ev.active_strategy_ = name;
        }
        // Bump the hits counter for the new strategy.
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            if (name == "coverage-greedy")
                m->strategy_greedy_hits.fetch_add(1, std::memory_order_relaxed);
            else if (name == "bug-fix-priority")
                m->strategy_bugfix_hits.fetch_add(1, std::memory_order_relaxed);
            else if (name == "minimal-mutation")
                m->strategy_minimal_hits.fetch_add(1, std::memory_order_relaxed);
        }
        return make_int(static_cast<std::int64_t>(name.size()));
    });

    add("strategy:active", [&ev](const auto&) -> EvalValue {
        std::string active;
        {
            std::shared_lock<std::shared_mutex> lk(ev.strategies_mtx_);
            if (ev.active_strategy_.empty())
                return make_void();
            active = ev.active_strategy_;
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(active));
        return make_string(sidx);
    });

    // (strategy:report-success strategy-name) — call
    // after a successful mutation driven by the given
    // strategy. Bumps the strategy's success counter.
    add("strategy:report-success", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        const std::string& name = ev.string_heap_[idx];
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            if (name == "coverage-greedy")
                m->strategy_greedy_successes.fetch_add(1, std::memory_order_relaxed);
            else if (name == "bug-fix-priority")
                m->strategy_bugfix_successes.fetch_add(1, std::memory_order_relaxed);
            else if (name == "minimal-mutation")
                m->strategy_minimal_successes.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(true);
    });

    // (strategy:escalate reason) — record a strategy
    // escalation event (the controller decided to switch
    // strategies due to a coverage plateau or a
    // regression). Reason is a string for observability.
    add("strategy:escalate", [&ev](const auto& a) -> EvalValue {
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            m->strategy_escalations.fetch_add(1, std::memory_order_relaxed);
        }
        (void)a;
        return make_bool(true);
    });
#endif // AURA_ENABLE_STRATEGY

    // (query:strategy-evolution-stats) — Issue #444:
    // hash variant of the strategy pheromone counters.
    // 7 fields:
    //   - active-strategy         (string, current)
    //   - greedy-hits / greedy-successes
    //   - bugfix-hits / bugfix-successes
    //   - minimal-hits / minimal-successes
    //   - escalations
    // Always registered (#1973 only gates strategy:* prefix).
    ObservabilityPrims::register_stats_impl(
        "query:strategy-evolution-stats", [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(32);
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
            std::uint64_t greedy_h = 0, greedy_s = 0;
            std::uint64_t bugfix_h = 0, bugfix_s = 0;
            std::uint64_t minimal_h = 0, minimal_s = 0;
            std::uint64_t escalations = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
                greedy_h = m->strategy_greedy_hits.load(std::memory_order_relaxed);
                greedy_s = m->strategy_greedy_successes.load(std::memory_order_relaxed);
                bugfix_h = m->strategy_bugfix_hits.load(std::memory_order_relaxed);
                bugfix_s = m->strategy_bugfix_successes.load(std::memory_order_relaxed);
                minimal_h = m->strategy_minimal_hits.load(std::memory_order_relaxed);
                minimal_s = m->strategy_minimal_successes.load(std::memory_order_relaxed);
                escalations = m->strategy_escalations.load(std::memory_order_relaxed);
            }
            // active-strategy as a string field (Issue #1720 lock).
            std::string active;
            {
                std::shared_lock<std::shared_mutex> lk(ev.strategies_mtx_);
                active = ev.active_strategy_;
            }
            auto active_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(active);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"active-strategy", make_string(active_idx)},
                {"greedy-hits", make_int(static_cast<std::int64_t>(greedy_h))},
                {"greedy-successes", make_int(static_cast<std::int64_t>(greedy_s))},
                {"bugfix-hits", make_int(static_cast<std::int64_t>(bugfix_h))},
                {"bugfix-successes", make_int(static_cast<std::int64_t>(bugfix_s))},
                {"minimal-hits", make_int(static_cast<std::int64_t>(minimal_h))},
                {"minimal-successes", make_int(static_cast<std::int64_t>(minimal_s))},
                {"escalations", make_int(static_cast<std::int64_t>(escalations))},
            };
            return build_hash(kv);
        });
}

void register_synthesize_primitives(PrimRegistrar add_raw, Evaluator& ev,
                                    std::function<void()> destroy_defuse_index) {
    // Issue #1232 Phase 1: gate synthesis primitives with kCapSynthesize.
    // Also wraps query:templates (always registered) for consistent cap checks.
    auto add = [&ev, add_raw = std::move(add_raw)](std::string name, PrimFn fn) {
        add_raw(std::move(name),
                PrimFn{[&ev, fn = std::move(fn)](std::span<const EvalValue> a) -> EvalValue {
                    if (ev.sandbox_mode() &&
                        !ev.has_capability(aura::compiler::security::kCapSynthesize) &&
                        !ev.has_capability(aura::compiler::security::kCapWildcard)) {
                        ev.bump_capability_denial();
                        return make_primitive_error(ev.string_heap_, ev.error_values_,
                                                    "capability denied: synthesize required",
                                                    ev.primitive_error_counter_ptr());
                    }
                    return fn(a);
                }});
    };

#if AURA_ENABLE_SYNTHESIZE
    // ═══════════════════════════════════════════════════════════════
    // P15: Synthesize Template Strategy (P0)
    // Issue #1974: commercial synthesis vertical (AURA_ENABLE_SYNTHESIZE).
    // ═══════════════════════════════════════════════════════════════

    // (synthesize:register-template name pattern param-names...)
    add("synthesize:register-template", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        auto pat_idx = as_string_idx(a[1]);
        if (name_idx >= ev.string_heap_.size() || pat_idx >= ev.string_heap_.size())
            return make_bool(false);

        std::string name = ev.string_heap_[name_idx];
        std::string pattern = ev.string_heap_[pat_idx];
        std::vector<std::string> params;
        for (std::size_t i = 2; i < a.size(); ++i) {
            if (is_string(a[i])) {
                auto pidx = as_string_idx(a[i]);
                if (pidx < ev.string_heap_.size())
                    params.push_back(ev.string_heap_[pidx]);
            }
        }

        // Replace or append
        bool found = false;
        for (auto& t : g_template_patterns) {
            if (t.first == name) {
                t.second = pattern;
                found = true;
                break;
            }
        }
        if (!found) {
            g_template_patterns.push_back({name, pattern});
            g_template_params.push_back(params);
        } else {
            // Find the params index
            for (std::size_t i = 0; i < g_template_patterns.size(); ++i) {
                if (g_template_patterns[i].first == name) {
                    g_template_params[i] = params;
                    break;
                }
            }
        }
        return make_bool(true);
    });

    // (synthesize:fill template-name arg-values...)
    add("synthesize:fill", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        if (name_idx >= ev.string_heap_.size())
            return make_void();
        std::string name = ev.string_heap_[name_idx];

        // Find template
        int ti = -1;
        for (std::size_t i = 0; i < g_template_patterns.size(); ++i) {
            if (g_template_patterns[i].first == name) {
                ti = static_cast<int>(i);
                break;
            }
        }
        if (ti < 0)
            return make_bool(false);

        // Build substitution map
        std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                           std::equal_to<>>
            subst;
        if (static_cast<std::size_t>(ti) < g_template_params.size()) {
            auto& pnames = g_template_params[ti];
            for (std::size_t i = 0; i < pnames.size() && i + 1 < a.size(); ++i) {
                if (is_string(a[i + 1])) {
                    auto vidx = as_string_idx(a[i + 1]);
                    if (vidx < ev.string_heap_.size())
                        subst[pnames[i]] = ev.string_heap_[vidx];
                }
            }
        }

        // Apply {param} substitutions
        std::string pattern = g_template_patterns[ti].second;
        std::string filled;
        std::size_t pos = 0;
        while (pos < pattern.size()) {
            auto open = pattern.find('{', pos);
            if (open == std::string::npos) {
                filled.append(pattern, pos, std::string::npos);
                break;
            }
            filled.append(pattern, pos, open - pos);
            auto close = pattern.find('}', open);
            if (close == std::string::npos) {
                filled.append(pattern, open);
                break;
            }
            auto pname = pattern.substr(open + 1, close - open - 1);
            auto it = subst.find(pname);
            if (it != subst.end())
                filled.append(it->second);
            else
                filled += "{" + pname + "}";
            pos = close + 1;
        }

        // Apply filled code to workspace via set-code
        auto code_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(filled);
        auto sc_fn = ev.primitives_.lookup("set-code");
        if (!sc_fn)
            return make_void();
        auto result = (*sc_fn)({make_string(code_idx)});
        // Return the source or true
        if (is_bool(result))
            return result;
        return make_bool(true);
    });
#endif // AURA_ENABLE_SYNTHESIZE

    // Issue #561: (synthesize:list-templates) demoted to
    // stdlib (lib/std/synthesize.aura (synthesize:list-templates)
    // wraps the new (query:templates) primitive below).
    // The C++ primitive was a pure read of g_template_patterns
    // — no state mutation, no LLM call — so it's a textbook
    // stdlib candidate per the decision framework.

    // (query:templates) — Issue #561 engine-level accessor.
    // Returns the list of registered synthesize template names.
    // Exposed so std/stats.aura / std/synthesize.aura can
    // enumerate without touching the static g_template_patterns
    // map directly. Always registered (#1974 only gates synthesize:*).
    add("query:templates", [&ev](const auto&) -> EvalValue {
        EvalValue list = make_void();
        for (auto it = g_template_patterns.rbegin(); it != g_template_patterns.rend(); ++it) {
            auto idx = ev.string_heap_.size();
            ev.string_heap_.push_back(it->first);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(idx), list});
            list = make_pair(pid);
        }
        return list;
    });

#if AURA_ENABLE_SYNTHESIZE
    // ═══════════════════════════════════════════════════════════════
    // P16: Synthesize LLM Strategy — synthesize:define
    // Issue #1974: commercial synthesis vertical (AURA_ENABLE_SYNTHESIZE).
    // ═══════════════════════════════════════════════════════════════

    // (synthesize:define name sig [key :val ...])
    //   Generates a function using LLM.
    add("synthesize:define", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        auto sig_idx = as_string_idx(a[1]);
        if (name_idx >= ev.string_heap_.size() || sig_idx >= ev.string_heap_.size())
            return make_void();

        std::string name = ev.string_heap_[name_idx];
        std::string sig = ev.string_heap_[sig_idx];
        std::string prompt_text;
        std::string model = "deepseek-chat";
        std::string examples_text;
        int max_attempts = 5;

        // Parse keyword arguments
        for (std::size_t i = 2; i + 1 < a.size(); i += 2) {
            if (!is_string(a[i]))
                continue;
            auto k_idx = as_string_idx(a[i]);
            if (k_idx >= ev.string_heap_.size())
                continue;
            std::string key = ev.string_heap_[k_idx];

            if (key == ":prompt" && is_string(a[i + 1])) {
                auto pidx = as_string_idx(a[i + 1]);
                if (pidx < ev.string_heap_.size())
                    prompt_text = ev.string_heap_[pidx];
            } else if (key == ":model" && is_string(a[i + 1])) {
                auto midx = as_string_idx(a[i + 1]);
                if (midx < ev.string_heap_.size())
                    model = ev.string_heap_[midx];
            } else if (key == ":examples" && is_string(a[i + 1])) {
                auto eidx = as_string_idx(a[i + 1]);
                if (eidx < ev.string_heap_.size())
                    examples_text = ev.string_heap_[eidx];
            } else if (key == ":max-attempts" && is_int(a[i + 1])) {
                max_attempts = static_cast<int>(as_int(a[i + 1]));
            }
        }

        // Build prompt: construct a simple instruction string
        std::string instruction =
            "Define a function named " + name + " in Aura Lisp with signature: " + sig + ".\n";
        if (!prompt_text.empty())
            instruction += "Task: " + prompt_text + ".\n";
        if (!examples_text.empty())
            instruction += "Examples: " + examples_text + "\n";

        // Get API key
        auto getenv_fn = ev.primitives_.lookup("getenv");
        std::string api_key;
        if (getenv_fn) {
            auto kidx = ev.string_heap_.size();
            ev.string_heap_.push_back("LLM_API_KEY");
            auto kr = (*getenv_fn)({make_string(kidx)});
            if (is_string(kr)) {
                auto ai = as_string_idx(kr);
                if (ai < ev.string_heap_.size())
                    api_key = ev.string_heap_[ai];
            }
        }
        if (api_key.empty())
            return make_string(0);

        // Get http-post primitive
        auto http_fn = ev.primitives_.lookup("http-post");
        if (!http_fn)
            return make_void();

        // Get API URL
        std::string api_url = "https://api.deepseek.com/v1/chat/completions";
        if (getenv_fn) {
            auto uidx = ev.string_heap_.size();
            ev.string_heap_.push_back("LLM_API_URL");
            auto ur = (*getenv_fn)({make_string(uidx)});
            if (is_string(ur)) {
                auto ui = as_string_idx(ur);
                if (ui < ev.string_heap_.size() && !ev.string_heap_[ui].empty())
                    api_url = ev.string_heap_[ui];
            }
        }

        // Auto-fix loop
        std::string last_error;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            // Issue #1236 Phase 1: JSON-escape all interpolated fields
            // (model, instruction, last_error) to prevent payload injection.
            std::string user_content = instruction;
            if (!last_error.empty())
                user_content += " Previous attempt error: " + last_error + ". Fix it.";
            std::string body;
            body += "{\n";
            body += "  \"model\": \"" + json_escape(model) + "\",\n";
            body += "  \"messages\": [\n";
            body += "    {\"role\": \"system\", \"content\": \"You are Aura Lisp. Return ONLY "
                    "valid Aura code. No markdown.\"},\n";
            body +=
                "    {\"role\": \"user\", \"content\": \"" + json_escape(user_content) + "\"}\n";
            body += "  ]\n";
            body += "}\n";

            auto bi = ev.string_heap_.size();
            ev.string_heap_.push_back(body);
            auto ui2 = ev.string_heap_.size();
            ev.string_heap_.push_back(api_url);
            // Issue #1236: do not leave API key permanently on the string heap —
            // push, call, then scrub the slot.
            auto ki = ev.string_heap_.size();
            ev.string_heap_.push_back(api_key);

            auto resp = (*http_fn)({make_string(ui2), make_string(bi), make_string(ki)});
            // Scrub key material from the heap after the call.
            if (ki < ev.string_heap_.size()) {
                auto& slot = ev.string_heap_[ki];
                std::fill(slot.begin(), slot.end(), '\0');
                slot.clear();
            }
            if (!is_string(resp))
                continue;
            auto ri = as_string_idx(resp);
            if (ri >= ev.string_heap_.size())
                continue;
            auto& response = ev.string_heap_[ri];

            // Issue #1715: extract choices[0].message.content via json-parse +
            // hash-ref structure walk (replaces prior substring content scan).
            // Handles pretty-print whitespace, embedded quotes, null content;
            // size-capped. Unicode \uXXXX is decoded by json-parse.
            constexpr std::size_t kMaxSynthCodeBytes = 256 * 1024;
            std::string code;
            auto parse_fn = ev.primitives_.lookup("json-parse");
            auto href_fn = ev.primitives_.lookup("hash-ref");
            if (parse_fn && href_fn) {
                auto root = (*parse_fn)({make_string(ri)});
                auto push_key = [&ev](const char* k) -> EvalValue {
                    auto i = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    return make_string(i);
                };
                if (is_hash(root)) {
                    auto choices = (*href_fn)({root, push_key("choices")});
                    if (is_pair(choices)) {
                        auto cidx = as_pair_idx(choices);
                        if (cidx < ev.pairs_.size()) {
                            auto choice0 = ev.pairs_[cidx].car;
                            if (is_hash(choice0)) {
                                auto message = (*href_fn)({choice0, push_key("message")});
                                if (is_hash(message)) {
                                    auto content = (*href_fn)({message, push_key("content")});
                                    if (is_string(content)) {
                                        auto sidx = as_string_idx(content);
                                        if (sidx < ev.string_heap_.size()) {
                                            code = ev.string_heap_[sidx];
                                            if (code.size() > kMaxSynthCodeBytes)
                                                code.resize(kMaxSynthCodeBytes);
                                        }
                                    }
                                    // null / non-string content → empty (retry)
                                }
                            }
                        }
                    }
                }
            }

            if (code.empty())
                continue;

            // Try set-code
            auto ci = ev.string_heap_.size();
            ev.string_heap_.push_back(code);
            auto sc_fn = ev.primitives_.lookup("set-code");
            if (!sc_fn)
                continue;
            auto sc_r = (*sc_fn)({make_string(ci)});
            if (!is_bool(sc_r) || !as_bool(sc_r)) {
                last_error = "syntax error";
                continue;
            }

            // Try typecheck (Issue #107 part 4: inline, no lock).
            // Going through the typecheck-current primitive would
            // re-enter workspace_mtx_ and deadlock.
            {
                std::string msg = ev.run_typecheck_no_lock();
                if (msg.find("error") != std::string::npos ||
                    msg.find("unbound") != std::string::npos) {
                    last_error = msg;
                    continue;
                }
            }

            // Success. Issue #107 part 4: this site deliberately avoids
            // taking workspace_mtx_ (the typecheck-current primitive would
            // re-enter the lock and deadlock). The manual bump is therefore
            // outside the #1904 migration scope — see Issue #107 part 4.
            ev.defuse_version_.fetch_add(1, std::memory_order_acq_rel); // #1904-allow legacy-lock
            ev.total_mutations_.fetch_add(1, std::memory_order_relaxed);
            auto src_fn = ev.primitives_.lookup("current-source");
            if (src_fn) {
                auto src = (*src_fn)({});
                return src;
            }
            return make_bool(true);
        }

        return make_bool(false);
    });

    // ═══════════════════════════════════════════════════════════════
    // P17: Synthesize Genetic Strategy — synthesize:optimize
    // ═══════════════════════════════════════════════════════════════

    // (synthesize:optimize name [key :val ...])
    //   Uses genetic algorithm to optimize a function.
    //   Keywords: :population, :generations, :mutation-rate,
    //             :fitness or :benchmark (benchmark expression)
    //   Default fitness: runs function with synthetic test inputs
    //   (correctness = 90% weight, code length = 10% tiebreaker).
    //   Creates variants, evaluates fitness, returns best.
    add("synthesize:optimize",
        [&ev, destroy_defuse_index](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_string(a[0]))
                return make_void();
            auto name_idx = as_string_idx(a[0]);
            if (name_idx >= ev.string_heap_.size())
                return make_void();
            std::string fn_name = ev.string_heap_[name_idx];
            int pop_size = 8;
            int generations = 3;
            double mutation_rate = 0.3;
            std::string fitness_expr; // optional user-provided fitness expr

            for (std::size_t i = 1; i + 1 < a.size(); i += 2) {
                if (!is_string(a[i]))
                    continue;
                auto k_idx = as_string_idx(a[i]);
                if (k_idx >= ev.string_heap_.size())
                    continue;
                std::string key = ev.string_heap_[k_idx];
                if (key == ":population" && is_int(a[i + 1]))
                    pop_size = static_cast<int>(as_int(a[i + 1]));
                else if (key == ":generations" && is_int(a[i + 1]))
                    generations = static_cast<int>(as_int(a[i + 1]));
                else if (key == ":mutation-rate" && is_float(a[i + 1]))
                    mutation_rate = as_float(a[i + 1]);
                else if ((key == ":fitness" || key == ":benchmark") && is_string(a[i + 1])) {
                    auto fi = as_string_idx(a[i + 1]);
                    if (fi < ev.string_heap_.size())
                        fitness_expr = ev.string_heap_[fi];
                }
            }
            if (pop_size < 2)
                pop_size = 2;
            if (pop_size > 50)
                pop_size = 50;
            if (generations < 1)
                generations = 1;

            // Get baseline source — use :workspace to read the user's set-code'd
            // script (the function to optimize), NOT the per-eval source (which
            // would be the surrounding synthesize:optimize call itself, causing
            // mutations to recursive-call this primitive → stack overflow).
            // Same root cause as colony-no-fns (commit 6d88544).
            auto src_fn = ev.primitives_.lookup("current-source");
            if (!src_fn)
                return make_void();
            // Intern :workspace keyword (lookup or add to ev.keyword_table_).
            std::uint64_t ws_kw = 0;
            bool ws_found = false;
            for (; ws_kw < ev.keyword_table_.size(); ++ws_kw) {
                if (ev.keyword_table_[ws_kw] == ":workspace") {
                    ws_found = true;
                    break;
                }
            }
            if (!ws_found) {
                ws_kw = ev.keyword_table_.size();
                ev.keyword_table_.push_back(":workspace");
            }
            auto cs_result = (*src_fn)({types::make_keyword(ws_kw)});
            if (!is_string(cs_result))
                return make_void();
            auto cs_idx = as_string_idx(cs_result);
            if (cs_idx >= ev.string_heap_.size())
                return make_void();
            std::string baseline = ev.string_heap_[cs_idx];

            // Fitness: generate synthetic test inputs and eval the function
            // to measure correctness + performance.
            //
            // Strategy:
            // 1. Parse variant source, count function args by scanning for fn_name
            // 2. Generate probe inputs (ints, pairs, etc.) based on arg count
            // 3. Eval each probe: (fn_name arg...), score = fraction of probes that
            //    return a valid value without error
            // 4. Code length is a tiebreaker only — correctness dominates
            //
            // If :fitness keyword is provided, use that expression instead.
            auto compute_fitness = [&](const std::string& src) -> double {
                if (!fitness_expr.empty()) {
                    // User-provided fitness: eval the expression
                    auto sv = ev.string_heap_.size();
                    ev.string_heap_.push_back(src);
                    auto eval_fn = ev.primitives_.lookup("eval");
                    if (eval_fn) {
                        auto r = (*eval_fn)({make_string(sv)});
                        if (is_float(r))
                            return as_float(r);
                        if (is_int(r))
                            return static_cast<double>(as_int(r));
                    }
                    return 0.0;
                }

                // Default fitness: eval the current workspace (which contains
                // the variant code) to bind the function in top_, then probe
                // via the eval primitive.
                //
                // The calling code has already set up the workspace via
                // set-code + typecheck-current before we're called, so
                // eval-current binds the function using workspace flats
                // (safe — no dangling closure pointers).
                //
                // Detect argument count by scanning for (define (fn_name ...))
                int arg_count = 0;
                {
                    auto def_pos = src.find("(define (" + fn_name);
                    if (def_pos != std::string::npos) {
                        auto after_fn = src.find(' ', def_pos);
                        if (after_fn != std::string::npos) {
                            auto param_start = src.find(' ', after_fn + 1);
                            if (param_start != std::string::npos) {
                                auto close_pos = src.find(')', param_start);
                                if (close_pos != std::string::npos) {
                                    auto params =
                                        src.substr(param_start + 1, close_pos - param_start - 1);
                                    if (!params.empty()) {
                                        arg_count = 1;
                                        for (auto c : params) {
                                            if (c == ' ')
                                                ++arg_count;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Eval-current to bind functions in top_ (uses workspace flats, safe)
                auto ec_fn = ev.primitives_.lookup("eval-current");
                if (ec_fn) {
                    (*ec_fn)({});
                }

                // Probe via eval (function is now bound in top_ from workspace eval)
                // Temporary flats in eval are OK for call expressions — they don't
                // create closures, just look up and apply.
                static const std::int64_t probe_ints[] = {0, 1, -1, 2};
                int successes = 0;
                int total_tests = 0;
                auto eval_fn = ev.primitives_.lookup("eval");

                // Issue #1718: reuse one string_heap slot for probe sources
                // across try_probe calls (Option C-style fixed slot, not
                // global index 0). Avoids unbounded push_back growth and
                // realloc churn during GA fitness (gen × pop × probes).
                std::size_t probe_slot = std::numeric_limits<std::size_t>::max();
                auto try_probe = [&](const std::string& call_src) {
                    ++total_tests;
                    if (probe_slot >= ev.string_heap_.size()) {
                        probe_slot = ev.string_heap_.size();
                        ev.string_heap_.push_back(call_src);
                    } else {
                        ev.string_heap_[probe_slot] = call_src;
                    }
                    if (eval_fn) {
                        auto r = (*eval_fn)({make_string(probe_slot)});
                        if (!types::is_error(r))
                            ++successes;
                    }
                };

                if (arg_count <= 0) {
                    try_probe("(" + fn_name + ")");
                } else if (arg_count == 1) {
                    for (auto v : probe_ints) {
                        if (total_tests >= 4)
                            break;
                        try_probe("(" + fn_name + " " + std::to_string(v) + ")");
                    }
                } else if (arg_count == 2) {
                    for (int i = 0; i < 4 && i + 1 < 4; ++i) {
                        try_probe("(" + fn_name + " " + std::to_string(probe_ints[i]) + " " +
                                  std::to_string(probe_ints[i + 1]) + ")");
                    }
                } else {
                    std::string call_src = "(" + fn_name;
                    for (int i = 0; i < arg_count; ++i)
                        call_src += " 0";
                    call_src += ")";
                    try_probe(call_src);
                }

                // Score: correctness dominates (up to 1000), then small length bonus
                double correctness = total_tests > 0 ? (1000.0 * static_cast<double>(successes) /
                                                        static_cast<double>(total_tests))
                                                     : 0.0;
                double length_bonus = 1.0 / static_cast<double>(src.size() + 1);
                return correctness + length_bonus;
            };

            std::string best_code = baseline;
            double best_fitness = compute_fitness(baseline);
            int best_gen = 0;

            // Store multiple candidates for crossover (elitism)
            std::vector<std::pair<std::string, double>> elite;

            for (int gen = 0; gen < generations; ++gen) {
                for (int p = 0; p < pop_size; ++p) {
                    std::string variant = best_code;

                    // Apply mutations (Issue #1716: agent_prng, not std::rand)
                    for (int m = 0; m < 5; ++m) {
                        if (agent_rand_unit() >= mutation_rate)
                            continue;
                        // Operator swap
                        for (const char* op = "+-*/"; *op; ++op) {
                            auto opos = variant.find(*op);
                            if (opos != std::string::npos && opos > 0) {
                                variant[opos] = "+-*/"[agent_rand_below(4)];
                                break;
                            }
                        }
                        // Numeric mutation
                        auto npos = variant.find_first_of("0123456789");
                        if (npos == std::string::npos)
                            break;
                        auto nend = variant.find_first_not_of("0123456789", npos);
                        if (nend == std::string::npos)
                            nend = variant.size();
                        std::string old_n = variant.substr(npos, nend - npos);
                        if (old_n.empty())
                            continue;
                        int val = std::stoi(old_n);
                        val += static_cast<int>(agent_rand_below(21)) - 10;
                        if (val < 0)
                            val = 0;
                        variant.replace(npos, nend - npos, std::to_string(val));
                    }

                    // Crossover: text-level or AST-level
                    if (!elite.empty() && agent_rand_below(3) == 0) {
                        auto& other =
                            elite[agent_rand_below(static_cast<unsigned>(elite.size()))].first;
                        // Try AST expression-level crossover via node swapping
                        // Use a child workspace and mutate:replace-value
                        if (ev.workspace_tree_ && agent_rand_below(2) == 0) {
                            auto* tree =
                                static_cast<aura::compiler::WorkspaceTree*>(ev.workspace_tree_);
                            // Issue #1717: RAII child workspace for AST crossover.
                            WorkspaceSwapGuard guard(ev, *tree, "xover");
                            if (guard.valid()) {
                                // Set variant as current code
                                auto vi = ev.string_heap_.size();
                                ev.string_heap_.push_back(variant);
                                auto sc_fn = ev.primitives_.lookup("set-code");
                                if (sc_fn) {
                                    auto sr = (*sc_fn)({make_string(vi)});
                                    if (is_bool(sr) && as_bool(sr)) {
                                        // Find LiteralInt nodes and swap value with other variant
                                        auto* flat = ev.workspace_flat();
                                        for (aura::ast::NodeId nid = 0;
                                             nid < (flat ? flat->size() : 0); ++nid) {
                                            if (agent_rand_below(5) != 0)
                                                continue; // 20% chance per node
                                            auto v = flat->get(nid);
                                            if (v.tag == aura::ast::NodeTag::LiteralInt) {
                                                // Extract a random int from "other"
                                                auto nums = other;
                                                auto npos = nums.find_first_of("0123456789");
                                                if (npos != std::string::npos) {
                                                    auto nend =
                                                        nums.find_first_not_of("0123456789", npos);
                                                    if (nend == std::string::npos)
                                                        nend = nums.size();
                                                    int new_val =
                                                        std::stoi(nums.substr(npos, nend - npos));
                                                    if (new_val >= 0) {
                                                        auto rv_fn = ev.primitives_.lookup(
                                                            "mutate:replace-value");
                                                        if (rv_fn) {
                                                            (*rv_fn)({make_int(nid),
                                                                      make_int(new_val),
                                                                      make_string(vi)});
                                                        }
                                                    }
                                                }
                                                break; // Mutate one node
                                            }
                                        }

                                        // Typecheck after crossover (Issue #107 part 4: inline)
                                        if (true) {
                                            auto tc_r = ev.run_typecheck_no_lock_bool();
                                            if (tc_r) {
                                                // Successful crossover: get the new source
                                                // (use :workspace to read user's set-code'd script,
                                                // not the per-eval source = the surrounding call)
                                                auto src_fn =
                                                    ev.primitives_.lookup("current-source");
                                                if (src_fn) {
                                                    std::uint64_t ws_kw = 0;
                                                    bool ws_found = false;
                                                    for (; ws_kw < ev.keyword_table_.size();
                                                         ++ws_kw) {
                                                        if (ev.keyword_table_[ws_kw] ==
                                                            ":workspace") {
                                                            ws_found = true;
                                                            break;
                                                        }
                                                    }
                                                    if (!ws_found) {
                                                        ws_kw = ev.keyword_table_.size();
                                                        ev.keyword_table_.push_back(":workspace");
                                                    }
                                                    auto src =
                                                        (*src_fn)({types::make_keyword(ws_kw)});
                                                    if (is_string(src)) {
                                                        auto si = as_string_idx(src);
                                                        if (si < ev.string_heap_.size())
                                                            variant = ev.string_heap_[si];
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            } // guard dtor: restore + delete_child
                        } else {
                            // Text-level crossover (fallback)
                            auto b1 = variant.find("(lambda");
                            auto b2 = other.find("(lambda");
                            if (b1 != std::string::npos && b2 != std::string::npos) {
                                auto e1 = variant.find(')', b1);
                                auto e2 = other.find(')', b2);
                                if (e1 != std::string::npos && e2 != std::string::npos && e1 > b1 &&
                                    e2 > b2) {
                                    auto body1_end = variant.find_last_of(')');
                                    auto body2_end = other.find_last_of(')');
                                    if (body1_end > b1 && body2_end > b2) {
                                        std::string body1 = variant.substr(b1, body1_end - b1 + 1);
                                        std::string body2 = other.substr(b2, body2_end - b2 + 1);
                                        if (body1 != body2) {
                                            variant = variant.substr(0, b1) + body2 +
                                                      variant.substr(body1_end + 1);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (variant == best_code)
                        continue;

                    // Evaluate in a child workspace (isolation)
                    // Use workspace tree if available, otherwise fall back to set-code
                    bool evaluated = false;
                    double f = 0.0;
                    auto sc_fn = ev.primitives_.lookup("set-code");
                    if (!sc_fn)
                        continue;

                    if (ev.workspace_tree_) {
                        // Issue #1717: RAII child workspace for isolated evaluate.
                        auto* tree =
                            static_cast<aura::compiler::WorkspaceTree*>(ev.workspace_tree_);
                        WorkspaceSwapGuard guard(ev, *tree, "evolve-variant");
                        if (guard.valid()) {
                            auto vi = ev.string_heap_.size();
                            ev.string_heap_.push_back(variant);
                            auto sc_r = (*sc_fn)({make_string(vi)});

                            if (is_bool(sc_r) && as_bool(sc_r)) {
                                // Issue #107 part 4: inline typecheck (no lock).
                                // Going through the primitive would re-enter
                                // workspace_mtx_ and deadlock.
                                bool valid = ev.run_typecheck_no_lock_bool();
                                if (valid) {
                                    f = compute_fitness(variant);
                                    evaluated = true;
                                }
                            }
                        } // guard dtor: restore + delete_child
                    }

                    if (!evaluated) {
                        // Fallback: direct set-code
                        auto vi = ev.string_heap_.size();
                        ev.string_heap_.push_back(variant);
                        auto sc_r = (*sc_fn)({make_string(vi)});
                        if (!is_bool(sc_r) || !as_bool(sc_r))
                            continue;

                        // Issue #107 part 4: inline typecheck (no lock).
                        // Going through the primitive would re-enter
                        // workspace_mtx_ and deadlock.
                        bool valid = ev.run_typecheck_no_lock_bool();
                        if (!valid)
                            continue;
                        f = compute_fitness(variant);
                        // Restore
                        auto bi = ev.string_heap_.size();
                        ev.string_heap_.push_back(baseline);
                        (*sc_fn)({make_string(bi)});
                    }

                    if (f > best_fitness) {
                        best_fitness = f;
                        best_code = variant;
                        best_gen = gen + 1;
                    }
                }

                // Update elite from this generation
                elite.clear();
                elite.push_back({best_code, best_fitness});
            }

            // Apply best to workspace
            auto bi = ev.string_heap_.size();
            ev.string_heap_.push_back(best_code);
            auto sc_fn = ev.primitives_.lookup("set-code");
            if (sc_fn)
                (*sc_fn)({make_string(bi)});
            // Issue #1904: removed redundant bump — synthesize:optimize
            // path doesn't currently use MutationBoundaryGuard; the
            // mutation scope needs its own Guard wrap. Marked TODO.
            // (ASAN fix #107 leak) delete the old index.
            destroy_defuse_index();

            auto gs = std::to_string(best_gen);
            auto gi = ev.string_heap_.size();
            ev.string_heap_.push_back(gs);
            auto fs = std::to_string(best_fitness);
            auto fi = ev.string_heap_.size();
            ev.string_heap_.push_back(fs);

            auto p1 = ev.pairs_.size();
            ev.pairs_.push_back({make_string(gi), make_string(fi)});
            return make_pair(p1);
        });
#else
    (void)destroy_defuse_index;
#endif // AURA_ENABLE_SYNTHESIZE
}

void register_strategy_primitives(PrimRegistrar add_raw, Evaluator& ev) {
    // Issue #1232 Phase 1: gate intend / strategy primitives with kCapStrategy.
    auto add = [&ev, add_raw = std::move(add_raw)](std::string name, PrimFn fn) {
        add_raw(std::move(name),
                PrimFn{[&ev, fn = std::move(fn)](std::span<const EvalValue> a) -> EvalValue {
                    if (ev.sandbox_mode() &&
                        !ev.has_capability(aura::compiler::security::kCapStrategy) &&
                        !ev.has_capability(aura::compiler::security::kCapWildcard)) {
                        ev.bump_capability_denial();
                        return make_primitive_error(ev.string_heap_, ev.error_values_,
                                                    "capability denied: strategy required",
                                                    ev.primitive_error_counter_ptr());
                    }
                    return fn(a);
                }});
    };

    // ── intend — 纯循环管理器 — 纯循环管理器 ────────────────────────────────
    // (intend goal generator-fn verifier-fn [fixer-fn] [max-attempts])
    //
    // 不管理 LLM 调用、不构建 prompt、不做 JSON 解析。
    // 只做循环编排。LLM 交互通过传入的 Aura 函数完成。
    //
    // - generator-fn: (lambda (goal) → code-string)
    // - verifier-fn:  (lambda (code) → "#t" for pass, else error-string)
    // - fixer-fn:     (lambda (code error goal) → new-code-string, optional)
    // - max-attempts: int (optional, default 3)
    add("intend", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Mark task context so closure bodies are allocated in temp_arena_.
        // Issue #68: depth counter (was bool) for nested intend support.
        int saved_depth = ev.in_task_context_;
        ev.in_task_context_ = saved_depth + 1;
        auto restore = [&] { ev.in_task_context_ = saved_depth; };

        if (a.size() < 3) {
            restore();
            return make_void();
        }
        if (!types::is_string(a[0]) || !types::is_closure(a[1]) || !types::is_closure(a[2])) {
            restore();
            return make_void();
        }

        auto goal = heap_str_from(ev.string_heap_, a[0]);
        auto gen_cid = types::as_closure_id(a[1]); // generator
        auto ver_cid = types::as_closure_id(a[2]); // verifier

        // Issue #63 Phase 3: optional strategy-name (string) as the
        // 3rd-or-4th positional arg. When present, use the strategy's
        // `max_attempts` (overriding the 4th-or-5th positional int).
        // Format: (intend goal gen ver [fixer] [max-attempts]
        //                 :strategy name)  ← keyword form preferred
        std::string strategy_name = "default";
        int max_attempts = 3;
        bool has_fixer = false;
        std::uint64_t fix_cid = 0;
        // Walk remaining positional args; support both (fixer [max])
        // and (fixer max :strategy name) orderings by looking for the
        // :strategy keyword.
        std::size_t i = 3;
        if (i < a.size() && types::is_closure(a[i])) {
            has_fixer = true;
            fix_cid = types::as_closure_id(a[i]);
            ++i;
        }
        if (i < a.size() && types::is_int(a[i])) {
            max_attempts = static_cast<int>(types::as_int(a[i]));
            ++i;
        }
        for (; i + 1 < a.size(); i += 2) {
            std::string k;
            if (types::is_string(a[i])) {
                k = heap_str_from(ev.string_heap_, a[i]);
            } else if (types::is_keyword(a[i])) {
                auto kidx = types::as_keyword_idx(a[i]);
                if (kidx < ev.keyword_table_.size())
                    k = ev.keyword_table_[kidx];
            } else
                continue;
            if (k == ":strategy" && types::is_string(a[i + 1])) {
                strategy_name = heap_str_from(ev.string_heap_, a[i + 1]);
                // Look up the strategy's max_attempts (overrides int arg)
                std::shared_lock<std::shared_mutex> lk(ev.strategies_mtx_);
                for (auto& s : ev.strategies_) {
                    if (s.name == strategy_name) {
                        max_attempts = s.max_attempts;
                        break;
                    }
                }
            }
        }
        if ((has_fixer && a.size() >= 5 && types::is_int(a[4])) ||
            (!has_fixer && a.size() >= 4 && types::is_int(a[3]))) {
            auto idx = has_fixer ? 4 : 3;
            max_attempts = static_cast<int>(types::as_int(a[idx]));
        }

        auto t0 = std::chrono::steady_clock::now();
        ev.timeline_clear();
        ev.timeline_push("start:" + goal);

        std::string current_code_str;
        std::string last_error;
        std::vector<std::string> errors;
        std::vector<std::string> error_types;
        std::vector<std::string> generated_codes;
        std::uint64_t llm_call_count = 0;

        auto classify_error = [](const std::string& err) -> std::string {
            if (err.find("unbound variable") != std::string::npos)
                return "unbound-variable";
            if (err.find("type mismatch") != std::string::npos)
                return "type-mismatch";
            if (err.find("division by zero") != std::string::npos)
                return "div-zero";
            if (err.find("syntax") != std::string::npos ||
                err.find("unbalanced") != std::string::npos)
                return "syntax-error";
            if (err.find("timeout") != std::string::npos)
                return "timeout";
            if (err.find("recursion") != std::string::npos ||
                err.find("stack") != std::string::npos)
                return "recursion-limit";
            if (err.find("bad-code") != std::string::npos)
                return "bad-code";
            if (err.find("verification failed") != std::string::npos)
                return "verification-failed";
            return "other";
        };

        // Call a closure, return string result.
        // Issue #1719: refuse apply_closure on freed ClosureIds (UAF sibling of #1713).
        auto call_fn = [&](std::uint64_t cid,
                           std::initializer_list<types::EvalValue> args) -> std::string {
            if (!agent_cid_live(ev, cid)) {
                agent_note_closure_freed_call(ev);
                ev.timeline_push("attempt:closure-freed:" + std::to_string(cid));
                return {};
            }
            auto opt = ev.apply_closure(cid, args);
            if (!opt)
                return {};
            auto& val = *opt;
            if (types::is_string(val))
                return heap_str_from(ev.string_heap_, val);
            if (types::is_void(val))
                return {};
            if (types::is_int(val))
                return std::to_string(types::as_int(val));
            if (types::is_bool(val))
                return types::as_bool(val) ? "#t" : "#f";
            return {};
        };

        // Issue #1721: reuse fixed string_heap slots for intermediate
        // gen/ver/fix args (goal / code / error). Avoids O(attempts)
        // unbounded push pollution; safer than end-of-loop pop_back under
        // concurrent fibers that also use string_heap_ (Option C refined).
        std::size_t slot_goal = std::numeric_limits<std::size_t>::max();
        std::size_t slot_code = std::numeric_limits<std::size_t>::max();
        std::size_t slot_err = std::numeric_limits<std::size_t>::max();
        auto put_slot = [&](std::size_t& slot, const std::string& s) -> types::EvalValue {
            if (slot >= ev.string_heap_.size()) {
                slot = ev.string_heap_.size();
                ev.string_heap_.push_back(s);
            } else {
                ev.string_heap_[slot] = s;
            }
            return types::make_string(slot);
        };
        auto finish_result = [&](std::string result_str) -> EvalValue {
            // Only the final status string is retained on the heap.
            auto rs = ev.string_heap_.size();
            ev.string_heap_.push_back(std::move(result_str));
            restore();
            return types::make_string(rs);
        };

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            // Issue #1205 Phase 1: GC safepoint on each intend attempt.
            if (auto* fn = aura::gc_hooks::g_arena_safepoint_check.load(std::memory_order_relaxed))
                fn();
            std::string code_str;
            if (attempt == 1 || current_code_str.empty()) {
                code_str = call_fn(gen_cid, {put_slot(slot_goal, goal)});
                llm_call_count++;
                if (code_str.empty()) {
                    ev.timeline_push("attempt_" + std::to_string(attempt) +
                                     ":empty from generator");
                    errors.push_back("empty from generator");
                    error_types.push_back("empty");
                    continue;
                }
            } else {
                if (!has_fixer) {
                    ev.timeline_push("attempt_" + std::to_string(attempt) + ":no fixer, stopping");
                    break;
                }
                code_str =
                    call_fn(fix_cid, {put_slot(slot_code, current_code_str),
                                      put_slot(slot_err, last_error), put_slot(slot_goal, goal)});
                llm_call_count++;
                if (code_str.empty()) {
                    ev.timeline_push("attempt_" + std::to_string(attempt) + ":empty from fixer");
                    errors.push_back("empty from fixer");
                    error_types.push_back("empty");
                    continue;
                }
            }
            generated_codes.push_back(code_str);

            auto ver = call_fn(ver_cid, {put_slot(slot_code, code_str)});

            if (ver.find("#t") == 0) {
                current_code_str = code_str;
                ev.timeline_push("attempt_" + std::to_string(attempt) + ":success");
                auto result = "#(status:\"ok\" goal:\"" + goal +
                              "\" iterations:" + std::to_string(attempt) + ")";

                auto t1 = std::chrono::steady_clock::now();
                auto duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                Evaluator::IntendRecord rec;
                rec.strategy_name = strategy_name;
                rec.task_desc = goal;
                rec.success = true;
                rec.attempts = attempt;
                rec.errors = errors;
                rec.error_types = error_types;
                rec.generated_codes = generated_codes;
                rec.llm_call_count = llm_call_count;
                rec.llm_tokens = 0;
                rec.duration_ms = static_cast<std::uint64_t>(duration);
                rec.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
                rec.parent_record_id = 0;
                ev.intend_history_push(std::move(rec));
                return finish_result(std::move(result));
            }

            current_code_str = code_str;
            last_error = ver.empty() ? "verification failed" : ver;
            errors.push_back(last_error);
            error_types.push_back(classify_error(last_error));
            ev.timeline_push("attempt_" + std::to_string(attempt) + ":" + last_error);
        }

        auto t1 = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        Evaluator::IntendRecord rec;
        rec.strategy_name = strategy_name;
        rec.task_desc = goal;
        rec.success = false;
        rec.attempts = max_attempts;
        rec.errors = errors;
        rec.error_types = error_types;
        rec.generated_codes = generated_codes;
        rec.llm_call_count = llm_call_count;
        rec.llm_tokens = 0;
        rec.duration_ms = static_cast<std::uint64_t>(duration);
        rec.timestamp = static_cast<std::uint64_t>(std::time(nullptr));
        rec.parent_record_id = 0;
        ev.intend_history_push(std::move(rec));

        auto result = "#(status:\"failed\" goal:\"" + goal +
                      "\" iterations:" + std::to_string(max_attempts) + " last-error:\"" +
                      last_error + "\")";
        ev.timeline_push("failed:" + last_error);
        return finish_result(std::move(result));
    });
    // ── intend-history — 查询意图执行时间线 ────────────────────
    add("intend-history", [&ev](const auto&) -> EvalValue {
        // Issue #1720: shared lock while snapshotting timeline.
        std::string result = ev.timeline_snapshot();
        if (result.empty())
            result = "(empty)";
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(result);
        return types::make_string(sidx);
    });

    // ── workspace-state — 汇总当前 workspace 的定义 + 近期 mutations ───
    // Issue #63 Phase 2: a single primitive that gives the LLM
    // (or the EDSL) a snapshot of the workspace. The output is a
    // human-readable string: lines starting with "DEFINE: " list
    // top-level definitions; lines starting with "MUTATION: " list
    // the most recent timeline entries (capped at last 10). The
    // first line is a summary header "WORKSPACE: <n> defines" so
    // the LLM has a one-token extractable count.
    add("workspace-state", [&ev](const auto&) -> EvalValue {
        // Issue #1994 / Wave1 B-09: pin (shared or adopt Guard exclusive).
        auto pin = ev.pin_workspace_flat();
        if (!pin || !pin.pool()) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::string("WORKSPACE-ERROR: no workspace AST loaded"));
            return types::make_string(sidx);
        }
        auto& flat = *pin;
        auto& pool = *pin.pool();
        std::string out;
        // Top-level defines: walk the flat for Define nodes and
        // collect their bound name.
        std::size_t define_count = 0;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM) {
                out += "DEFINE: " + std::string(pool.resolve(v.sym_id)) + "\n";
                ++define_count;
            }
        }
        if (define_count == 0)
            out += "(no defines)\n";
        // Recent mutations: last 10 timeline entries (Issue #1720 locked)
        out += "MUTATIONS (last 10):\n";
        {
            auto tail = ev.timeline_tail(10);
            out += (tail == "  (empty)\n") ? "  (none)\n" : tail;
        }
        // Prepend a one-line summary header so the LLM has an
        // easy parse target: "WORKSPACE: <n> defines".
        out = "WORKSPACE: " + std::to_string(define_count) + " defines\n" + out;
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return types::make_string(sidx);
    });

    // ── intend-analytics — 聚合 intend 历史数据 ────────────────
    add("intend-analytics", [&ev](std::span<const EvalValue> a) -> EvalValue {
        std::string filter_strategy;
        std::string filter_field;
        std::string filter_value;
        std::size_t arg_idx = 0;

        if (a.size() > arg_idx && types::is_string(a[arg_idx])) {
            filter_strategy = heap_str_from(ev.string_heap_, a[arg_idx]);
            arg_idx++;
        }
        if (a.size() > arg_idx + 2 && types::is_string(a[arg_idx]) &&
            heap_str_from(ev.string_heap_, a[arg_idx]) == ":filter") {
            if (types::is_string(a[arg_idx + 1]))
                filter_field = heap_str_from(ev.string_heap_, a[arg_idx + 1]);
            if (types::is_string(a[arg_idx + 2]))
                filter_value = heap_str_from(ev.string_heap_, a[arg_idx + 2]);
        }

        std::uint64_t total = 0, successes = 0, total_attempts = 0;
        std::uint64_t total_llm_calls = 0, total_duration = 0;
        std::map<std::string, std::uint64_t> error_type_counts;
        std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> task_stats;

        {
            // Issue #1720: snapshot under shared lock then aggregate offline.
            std::shared_lock<std::shared_mutex> lk(ev.intend_history_mtx_);
            for (auto& rec : ev.intend_history_) {
                if (!filter_strategy.empty() && rec.strategy_name != filter_strategy)
                    continue;
                if (!filter_field.empty()) {
                    if (filter_field == "error-type") {
                        bool matches = false;
                        for (auto& et : rec.error_types) {
                            if (et.find(filter_value) != std::string::npos) {
                                matches = true;
                                break;
                            }
                        }
                        if (!matches)
                            continue;
                    } else
                        continue;
                }
                total++;
                if (rec.success)
                    successes++;
                total_attempts += rec.attempts;
                total_llm_calls += rec.llm_call_count;
                total_duration += rec.duration_ms;
                for (auto& et : rec.error_types)
                    error_type_counts[et]++;
                auto key = rec.task_desc.substr(
                    0, std::min<std::size_t>(rec.task_desc.size(), std::size_t{60}));
                auto& ts = task_stats[key];
                ts.second++;
                if (rec.success)
                    ts.first++;
            }
        }

        std::string result = "#(analytics";
        result += " total-runs:" + std::to_string(total);
        result += " success-rate:";
        if (total > 0)
            result += std::to_string(static_cast<double>(successes) / static_cast<double>(total));
        else
            result += "0";
        result += " avg-attempts:";
        if (total > 0)
            result +=
                std::to_string(static_cast<double>(total_attempts) / static_cast<double>(total));
        else
            result += "0";
        result += " total-llm-calls:" + std::to_string(total_llm_calls);
        result += " avg-duration-ms:";
        if (total > 0)
            result += std::to_string(total_duration / total);
        else
            result += "0";
        result += " top-errors:(";
        for (auto& [et, count] : error_type_counts) {
            result += " " + et + ":" + std::to_string(count);
        }
        result += ")";
        result += " by-task:(";
        for (auto& [task, stats] : task_stats) {
            result += " (" + task + " " + std::to_string(stats.first) + "/" +
                      std::to_string(stats.second) + ")";
        }
        result += "))";

        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(result);
        return types::make_string(sidx);
    });


    // ── define-strategy — 定义策略 ──────────────────────────
    // Issue #63 Phase 3: accept optional keyword args
    //   :max-attempts <int>     (1..20, default 3)
    //   :temperature <float>   (0.0..1.0, default 0.3)
    //   :sys-prompt-template <str>
    add("define-strategy", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_bool(false);
        auto name = heap_str_from(ev.string_heap_, a[0]);
        auto body = heap_str_from(ev.string_heap_, a[1]);
        // Optional keyword args (pairs from index 2)
        int new_max = 3;
        double new_temp = 0.3;
        std::string new_spt;
        for (std::size_t i = 2; i + 1 < a.size(); i += 2) {
            // Issue #63 Phase 3: keywords come in as a separate EvalValue
            // type, not strings. Resolve both.
            std::string k;
            if (types::is_string(a[i])) {
                k = heap_str_from(ev.string_heap_, a[i]);
            } else if (types::is_keyword(a[i])) {
                auto kidx = types::as_keyword_idx(a[i]);
                if (kidx < ev.keyword_table_.size())
                    k = ev.keyword_table_[kidx];
            } else {
                continue;
            }
            if (k == ":max-attempts" && types::is_int(a[i + 1])) {
                int v = static_cast<int>(types::as_int(a[i + 1]));
                if (v >= 1 && v <= 20)
                    new_max = v;
            } else if (k == ":temperature" &&
                       (types::is_int(a[i + 1]) || types::is_float(a[i + 1]))) {
                double v = types::is_float(a[i + 1]) ? types::as_float(a[i + 1])
                                                     : static_cast<double>(types::as_int(a[i + 1]));
                if (v >= 0.0 && v <= 1.0)
                    new_temp = v;
            } else if (k == ":sys-prompt-template" && types::is_string(a[i + 1])) {
                new_spt = heap_str_from(ev.string_heap_, a[i + 1]);
            }
        }
        {
            // Issue #1720 / #1722: unique lock for strategies_ mutate.
            std::unique_lock<std::shared_mutex> lk(ev.strategies_mtx_);
            for (auto& s : ev.strategies_) {
                if (s.name == name) {
                    s.body = body;
                    s.max_attempts = new_max;
                    s.temperature = new_temp;
                    s.sys_prompt_template = new_spt;
                    return make_bool(true);
                }
            }
            ev.strategies_.push_back({name, body, new_max, new_temp, new_spt, 0, ""});
        }
        return make_bool(true);
    });
    // ── register-strategy! — 注册/更新策略 ──────────────
    add("register-strategy!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_bool(false);
        auto name = heap_str_from(ev.string_heap_, a[0]);
        auto body = heap_str_from(ev.string_heap_, a[1]);
        {
            std::unique_lock<std::shared_mutex> lk(ev.strategies_mtx_); // #1720/#1722
            for (auto& s : ev.strategies_) {
                if (s.name == name) {
                    s.body = body;
                    return make_bool(true);
                }
            }
            ev.strategies_.push_back({name, body, 3, 0.3, "", 0, ""});
        }
        return make_bool(true);
    });
    // ── strategy-field — 读取策略字段 ──────────────────────
    // Issue #63 Phase 3: support tunable fields beyond 'body'.
    add("strategy-field", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();
        auto name = heap_str_from(ev.string_heap_, a[0]);
        auto field = heap_str_from(ev.string_heap_, a[1]);
        std::shared_lock<std::shared_mutex> lk(ev.strategies_mtx_); // #1720/#1722
        for (auto& s : ev.strategies_) {
            if (s.name != name)
                continue;
            if (field == "body") {
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(s.body);
                return types::make_string(sid);
            }
            if (field == "max-attempts") {
                return types::make_int(s.max_attempts);
            }
            if (field == "temperature") {
                return types::make_float(s.temperature);
            }
            if (field == "sys-prompt-template") {
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(s.sys_prompt_template);
                return types::make_string(sid);
            }
            if (field == "evolution") {
                return types::make_int(s.evolution);
            }
            if (field == "parent") {
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(s.parent);
                return types::make_string(sid);
            }
        }
        return make_void();
    });
    // ── strategy-set-field! — 修改策略字段（白名单）───────────
    // Whitelist + range checks per e4_evolvable_strategies.md §1.
    add("strategy-set-field!", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 3 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_bool(false);
        auto field = heap_str_from(ev.string_heap_, a[1]);
        auto name = heap_str_from(ev.string_heap_, a[0]);
        std::unique_lock<std::shared_mutex> lk(ev.strategies_mtx_); // #1720/#1722
        for (auto& s : ev.strategies_) {
            if (s.name != name)
                continue;
            if (field == "body" && types::is_string(a[2])) {
                s.body = heap_str_from(ev.string_heap_, a[2]);
                return make_bool(true);
            }
            if (field == "max-attempts" && types::is_int(a[2])) {
                int v = static_cast<int>(types::as_int(a[2]));
                if (v < 1 || v > 20)
                    return make_bool(false);
                s.max_attempts = v;
                return make_bool(true);
            }
            if (field == "temperature" && (types::is_int(a[2]) || types::is_float(a[2]))) {
                double v = types::is_float(a[2]) ? types::as_float(a[2])
                                                 : static_cast<double>(types::as_int(a[2]));
                if (v < 0.0 || v > 1.0)
                    return make_bool(false);
                s.temperature = v;
                return make_bool(true);
            }
            if (field == "sys-prompt-template" && types::is_string(a[2])) {
                s.sys_prompt_template = heap_str_from(ev.string_heap_, a[2]);
                return make_bool(true);
            }
            // name / evolution / parent are read-only
            return make_bool(false);
        }
        return make_bool(false);
    });
    // ── strategy-inspect — 一键检视 ────────────────────────
    // Issue #63 Phase 3: show all tunable fields + read-only metadata.
    add("strategy-inspect", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto name = heap_str_from(ev.string_heap_, a[0]);
        std::shared_lock<std::shared_mutex> lk(ev.strategies_mtx_); // #1720/#1722
        for (auto& s : ev.strategies_) {
            if (s.name == name) {
                std::string result = "#(strategy-inspect";
                result += " name:\"" + s.name + "\"";
                result += " body:\"" + s.body + "\"";
                result += " max-attempts:" + std::to_string(s.max_attempts);
                result += " temperature:" + std::to_string(s.temperature);
                result += " evolution:" + std::to_string(s.evolution);
                result += " parent:\"" + s.parent + "\"";
                result += ")";
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(result);
                return types::make_string(sid);
            }
        }
        return make_void();
    });

    // ── evolve-strategy — 基于 intend-analytics 自动调优策略 ────
    // Issue #63 Phase 3. Was registered in type_checker_impl.cpp but
    // never implemented as a runtime primitive. Implements the
    // heuristics from e4_evolvable_strategies.md §3:
    //
    //   - success-rate < 50% AND avg-attempts = max-attempts
    //       → ↑ max-attempts (add 2, capped at 20)
    //   - success-rate > 90% AND avg-attempts < 1.5
    //       → ↓ max-attempts (sub 1, floored at 1)
    //   - top-error is "unbound-variable" (≥ 2 occurrences)
    //       → sys-prompt-template += "Do NOT use undefined variables.\n"
    //   - top-error is "type-mismatch" (≥ 2 occurrences)
    //       → sys-prompt-template += "Use (check x : Type) before ops.\n"
    //   - top-error is "syntax-error" (≥ 2 occurrences)
    //       → sys-prompt-template += "Check parentheses carefully.\n"
    //
    // Returns the new strategy name (e.g. "adaptive-v2"). The original
    // strategy is left untouched (E4 §3 "保留旧版本 + 探索/利用平衡").
    //
    // Args:
    //   (evolve-strategy <strategy-name>)
    //   (evolve-strategy <strategy-name> <analytics-string>)
    add("evolve-strategy", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto name = heap_str_from(ev.string_heap_, a[0]);

        // Find the source strategy (copy under lock — #1720/#1722)
        Evaluator::StrategyDef src_copy;
        bool found_src = false;
        {
            std::shared_lock<std::shared_mutex> lk(ev.strategies_mtx_);
            for (auto& s : ev.strategies_) {
                if (s.name == name) {
                    src_copy = s;
                    found_src = true;
                    break;
                }
            }
        }
        if (!found_src)
            return make_void();
        const Evaluator::StrategyDef* src = &src_copy;

        // Get analytics: either passed as 2nd arg, or call
        // intend-analytics ourselves on this strategy.
        std::string analytics;
        if (a.size() >= 2 && types::is_string(a[1])) {
            analytics = heap_str_from(ev.string_heap_, a[1]);
        } else {
            auto prim = ev.primitives_.lookup("intend-analytics");
            if (prim) {
                auto sid = ev.string_heap_.size();
                ev.string_heap_.push_back(name);
                auto res = (*prim)({types::make_string(sid)});
                if (types::is_string(res))
                    analytics = heap_str_from(ev.string_heap_, res);
            }
        }

        // Parse key fields out of the analytics s-expression.
        // Format: #(analytics total-runs:N success-rate:X avg-attempts:Y
        //          total-llm-calls:N avg-duration-ms:N top-errors:( k:n ...)
        //          by-task:( ... ))
        // Issue #1723: track paren depth so nested values like
        // top-errors:( a:1 (nested) ) are not truncated at the first ')'.
        auto find_after = [](const std::string& s, const std::string& key) -> std::string {
            auto p = s.find(key);
            if (p == std::string::npos)
                return {};
            p += key.size();
            // skip ':' and spaces
            while (p < s.size() && (s[p] == ':' || s[p] == ' '))
                ++p;
            auto start = p;
            int depth = 0;
            while (p < s.size()) {
                if (s[p] == '(') {
                    ++depth;
                } else if (s[p] == ')') {
                    if (depth == 0)
                        break; // end of value at outer ')'
                    --depth;
                } else if (s[p] == ' ' && depth == 0) {
                    break; // end of atom at top-level space
                }
                ++p;
            }
            return s.substr(start, p - start);
        };
        double success_rate = 0.0;
        double avg_attempts = 0.0;
        // Issue #1724: narrow catch + metric (was silent catch(...)).
        // Keep defaults on malformed analytics — evolve is best-effort.
        try {
            auto sr = find_after(analytics, "success-rate");
            if (!sr.empty())
                success_rate = std::stod(sr);
            auto aa = find_after(analytics, "avg-attempts");
            if (!aa.empty())
                avg_attempts = std::stod(aa);
        } catch (const std::exception& e) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->agent_evolve_analytics_parse_failures.fetch_add(1, std::memory_order_relaxed);
#ifdef AURA_DEBUG_INTEND
            std::fprintf(stderr, "evolve-strategy analytics parse failure: %s\n", e.what());
#else
            (void)e;
#endif
        }

        // Parse top-errors: walk "(k1:n1 k2:n2 ...)" inside top-errors:(...)
        // Issue #1723: match the closing ')' by paren depth, not first ')'.
        std::map<std::string, int> top_errors;
        auto te_pos = analytics.find("top-errors:(");
        if (te_pos != std::string::npos) {
            auto content = te_pos + std::string("top-errors:(").size();
            int te_depth = 1;
            auto te_end = content;
            while (te_end < analytics.size() && te_depth > 0) {
                if (analytics[te_end] == '(')
                    ++te_depth;
                else if (analytics[te_end] == ')')
                    --te_depth;
                if (te_depth > 0)
                    ++te_end;
            }
            if (te_depth == 0) {
                // Interior only (exclude the "top-errors:(" label — Issue #1724).
                auto te = analytics.substr(content, te_end - content);
                std::size_t i = 0;
                while (i < te.size()) {
                    // find pattern "k:n"
                    while (i < te.size() && (te[i] == ' ' || te[i] == '('))
                        ++i;
                    auto kstart = i;
                    while (i < te.size() && te[i] != ':')
                        ++i;
                    if (i >= te.size())
                        break;
                    auto k = te.substr(kstart, i - kstart);
                    ++i; // skip ':'
                    auto nstart = i;
                    while (i < te.size() && te[i] != ' ' && te[i] != ')')
                        ++i;
                    try {
                        top_errors[k] = std::stoi(te.substr(nstart, i - nstart));
                    } catch (const std::exception&) {
                        // Issue #1724 / #1725: top-errors stoi failure — keep
                        // slot default + bump analytics parse metric
                        // (best-effort telemetry; not user input).
                        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                            m->agent_evolve_analytics_parse_failures.fetch_add(
                                1, std::memory_order_relaxed);
                    }
                }
            }
        }

        // Build new strategy (preserve old, copy with bumped fields)
        Evaluator::StrategyDef evolved;
        evolved.name = src->name + "-v" + std::to_string(src->evolution + 1);
        evolved.body = src->body;
        evolved.max_attempts = src->max_attempts;
        evolved.temperature = src->temperature;
        evolved.sys_prompt_template = src->sys_prompt_template;
        evolved.evolution = src->evolution + 1;
        evolved.parent = src->name;

        std::string reason;
        if (success_rate < 0.5 && avg_attempts >= static_cast<double>(src->max_attempts)) {
            int bumped = std::min(20, src->max_attempts + 2);
            evolved.max_attempts = bumped;
            reason = "success-rate " + std::to_string(success_rate).substr(0, 4) +
                     " < 0.5 with avg-attempts=" + std::to_string(static_cast<int>(avg_attempts)) +
                     " = max-attempts; bumped " + std::to_string(src->max_attempts) + " → " +
                     std::to_string(bumped);
        } else if (success_rate > 0.9 && avg_attempts < 1.5) {
            int lowered = std::max(1, src->max_attempts - 1);
            evolved.max_attempts = lowered;
            reason = "success-rate > 0.9 with avg-attempts < 1.5; lowered " +
                     std::to_string(src->max_attempts) + " → " + std::to_string(lowered);
        }

        // Add sys-prompt-template hints based on top errors
        auto append_hint = [&](const std::string& key, const std::string& hint) {
            auto it = top_errors.find(key);
            if (it != top_errors.end() && it->second >= 2) {
                evolved.sys_prompt_template += hint;
                if (!reason.empty())
                    reason += "; ";
                reason += "appended hint for " + key;
            }
        };
        append_hint("unbound-variable", "\nDo NOT use undefined variables.");
        append_hint("type-mismatch", "\nUse (check x : Type) before operations.");
        append_hint("div-zero", "\nGuard division with (if (= d 0) ...).");
        append_hint("syntax-error", "\nCheck parentheses carefully.");

        if (reason.empty())
            reason = "no heuristics matched; clone unchanged";

        ev.timeline_push("evolve:" + evolved.name + " from " + src->name + " (" + reason + ")");

        // Insert into ev.strategies_ (avoid name collision: bump suffix).
        // Issue #1726: cap iterations — unbounded for(;;) can hang if
        // strategies_ is stuffed with every base + "-N" variant.
        {
            std::unique_lock<std::shared_mutex> lk(ev.strategies_mtx_); // #1720/#1722
            const std::string base_name = evolved.name;
            std::string final_name = base_name;
            constexpr int kMaxNameCollisions = 10000;
            bool found_slot = false;
            for (int attempt = 0; attempt < kMaxNameCollisions; ++attempt) {
                bool taken = false;
                for (auto& s : ev.strategies_) {
                    if (s.name == final_name) {
                        taken = true;
                        break;
                    }
                }
                if (!taken) {
                    found_slot = true;
                    break;
                }
                // base, base-2, base-3, ...
                final_name = base_name + "-" + std::to_string(attempt + 2);
            }
            if (!found_slot) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->agent_evolve_name_collision_exhausted.fetch_add(1,
                                                                       std::memory_order_relaxed);
                return make_void(); // give up rather than hang
            }
            evolved.name = final_name;
            ev.strategies_.push_back(evolved);
        }

        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(evolved.name);
        return types::make_string(sid);
    });

    // ── parallel-intend — Issue #1587: Aura surface for parallel_orch (#1586) ──
    //
    // (parallel-intend tasks
    //    [:max-concurrency n] [:timeout-ms ms] [:fail-fast bool] [:collect-errors bool]
    //    [:pure bool] …)   ; Issue #2163 — default pure=#f (eval-serialized=#t)
    //
    // tasks: vector or list of 0-arg closures (thunks).
    // Returns a hash:
    //   status, ok-count, err-count, aborted-count, wait-us, results
    // where results is a vector of per-task hashes {ok, index, value|error}.
    //
    // Wires to aura::serve::parallel_orch::parallel_intend (Fiber::join +
    // concurrency gate). Default: apply_closure serialized via eval_mu.
    // :pure #t (Issue #2163): skip eval_mu for known-pure thunks; mutating
    // thunks fail with pure-contract-violated (or force-lock + metric).
    add("parallel-intend", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            // FlatHashTable probes with (cap-1) mask → capacity must be power of 2.
            // #2007 added retries/circuit keys; kv*2 can become non-pow2 (e.g. 20).
            std::size_t need = std::max<std::size_t>(16, kv.size() * 2);
            std::size_t cap = 16;
            while (cap < need)
                cap <<= 1;
            auto* ht = FlatHashTable::create(cap);
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
                    auto slot = ((h >> 1) + at) & (hcap - 1);
                    if (meta[slot] == 0xFF) {
                        meta[slot] = fp;
                        keys[slot] = key_ev.val;
                        vals[slot] = v.val;
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

        auto heap_str = [&](const EvalValue& v) -> std::string {
            if (types::is_string(v))
                return heap_str_from(ev.string_heap_, v);
            if (types::is_keyword(v)) {
                auto kidx = types::as_keyword_idx(v);
                if (kidx < ev.keyword_table_.size())
                    return ev.keyword_table_[kidx];
            }
            return {};
        };

        if (a.empty()) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "parallel-intend: usage (parallel-intend tasks ...)",
                                        ev.primitive_error_counter_ptr());
        }

        // Issue #2746: optional :region-keys vector of ints, same length as
        // tasks (or prefix; missing → 0). Paired into TaskSpec.region_key.
        std::vector<std::uint64_t> region_keys;
        // Issue #2760: optional :cone-masks vector of ints (ImpactScope /
        // dirty-bit packed masks) for #2754/#2757 mask-AND concurrent admit
        // when keys collide or are zero. Paired into TaskSpec.cone_mask.
        std::vector<std::uint64_t> cone_masks;

        // Collect 0-arg closure ids from vector or list.
        std::vector<std::uint64_t> cids;
        if (types::is_vector(a[0])) {
            auto vidx = types::as_vector_idx(a[0]);
            if (vidx >= ev.vector_heap_.size()) {
                return make_primitive_error(ev.string_heap_, ev.error_values_,
                                            "parallel-intend: bad vector",
                                            ev.primitive_error_counter_ptr());
            }
            for (auto& e : ev.vector_heap_[vidx]) {
                if (!types::is_closure(e)) {
                    return make_primitive_error(ev.string_heap_, ev.error_values_,
                                                "parallel-intend: tasks must be closures",
                                                ev.primitive_error_counter_ptr());
                }
                cids.push_back(types::as_closure_id(e));
            }
        } else if (types::is_pair(a[0]) || types::is_void(a[0])) {
            EvalValue cur = a[0];
            while (types::is_pair(cur)) {
                auto pidx = types::as_pair_idx(cur);
                if (pidx >= ev.pairs_.size())
                    break;
                auto& e = ev.pairs_[pidx].car;
                if (!types::is_closure(e)) {
                    return make_primitive_error(ev.string_heap_, ev.error_values_,
                                                "parallel-intend: tasks must be closures",
                                                ev.primitive_error_counter_ptr());
                }
                cids.push_back(types::as_closure_id(e));
                cur = ev.pairs_[pidx].cdr;
            }
        } else {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "parallel-intend: tasks must be vector or list",
                                        ev.primitive_error_counter_ptr());
        }

        using aura::serve::parallel_orch::ParallelPolicy;
        ParallelPolicy policy{};
        // Issue #2163: default pure=#f → eval_mu serialization (AC1).
        bool pure_mode = false;
        // Keyword / optional positional policy args after tasks.
        for (std::size_t i = 1; i < a.size();) {
            if (i + 1 < a.size() && (types::is_string(a[i]) || types::is_keyword(a[i]))) {
                auto k = heap_str(a[i]);
                // Normalize ":max-concurrency" / "max-concurrency" forms.
                while (!k.empty() && k[0] == ':')
                    k = k.substr(1);
                auto& val = a[i + 1];
                if ((k == "max-concurrency" || k == "max_concurrency") && types::is_int(val)) {
                    policy.max_concurrency =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(1, types::as_int(val)));
                } else if ((k == "timeout-ms" || k == "timeout_ms") && types::is_int(val)) {
                    policy.timeout_ms =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                } else if ((k == "fail-fast" || k == "fail_fast") && types::is_bool(val)) {
                    policy.fail_fast = types::as_bool(val);
                } else if ((k == "collect-errors" || k == "collect_errors") &&
                           types::is_bool(val)) {
                    policy.collect_errors = types::as_bool(val);
                } else if (k == "pure" && types::is_bool(val)) {
                    // Issue #2163 Phase A/B: optional pure path.
                    pure_mode = types::as_bool(val);
                } else if ((k == "max-retries" || k == "max_retries") && types::is_int(val)) {
                    // Issue #2007 RetryN
                    policy.max_retries =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                    policy.failure_policy = aura::serve::parallel_orch::FailurePolicy::RetryN;
                } else if ((k == "consecutive-fail-limit" || k == "consecutive_fail_limit") &&
                           types::is_int(val)) {
                    // Issue #2007 CircuitBreaker
                    policy.consecutive_fail_limit =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(1, types::as_int(val)));
                    policy.failure_policy =
                        aura::serve::parallel_orch::FailurePolicy::CircuitBreaker;
                } else if ((k == "retry-backoff-ms" || k == "retry_backoff_ms") &&
                           types::is_int(val)) {
                    policy.retry_backoff_ms =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                } else if ((k == "failure-policy" || k == "failure_policy") &&
                           (types::is_string(val) || types::is_keyword(val))) {
                    auto v = heap_str(val);
                    while (!v.empty() && v[0] == ':')
                        v = v.substr(1);
                    using FP = aura::serve::parallel_orch::FailurePolicy;
                    if (v == "fail-fast" || v == "fail_fast")
                        policy.failure_policy = FP::FailFast;
                    else if (v == "collect-all" || v == "collect_all")
                        policy.failure_policy = FP::CollectAll;
                    else if (v == "retry-n" || v == "retry_n" || v == "retry")
                        policy.failure_policy = FP::RetryN;
                    else if (v == "circuit-breaker" || v == "circuit_breaker" || v == "circuit")
                        policy.failure_policy = FP::CircuitBreaker;
                } else if ((k == "region-keys" || k == "region_keys") && types::is_vector(val)) {
                    // Issue #2746: per-task region keys for #2724 concurrent admit.
                    auto rvidx = types::as_vector_idx(val);
                    if (rvidx < ev.vector_heap_.size()) {
                        region_keys.clear();
                        for (auto& e : ev.vector_heap_[rvidx]) {
                            if (types::is_int(e))
                                region_keys.push_back(static_cast<std::uint64_t>(
                                    std::max<std::int64_t>(0, types::as_int(e))));
                            else
                                region_keys.push_back(0);
                        }
                    }
                } else if ((k == "cone-masks" || k == "cone_masks") && types::is_vector(val)) {
                    // Issue #2760: per-task ImpactScope / dirty-bit cone masks
                    // for #2754/#2757 mask-AND when keys collide or are zero.
                    auto cvidx = types::as_vector_idx(val);
                    if (cvidx < ev.vector_heap_.size()) {
                        cone_masks.clear();
                        for (auto& e : ev.vector_heap_[cvidx]) {
                            if (types::is_int(e))
                                cone_masks.push_back(static_cast<std::uint64_t>(
                                    std::max<std::int64_t>(0, types::as_int(e))));
                            else
                                cone_masks.push_back(0);
                        }
                    }
                }
                i += 2;
                continue;
            }
            // Positional: max-concurrency, timeout-ms, fail-fast
            if (types::is_int(a[i]) && i == 1) {
                policy.max_concurrency =
                    static_cast<std::uint32_t>(std::max<std::int64_t>(1, types::as_int(a[i])));
                ++i;
                continue;
            }
            if (types::is_int(a[i]) && i == 2) {
                policy.timeout_ms =
                    static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(a[i])));
                ++i;
                continue;
            }
            if (types::is_bool(a[i]) && i == 3) {
                policy.fail_fast = types::as_bool(a[i]);
                ++i;
                continue;
            }
            ++i;
        }

        if (!aura::serve::parallel_orch::validate_policy(policy)) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "parallel-intend: invalid policy",
                                        ev.primitive_error_counter_ptr());
        }

        // Issue #2660: security-schedule-gate once per batch admit
        // (before the unlocked/locked apply batch). Same gate pattern as
        // MutationBoundaryGuard::try_acquire — production hard-rejects
        // with structured AdmissionRejected: security-schedule:<reason>;
        // soft / sandbox=off stays observe-only (metric-only, no deny).
        // Counters always bump via evaluate_security_schedule.
        {
            const auto prod = aura::compiler::typed_audit::production_defaults_active();
            const auto in = aura::orch::make_security_schedule_input_live(
                ev.effect_sandbox_mode(), prod, /*soft_mode=*/!prod);
            if (auto reason = aura::orch::admit_security_schedule(in); reason.has_value()) {
                return make_primitive_error(ev.string_heap_, ev.error_values_, *reason,
                                            ev.primitive_error_counter_ptr());
            }
        }

        if (cids.empty()) {
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"status",
                 [&] {
                     auto sidx = ev.string_heap_.size();
                     ev.string_heap_.push_back("ok");
                     return make_string(sidx);
                 }()},
                {"ok-count", make_int(0)},
                {"err-count", make_int(0)},
                {"aborted-count", make_int(0)},
                {"wait-us", make_int(0)},
                {"results",
                 [&] {
                     auto vidx = ev.vector_heap_.size();
                     ev.vector_heap_.push_back({});
                     return make_vector(vidx);
                 }()},
                {"schema", make_int(1587)},
            };
            return build_hash(kv);
        }

        struct AuraShared {
            std::mutex eval_mu;
            std::vector<EvalValue> values;
            std::vector<std::string> errors;
            // Issue #2163: per-batch pure accounting (task bodies bump atomics).
            std::atomic<std::uint64_t> pure_unlocked_applies{0};
            std::atomic<std::uint64_t> pure_fallback_locked{0};
            std::atomic<std::uint64_t> pure_contract_violated{0};
            // Issue #2662: batch_force_eval_mu — once a pure task violates the
            // contract in production mode + parallel_intend_force_lock_on_violation
            // flag, subsequent pure tasks in this batch take eval_mu (per-batch).
            std::atomic<bool> batch_force_eval_mu{false};
        };
        auto ash = std::make_shared<AuraShared>();
        ash->values.assign(cids.size(), make_void());
        ash->errors.assign(cids.size(), {});

        std::vector<aura::serve::parallel_orch::TaskSpec> tasks;
        tasks.reserve(cids.size());
        for (std::size_t i = 0; i < cids.size(); ++i) {
            const auto cid = cids[i];
            const std::uint64_t rkey = (i < region_keys.size()) ? region_keys[i] : 0;
            const std::uint64_t cmask = (i < cone_masks.size()) ? cone_masks[i] : 0;
            tasks.push_back(aura::serve::parallel_orch::TaskSpec{
                .body = [&ev, ash, cid, i, pure_mode, rkey,
                         cmask]() -> aura::serve::parallel_orch::TaskResult {
                    // Issue #2746 / #2760: stamp parallel-task region + cone
                    // TLS so try_acquire → try_acquire_for_region (#2724) and
                    // mask-AND concurrent admit (#2754/#2757) when enabled.
                    struct RegionTls {
                        explicit RegionTls(std::uint64_t k, std::uint64_t m) {
                            if (k != 0)
                                Evaluator::note_parallel_task_region_key(k);
                            if (m != 0)
                                Evaluator::note_parallel_task_cone_mask(m);
                        }
                        ~RegionTls() {
                            Evaluator::clear_parallel_task_region_key();
                            Evaluator::clear_parallel_task_cone_mask();
                        }
                    } region_tls(rkey, cmask);
                    aura::serve::parallel_orch::TaskResult tr;
                    tr.task_index = i;
                    try {
                        // Issue #2163: pure path may skip eval_mu. Force lock when
                        // a mutation boundary is already held (unsafe concurrent
                        // pure apply) — pure_fallback_locked_total.
                        // Issue #2662: also force lock when batch_force_eval_mu
                        // is set (production + opt-in flag, post a pure-contract
                        // violation in the same batch — see wire-up below).
                        const bool force_lock =
                            pure_mode &&
                            (ev.mutation_boundary_held() || ev.mutation_boundary_depth() > 0 ||
                             ash->batch_force_eval_mu.load(std::memory_order_relaxed));
                        const bool use_lock = !pure_mode || force_lock;
                        std::unique_lock<std::mutex> lock(ash->eval_mu, std::defer_lock);
                        if (use_lock) {
                            lock.lock();
                            if (force_lock) {
                                ash->pure_fallback_locked.fetch_add(1, std::memory_order_relaxed);
                                aura::orch::g_orch_module_stats.pure_fallback_locked_total
                                    .fetch_add(1, std::memory_order_relaxed);
                            }
                        } else {
                            ash->pure_unlocked_applies.fetch_add(1, std::memory_order_relaxed);
                            aura::orch::g_orch_module_stats.pure_parallel_tasks_total.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        // Issue #1719: refuse apply on freed closure (sibling of intend).
                        if (!agent_cid_live(ev, cid)) {
                            agent_note_closure_freed_call(ev);
                            tr.ok = false;
                            tr.error = "closure-freed";
                            ash->errors[i] = tr.error;
                        } else {
                            // Issue #2163 AC3: snapshot defuse for pure-contract probe.
                            const auto defuse_before =
                                pure_mode ? ev.defuse_version() : std::uint64_t{0};
                            // Issue #2634 AC1: extend the pure-contract probe with
                            // total_mutations() + workspace_generation() snapshots.
                            // Indirect writers (engine:metrics, side caches,
                            // persistent TypeChecker reuse #2220) may mutate
                            // without bumping defuse_version; the additional
                            // snapshots catch that class of pure-contract
                            // violation. Zero cost when !pure_mode (the
                            // ?-expressions short-circuit to literal 0 below).
                            const auto mut_before =
                                pure_mode ? ev.total_mutations() : std::uint64_t{0};
                            const auto ws_gen_before =
                                pure_mode ? ev.workspace_generation() : std::uint64_t{0};
                            auto opt = ev.apply_closure(cid, {});
                            if (!opt) {
                                tr.ok = false;
                                tr.error = "apply-failed";
                                ash->errors[i] = tr.error;
                            } else if (pure_mode && !force_lock &&
                                       (ev.defuse_version() != defuse_before ||
                                        ev.total_mutations() != mut_before ||
                                        ev.workspace_generation() != ws_gen_before ||
                                        ev.mutation_boundary_held() ||
                                        ev.mutation_boundary_depth() > 0)) {
                                // Mutating thunk under :pure #t — fail task (policy).
                                tr.ok = false;
                                tr.error = "pure-contract-violated";
                                ash->errors[i] = tr.error;
                                ash->pure_contract_violated.fetch_add(1, std::memory_order_relaxed);
                                aura::orch::g_orch_module_stats.pure_contract_violated_total
                                    .fetch_add(1, std::memory_order_relaxed);
                                // Issue #2662: production hardening — under
                                // production_defaults + opt-in flag, force
                                // remaining pure tasks in this batch to take
                                // eval_mu (best-effort-pure → serialized for
                                // the rest of the batch). NOT a transactional
                                // isolation promise — best-effort hardening.
                                if (aura::compiler::typed_audit::production_defaults_active() &&
                                    aura::orch::g_orch_module_stats
                                        .parallel_intend_force_lock_on_violation.load(
                                            std::memory_order_relaxed)) {
                                    ash->batch_force_eval_mu.store(true, std::memory_order_relaxed);
                                }
                            } else {
                                ash->values[i] = *opt;
                                if (types::is_error(*opt)) {
                                    tr.ok = false;
                                    tr.error = "task-error";
                                    ash->errors[i] = tr.error;
                                } else {
                                    tr.ok = true;
                                    tr.value = "ok";
                                }
                            }
                        }
                    } catch (const std::exception& ex) {
                        tr.ok = false;
                        tr.error = ex.what();
                        ash->errors[i] = tr.error;
                    } catch (...) {
                        // [SILENCE-PRIM-#615] task result records
                        // ok=false + error string for join/status
                        // (#1669 class A intentional-return-value).
                        tr.ok = false;
                        tr.error = "unknown-exception";
                        ash->errors[i] = tr.error;
                    }
                    return tr;
                },
                .name = "aura-task-" + std::to_string(i),
                .region_key = rkey,
                .cone_mask = cmask,
            });
        }

        // Issue #2543: self-throttle max_concurrency from aot-hot-update-health
        // when health_bp < budget (storm / force-jit → split-batch cap=1, etc.).
        // Advisory only — never hard-fails the batch.
        {
            const auto req = std::max<std::uint32_t>(policy.max_concurrency, 1);
            const auto capped = aura::compiler::apply_hot_update_health_concurrency_cap(req);
            policy.max_concurrency = capped;
            if (capped < req) {
                aura::orch::g_orch_module_stats.orch_hot_update_health_throttle_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        const int workers = static_cast<int>(
            std::min<std::uint32_t>(std::max<std::uint32_t>(policy.max_concurrency, 1), 8));
        aura::serve::Scheduler sched(workers);
        std::thread runner([&sched] { sched.run(); });
        auto batch = aura::serve::parallel_orch::parallel_intend(sched, tasks, policy);
        sched.stop();
        if (runner.joinable())
            runner.join();

        using aura::serve::parallel_orch::BatchStatus;
        const char* status_str = "invalid";
        switch (batch.status) {
            case BatchStatus::Ok:
                status_str = "ok";
                break;
            case BatchStatus::Partial:
                status_str = "partial";
                break;
            case BatchStatus::Timeout:
                status_str = "timeout";
                break;
            case BatchStatus::FailFast:
                status_str = "fail-fast";
                break;
            case BatchStatus::Invalid:
                status_str = "invalid";
                break;
            case BatchStatus::QuotaExceeded: // #1600
                status_str = "quota-exceeded";
                break;
        }

        std::vector<EvalValue> result_elems;
        result_elems.reserve(batch.results.size());
        for (std::size_t i = 0; i < batch.results.size(); ++i) {
            const auto& tr = batch.results[i];
            std::vector<std::pair<std::string, EvalValue>> tkv;
            tkv.push_back({"ok", make_bool(tr.ok)});
            tkv.push_back({"index", make_int(static_cast<std::int64_t>(i))});
            if (tr.ok) {
                EvalValue val = (i < ash->values.size()) ? ash->values[i] : make_void();
                // Issue #2632: closure values crossing the worker→main fiber
                // boundary in a parallel-intend batch must be handoff_ref'd.
                // Build a StableNodeRef from the closure's body_id and pipe
                // through handoff_ref; on unrefreshable the dedicated
                // stable_ref_handoff_reject_total counter bumps (distinct
                // from query-time export-stale-reject — see AC2 / linter).
                // 2632 AC4: parallel-intend result packaging wire-up.
                if (types::is_closure(val) && ev.workspace_flat_) {
                    const auto cid = types::as_closure_id(val);
                    if (cid < ev.closures_.size()) {
                        const auto& cl = ev.closures_[cid];
                        if (cl.body_id != aura::ast::NULL_NODE) {
                            // Issue #2759: layout + Evaluator stamp (sole production
                            // authority) — do not write tenant/fiber by hand or rely
                            // on process-global capture under multi-tenant hard-close.
                            auto ref = ev.workspace_flat_->make_ref_layout(cl.body_id);
                            ev.stamp_stable_ref(ref);
                            (void)ev.handoff_ref(std::move(ref));
                        }
                    }
                }
                tkv.push_back({"value", val});
            } else {
                std::string err =
                    tr.error.empty() && i < ash->errors.size() ? ash->errors[i] : tr.error;
                if (err.empty())
                    err = "error";
                auto eidx = ev.string_heap_.size();
                ev.string_heap_.push_back(err);
                tkv.push_back({"error", make_string(eidx)});
            }
            result_elems.push_back(build_hash(tkv));
        }
        auto rvidx = ev.vector_heap_.size();
        ev.vector_heap_.push_back(std::move(result_elems));

        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(status_str);

        // Issue #2163: eval-serialized=#f when pure path engaged for this batch
        // and at least one task applied unlocked (not all forced-lock fallback).
        const auto pure_unlocked = ash->pure_unlocked_applies.load(std::memory_order_relaxed);
        const auto pure_fallback = ash->pure_fallback_locked.load(std::memory_order_relaxed);
        const auto pure_viol = ash->pure_contract_violated.load(std::memory_order_relaxed);
        const bool pure_engaged = pure_mode && pure_unlocked > 0;
        if (pure_mode) {
            aura::orch::g_orch_module_stats.pure_parallel_batches_total.fetch_add(
                1, std::memory_order_relaxed);
        }

        // Issue #2400: isolation-level for Agent control planes.
        //   pure_mode=false → "serialized"   (default :pure #f / eval_mu)
        //   pure_mode=true  → "best-effort-pure"  (never "transactional";
        //                     even if all tasks fallback-locked)
        // C++ TaskSpec-only path (never touches Evaluator) may use "none"
        // outside this Aura primitive — not advertised here.
        // Do NOT advertise pure as transactional isolation (AC4 #2400 / #2230).
        const char* isolation_level = pure_mode ? "best-effort-pure" : "serialized";
        const auto iso_sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(isolation_level);

        // Issue #2743: surface underlying Fiber join status (incl. reclaimed)
        // on the batch hash so residual-reclaimed is not collapsed into timeout.
        const char* join_st = "ok";
        switch (batch.join_status) {
            case aura::serve::JoinStatus::Ok:
                join_st = "ok";
                break;
            case aura::serve::JoinStatus::Timeout:
                join_st = "timeout";
                break;
            case aura::serve::JoinStatus::Cancelled:
                join_st = "cancelled";
                break;
            case aura::serve::JoinStatus::Invalid:
                join_st = "invalid";
                break;
            case aura::serve::JoinStatus::Reclaimed:
                join_st = "reclaimed";
                break;
        }
        const auto join_sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(join_st);

        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"status", make_string(sidx)},
            {"ok-count", make_int(static_cast<std::int64_t>(batch.ok_count))},
            {"err-count", make_int(static_cast<std::int64_t>(batch.err_count))},
            {"aborted-count", make_int(static_cast<std::int64_t>(batch.aborted_count))},
            {"wait-us", make_int(static_cast<std::int64_t>(batch.wait_us))},
            // Issue #2007 FailurePolicy outcomes
            {"retries-performed", make_int(static_cast<std::int64_t>(batch.retries_performed))},
            {"circuit-opened", make_bool(batch.circuit_opened)},
            {"results", make_vector(rvidx)},
            // Issue #2081 / #2163: eval-serialization contract surface.
            // Default: apply_closure serialized via shared eval_mu.
            // :pure #t with unlocked applies → eval-serialized=#f.
            {"eval-serialized", make_bool(!pure_engaged)},
            {"pure", make_bool(pure_mode)},
            {"pure-unlocked-tasks", make_int(static_cast<std::int64_t>(pure_unlocked))},
            {"pure-fallback-locked", make_int(static_cast<std::int64_t>(pure_fallback))},
            {"pure-contract-violations", make_int(static_cast<std::int64_t>(pure_viol))},
            // Issue #2400: explicit isolation-level enum (additive).
            {"isolation-level", make_string(iso_sidx)},
            {"isolation-level-wired", make_int(1)},
            // Issue #2743: Fiber join status (reclaimed residual visible).
            {"join-status", make_string(join_sidx)},
            {"join-status-reclaimed",
             make_bool(batch.join_status == aura::serve::JoinStatus::Reclaimed)},
            // Issue #2746: region concurrent surface (#2724 integration).
            {"region-keys-supplied",
             make_int(static_cast<std::int64_t>(
                 aura::serve::parallel_orch::g_parallel_orch_stats.region_keys_supplied_total.load(
                     std::memory_order_relaxed)))},
            {"region-concurrent-batches",
             make_int(static_cast<std::int64_t>(
                 aura::serve::parallel_orch::g_parallel_orch_stats.region_concurrent_batches_total
                     .load(std::memory_order_relaxed)))},
            {"region-concurrent-eligible", make_bool([&] {
                 std::uint64_t distinct = 0;
                 std::uint64_t seen[8] = {};
                 std::size_t n_seen = 0;
                 for (const auto& t : tasks) {
                     if (t.region_key == 0)
                         continue;
                     bool already = false;
                     for (std::size_t j = 0; j < n_seen; ++j) {
                         if (seen[j] == t.region_key) {
                             already = true;
                             break;
                         }
                     }
                     if (!already) {
                         if (n_seen < 8)
                             seen[n_seen++] = t.region_key;
                         ++distinct;
                     }
                 }
                 return distinct >= 2;
             }())},
            {"schema", make_int(1587)},
            {"schema-2007", make_int(2007)},
            {"schema-2081", make_int(2081)},
            {"schema-2163", make_int(2163)},
            {"schema-2400", make_int(2400)},
            {"schema-2743", make_int(2743)},
            {"schema-2746", make_int(2746)},
        };
        if (pure_engaged) {
            kv.push_back({"schema-pure-parallel", make_int(2163)});
        }
        return build_hash(kv);
    });

    // ── Issue #1588 / #2011: unified src/orch/ Aura surface ────────
    //
    // (orch:spawn-agent name [thunk]
    //    [:attach-mailbox bool] [:high-water n] [:keepalive-interval-ms n])
    //   → hash {ok, id, name, schema=1588, [quota-exceeded, error]}
    // (orch:agent-join name [:timeout-ms n]) → hash {status, wait-us, ok}
    // (orch:agent-send name payload) → hash {ok, status, schema}
    // (orch:agent-recv name [:wait bool] [:timeout-ms n]) → hash {ok, empty, payload}
    // (orch:parallel-intend …) → alias of (parallel-intend …)
    // (engine:metrics "query:orch-module-stats") → OrchModuleStats snapshot

    auto build_orch_hash =
        [&ev](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
        // FlatHashTable probes with (cap-1) mask → capacity must be power of 2.
        std::size_t need = std::max<std::size_t>(16, kv.size() * 2);
        std::size_t cap = 16;
        while (cap < need)
            cap <<= 1;
        auto* ht = FlatHashTable::create(cap);
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
                auto slot = ((h >> 1) + at) & (hcap - 1);
                if (meta[slot] == 0xFF) {
                    meta[slot] = fp;
                    keys[slot] = key_ev.val;
                    vals[slot] = v.val;
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

    auto orch_keyword_key = [&ev](const EvalValue& v) -> std::string {
        std::string k;
        if (types::is_string(v))
            k = heap_str_from(ev.string_heap_, v);
        else if (types::is_keyword(v)) {
            auto ki = types::as_keyword_idx(v);
            if (ki < ev.keyword_table_.size())
                k = ev.keyword_table_[ki];
        }
        while (!k.empty() && k[0] == ':')
            k = k.substr(1);
        return k;
    };

    // Keep a process-local scheduler for orch:spawn-agent tests (stdin-friendly).
    // Lazy-init: shared across calls in-process; stopped on process exit.
    struct OrchSchedHolder {
        std::unique_ptr<aura::serve::Scheduler> sched;
        std::thread runner;
        std::mutex mu;
        void ensure(int workers = 2) {
            std::lock_guard lock(mu);
            if (sched)
                return;
            sched = std::make_unique<aura::serve::Scheduler>(workers);
            runner = std::thread([this] { sched->run(); });
        }
        ~OrchSchedHolder() {
            if (sched)
                sched->stop();
            if (runner.joinable())
                runner.join();
        }
    };
    static OrchSchedHolder orch_sched;

    // Issue #1966 / #2078: name→AgentHandle bookkeeping for MVP orch:spawn-agent /
    // orch:agent-join. Formerly aura::orch::AgentRegistry (public deferred
    // multi-agent surface); demoted to TU-local static table for MVP, but
    // `static` still crossed Evaluator lifetime (#1966). Issue #2078 moves
    // storage to Evaluator::agent_names_ (per-Evaluator, see
    // src/compiler/agent_name_table.h) so two CompilerService instances can
    // use the same agent name without cross-talk. Drained at ~Evaluator
    // via Evaluator::cleanup_orch_agents(). Not a multi-agent coordination
    // API — check_orch_mvp_scope.py --strict still guards the public surface.

    add("orch:spawn-agent",
        [&ev, build_orch_hash, orch_keyword_key](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !types::is_string(a[0])) {
                return make_primitive_error(
                    ev.string_heap_, ev.error_values_,
                    "orch:spawn-agent: usage (orch:spawn-agent name [thunk] [:kw val]…)",
                    ev.primitive_error_counter_ptr());
            }
            auto name = heap_str_from(ev.string_heap_, a[0]);
            std::optional<std::uint64_t> cid;
            std::size_t i = 1;
            if (i < a.size() && types::is_closure(a[i])) {
                cid = types::as_closure_id(a[i]);
                ++i;
            }
            // Issue #2011: optional policy keywords.
            bool attach_mailbox = true;
            std::size_t high_water = 256;
            std::uint32_t keepalive_interval_ms = 0;
            std::uint32_t max_no_yield_ms = 0; // Issue #2540
            // Issue #2591: per-spec BP admit threshold override
            // (multi-tenant / multi-scope isolation). nullopt → process
            // default (#2535 default=32, env override via
            // AURA_ORCH_BP_ADMIT_THRESHOLD); 0 → admit off for THIS
            // spawn (always reject under attach_mailbox); N > 0 →
            // local threshold. Gauge stays process-global; v1 isolates
            // policy per spawn, not per gauge.
            std::optional<std::uint64_t> bp_admit_threshold{};
            // Issue #2633: scope-local BP gauge key. empty = process
            // bucket (legacy / backward-compat with #2591); non-empty =
            // per-scope recent counter + admit (storm in A no longer
            // poisons B/C). Wired to AgentSpec::bp_scope_id at line 2883.
            std::string bp_scope_id{};
            for (; i + 1 < a.size(); i += 2) {
                auto k = orch_keyword_key(a[i]);
                auto& val = a[i + 1];
                if ((k == "attach-mailbox" || k == "attach_mailbox") && types::is_bool(val)) {
                    attach_mailbox = types::as_bool(val);
                } else if ((k == "high-water" || k == "high_water" || k == "mailbox-high-water") &&
                           types::is_int(val)) {
                    high_water =
                        static_cast<std::size_t>(std::max<std::int64_t>(1, types::as_int(val)));
                } else if ((k == "keepalive-interval-ms" || k == "keepalive_interval_ms") &&
                           types::is_int(val)) {
                    keepalive_interval_ms =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                } else if ((k == "max-no-yield-ms" || k == "max_no_yield_ms") &&
                           types::is_int(val)) {
                    // Issue #2540: cooperative yield budget (0 = off).
                    max_no_yield_ms =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                } else if ((k == "bp-admit-threshold" || k == "bp_admit_threshold") &&
                           types::is_int(val)) {
                    // Issue #2591: per-spec BP admit override. Negative
                    // or absent → leave nullopt (process default); 0 →
                    // admit off for this spawn; N > 0 → local threshold.
                    const auto raw = types::as_int(val);
                    if (raw >= 0) {
                        bp_admit_threshold = static_cast<std::uint64_t>(raw);
                    }
                } else if ((k == "bp-scope-id" || k == "bp_scope_id") && types::is_string(val)) {
                    // Issue #2633: scope-local BP gauge key. Routing
                    // string is interned via the per-Evaluator string
                    // heap so the lookup at admit preflight is O(1)
                    // (avoids re-interning on every spawn). empty
                    // string keeps the process-bucket default.
                    const auto sid = types::as_string_idx(val);
                    if (sid < ev.string_heap_.size())
                        bp_scope_id = ev.string_heap_[sid];
                }
            }

            orch_sched.ensure(2);
            auto body = [&ev, cid]() {
                if (!cid)
                    return;
                try {
                    // Issue #2158: per-Evaluator apply gate — not process-static.
                    // try_acquire reject path never reaches this body (agent_spawn.h
                    // wraps body after aura_orch_agent_body_try_acquire_ex succeeds),
                    // so we never hold agent_apply_mu_ on quota-reject (AC5).
                    const auto t0 = std::chrono::steady_clock::now();
                    std::lock_guard lock(ev.agent_apply_mu_);
                    const auto wait_us = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count());
                    aura::orch::g_orch_module_stats.agent_apply_lock_acquisitions_total.fetch_add(
                        1, std::memory_order_relaxed);
                    aura::orch::g_orch_module_stats.agent_apply_lock_wait_us_total.fetch_add(
                        wait_us, std::memory_order_relaxed);
                    // Issue #1719: refuse apply on freed thunk closure.
                    if (!agent_cid_live(ev, *cid)) {
                        agent_note_closure_freed_call(ev);
                        return;
                    }
                    (void)ev.apply_closure(*cid, {});
                } catch (...) {
                    // [SILENCE-PRIM-#615] swallow: agent body errors surface
                    // via join/status only (#1669 class B intentional-state).
                }
            };
            aura::orch::AgentSpec spec;
            spec.name = name;
            spec.body = std::move(body);
            spec.attach_mailbox = attach_mailbox;
            spec.mailbox_high_water = high_water;
            spec.keepalive_interval_ms = keepalive_interval_ms;
            spec.max_no_yield_ms = max_no_yield_ms;       // Issue #2540
            spec.bp_admit_threshold = bp_admit_threshold; // Issue #2591
            spec.bp_scope_id = std::move(bp_scope_id);    // Issue #2633
            auto handle = aura::orch::spawn_agent_with_mailbox(*orch_sched.sched, std::move(spec));
            // Issue #2009: move-only handle; snapshot then put on success.
            const bool ok = handle.ok;
            const bool quota_exceeded = handle.quota_exceeded;
            const auto id = handle.id;
            const std::string out_name = handle.name.empty() ? name : handle.name;
            const std::string err = handle.error;
            // Issue #2079: structured quota-reject fields (snapshot before any handle move).
            const std::string qdim = handle.quota_dimension;
            const std::uint64_t qused = handle.quota_used;
            const std::uint64_t qlimit = handle.quota_limit;
            const std::uint64_t qretry = handle.retry_after_ms;
            // Issue #2079 / #2155: put ONLY on ok. Quota-reject must never
            // register into agent_names_ (no leaked name-table slots).
            if (ok)
                ev.agent_names_->put(std::move(handle));

            // Issue #2011 / #2079: quota reject returns a structured hash (not
            // primitive-error) so Agent frameworks can branch on quota-dimension /
            // quota-used / quota-limit / retry-after-ms without parsing error
            // strings. Field names align with `query:resource-quota-stats` and
            // `query:orch-module-stats` keys for consistency.
            if (!ok && quota_exceeded) {
                // Issue #2155: invariant — this attempt did not put. A prior
                // live agent may still occupy the same name; that is fine.
                // Handle was never moved into the table on this path.
                (void)handle; // not put; destructor releases any residual (none)
                std::vector<std::pair<std::string, EvalValue>> qkv = {
                    {"ok", make_bool(false)},
                    {"id", make_int(0)},
                    {"name", make_string(ev.string_heap_.size())},
                    {"quota-exceeded", make_bool(true)},
                    {"schema", make_int(1588)},
                    {"schema-2011", make_int(2011)},
                    {"schema-2079", make_int(2079)},
                    {"schema-2155", make_int(aura::orch::kSpawnQuotaNoLeakIssue)},
                    {"spawn-quota-reject-no-leak",
                     make_int(static_cast<std::int64_t>(
                         aura::orch::g_orch_module_stats.spawn_quota_reject_no_leak.load(
                             std::memory_order_relaxed)))},
                };
                // Sentinel name for quota-reject (we never put into agent_names_).
                ev.string_heap_.push_back(out_name);
                if (!qdim.empty()) {
                    auto qidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(qdim);
                    qkv.push_back({"quota-dimension", make_string(qidx)});
                }
                qkv.push_back({"quota-used", make_int(static_cast<std::int64_t>(qused))});
                qkv.push_back({"quota-limit", make_int(static_cast<std::int64_t>(qlimit))});
                qkv.push_back({"retry-after-ms", make_int(static_cast<std::int64_t>(qretry))});
                const std::string qerr =
                    err.empty() ? std::string("ResourceQuotaExceeded: orch spawn rejected") : err;
                auto eidx = ev.string_heap_.size();
                ev.string_heap_.push_back(qerr);
                qkv.push_back({"error", make_string(eidx)});
                return build_orch_hash(qkv);
            }

            auto nidx = ev.string_heap_.size();
            ev.string_heap_.push_back(out_name);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ok", make_bool(ok)},           {"id", make_int(static_cast<std::int64_t>(id))},
                {"name", make_string(nidx)},     {"schema", make_int(1588)},
                {"schema-2011", make_int(2011)}, {"quota-exceeded", make_bool(quota_exceeded)},
            };
            if (!ok && !err.empty()) {
                auto eidx = ev.string_heap_.size();
                ev.string_heap_.push_back(err);
                kv.push_back({"error", make_string(eidx)});
            }
            return build_orch_hash(kv);
        });

    // (orch:agent-join name [:timeout-ms n] [:drain-ms n])
    // Issue #2153: :drain-ms secondary cancel-drain window (default 2000).
    add("orch:agent-join",
        [&ev, build_orch_hash, orch_keyword_key](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty()) {
                return make_primitive_error(ev.string_heap_, ev.error_values_,
                                            "orch:agent-join: need name",
                                            ev.primitive_error_counter_ptr());
            }
            aura::orch::JoinPolicy policy{};
            policy.drain_ms = aura::orch::kDefaultJoinDrainMs;
            for (std::size_t i = 1; i + 1 < a.size(); i += 2) {
                auto k = orch_keyword_key(a[i]);
                if ((k == "timeout-ms" || k == "timeout_ms") && types::is_int(a[i + 1]))
                    policy.primary_ms = static_cast<std::uint64_t>(
                        std::max<std::int64_t>(0, types::as_int(a[i + 1])));
                else if ((k == "drain-ms" || k == "drain_ms") && types::is_int(a[i + 1]))
                    policy.drain_ms = static_cast<std::uint64_t>(
                        std::max<std::int64_t>(0, types::as_int(a[i + 1])));
            }

            aura::orch::AgentHandle* hp = nullptr;
            if (types::is_string(a[0])) {
                auto name = heap_str_from(ev.string_heap_, a[0]);
                hp = ev.agent_names_->find(name);
            }
            // Join-by-id is intentionally not supported (name table is name-keyed).
            if (!hp) {
                std::vector<std::pair<std::string, EvalValue>> kv = {
                    {"ok", make_bool(false)},
                    {"status",
                     [&] {
                         auto s = ev.string_heap_.size();
                         ev.string_heap_.push_back("invalid");
                         return make_string(s);
                     }()},
                    {"wait-us", make_int(0)},
                    {"schema", make_int(1588)},
                    {"schema-2011", make_int(2011)},
                    {"schema-2153", make_int(aura::orch::kJoinDrainTimeoutIssue)},
                    {"drain-ms", make_int(static_cast<std::int64_t>(policy.drain_ms))},
                };
                return build_orch_hash(kv);
            }

            auto jr = aura::orch::join_agent(*hp, policy);
            const char* st = "ok";
            switch (jr.status) {
                case aura::serve::JoinStatus::Ok:
                    st = "ok";
                    break;
                case aura::serve::JoinStatus::Timeout:
                    st = "timeout";
                    break;
                case aura::serve::JoinStatus::Cancelled:
                    st = "cancelled";
                    break;
                case aura::serve::JoinStatus::Invalid:
                    st = "invalid";
                    break;
                // Issue #2467 / #2743: target force-reclaimed but body still
                // executing. Joiner deferred cleanup (no UAF). Map to
                // "reclaimed" status string so orch hash surfaces the
                // new path for dashboards + operator queries.
                case aura::serve::JoinStatus::Reclaimed:
                    st = "reclaimed";
                    break;
            }
            if (jr.status == aura::serve::JoinStatus::Reclaimed) {
                // Issue #2743 AC4: Aura-side agent-join reclaimed counter
                // (join_reclaimed_deferred_cleanup_total remains authoritative
                // for C++ cleanup path).
                aura::orch::g_orch_module_stats.agent_join_reclaimed_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(st);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ok", make_bool(jr.status == aura::serve::JoinStatus::Ok)},
                {"status", make_string(sidx)},
                {"wait-us", make_int(static_cast<std::int64_t>(jr.wait_us))},
                {"schema", make_int(1588)},
                {"schema-2011", make_int(2011)},
                {"schema-2153", make_int(aura::orch::kJoinDrainTimeoutIssue)},
                {"schema-2743", make_int(2743)},
                {"issue-2743", make_int(2743)},
                {"drain-ms", make_int(static_cast<std::int64_t>(policy.drain_ms))},
            };
            return build_orch_hash(kv);
        });

    // Issue #2588: Aura language surface for AgentScope supervision.
    // Per-Evaluator scope (not process-static — see agent_scope.h
    // g_evaluator_agent_scopes map). Mirrors Aura semantics:
    //   (orch:scope-spawn name body [:keepalive-ms n] [:max-no-yield-ms n] ...)
    //   (orch:scope-watch [:stall-ms n] [:policy 'cancel|'report-only|'restart-n]
    //                       [:max-restarts n] [:consecutive-stall-limit n])
    //   (orch:scope-join-all [:timeout-ms n] [:drain-ms n])
    //   (orch:scope-cancel-all)
    // MVP linter guard: scripts/coverage/checks/check_orch_mvp_scope.py still rejects
    // AgentRegistry / global_agent_registry. The scope map is just a
    // storage convenience; the AgentScope objects themselves are
    // per-Evaluator and do not become a global agent map.

    // Issue #2588 AC1: orch:scope-spawn name [body] [:keepalive-ms n]
    //   [:max-no-yield-ms n] ... → hash {ok, id, name, schema=2588, status}.
    // Body thunk is optional for MVP (empty body = supervised no-op);
    // closure-ID parsing mirrors orch:spawn-agent pattern.
    add("orch:scope-spawn",
        [&ev, build_orch_hash, orch_keyword_key](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !types::is_string(a[0])) {
                return make_primitive_error(
                    ev.string_heap_, ev.error_values_,
                    "orch:scope-spawn: usage (orch:scope-spawn name [body] [:kw val]…)",
                    ev.primitive_error_counter_ptr());
            }
            orch_sched.ensure(2);
            auto& scope =
                aura::orch::get_or_create_agent_scope(static_cast<void*>(&ev), *orch_sched.sched);

            auto name = heap_str_from(ev.string_heap_, a[0]);
            std::optional<std::uint64_t> cid;
            std::size_t i = 1;
            if (i < a.size() && types::is_closure(a[i])) {
                cid = types::as_closure_id(a[i]);
                ++i;
            }
            // Issue #2588: scope-spawn kw args (parallel to orch:spawn-agent).
            std::uint32_t keepalive_interval_ms = 0;
            std::uint32_t max_no_yield_ms = 0; // #2540
            for (; i + 1 < a.size(); i += 2) {
                auto k = orch_keyword_key(a[i]);
                auto& val = a[i + 1];
                if ((k == "keepalive-interval-ms" || k == "keepalive_interval_ms") &&
                    types::is_int(val)) {
                    keepalive_interval_ms =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                } else if ((k == "max-no-yield-ms" || k == "max_no_yield_ms") &&
                           types::is_int(val)) {
                    max_no_yield_ms =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                }
            }

            auto body = [&ev, cid]() {
                if (!cid)
                    return;
                try {
                    // Same per-Evaluator apply gate as orch:spawn-agent.
                    std::lock_guard lock(ev.agent_apply_mu_);
                    if (!agent_cid_live(ev, *cid)) {
                        agent_note_closure_freed_call(ev);
                        return;
                    }
                    (void)ev.apply_closure(*cid, {});
                } catch (...) {
                    // [SILENCE-PRIM-#615]: agent body errors surface via
                    // join/status only (#1669 class B intentional-state).
                }
            };
            aura::orch::AgentSpec spec;
            spec.name = name;
            spec.body = std::move(body);
            spec.attach_mailbox = true; // scope handles mailbox supervision
            spec.mailbox_high_water = 256;
            spec.keepalive_interval_ms = keepalive_interval_ms;
            spec.max_no_yield_ms = max_no_yield_ms;
            try {
                auto& handle = scope.spawn(std::move(spec));
                // #2009: handle is a reference; extract id before any potential
                // concurrent scope mutation.
                const auto id = handle.id;
                const std::string out_name = handle.name.empty() ? name : handle.name;
                const auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(out_name);
                aura::orch::g_orch_module_stats.scope_spawn_total.fetch_add(
                    1, std::memory_order_relaxed);
                std::vector<std::pair<std::string, EvalValue>> kv = {
                    {"ok", make_bool(true)},
                    {"id", make_int(static_cast<std::int64_t>(id))},
                    {"name", make_string(sidx)},
                    {"schema", make_int(2588)},
                    {"schema-2083", make_int(2083)},
                    {"schema-2161", make_int(2161)},
                    {"status",
                     [&] {
                         auto s = ev.string_heap_.size();
                         ev.string_heap_.push_back("ok");
                         return make_string(s);
                     }()},
                };
                return build_orch_hash(kv);
            } catch (...) {
                // [SILENCE-PRIM-#2588]: scope-spawn exceptions convert to
                // structured Aura error hash via OrchModuleStats bumps +
                // ok=false / status="spawn-failed" return (#1669 class B
                // intentional-state; not silent — counters + return path
                // both observable).
                aura::orch::g_orch_module_stats.scope_spawn_total.fetch_add(
                    1, std::memory_order_relaxed);
                aura::orch::g_orch_module_stats.spawn_failures.fetch_add(1,
                                                                         std::memory_order_relaxed);
                std::vector<std::pair<std::string, EvalValue>> kv = {
                    {"ok", make_bool(false)},
                    {"schema", make_int(2588)},
                    {"status",
                     [&] {
                         auto s = ev.string_heap_.size();
                         ev.string_heap_.push_back("spawn-failed");
                         return make_string(s);
                     }()},
                };
                return build_orch_hash(kv);
            }
        });

    // Issue #2631: orch:scope-child — hierarchical AgentScope surface.
    // Requires an existing per-Evaluator scope (lazy get_or_create;
    // #2588 flat per-Evaluator scope). Calls AgentScope::spawn_child()
    // on the parent; child is owned by parent, cancel_all propagates
    // top-down. Soft / sandbox=off stays observe-only (never denies).
    // Not a global registry — linter check_orch_mvp_scope.py stays green
    // (hierarchical scope bound to per-Evaluator scope map, no process-wide
    // discoverable agent map). Issue #2537 is the C++ primitive.
    // Returns hash {ok, name, schema=2631, schema=2588, schema=2537, status}.
    add("orch:scope-child", [&ev, build_orch_hash](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) {
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                "orch:scope-child: usage (orch:scope-child name [:kw val]…)",
                ev.primitive_error_counter_ptr());
        }
        orch_sched.ensure(2);
        auto& parent =
            aura::orch::get_or_create_agent_scope(static_cast<void*>(&ev), *orch_sched.sched);
        auto name = heap_str_from(ev.string_heap_, a[0]);
        try {
            (void)parent.spawn_child();
            const auto cidx = ev.string_heap_.size();
            ev.string_heap_.push_back(name);
            aura::orch::g_orch_module_stats.scope_child_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ok", make_bool(true)},
                {"name", make_string(cidx)},
                {"schema", make_int(2631)},
                {"schema-2588", make_int(2588)},
                {"schema-2537", make_int(2537)},
                {"status",
                 [&] {
                     auto s = ev.string_heap_.size();
                     ev.string_heap_.push_back("ok");
                     return make_string(s);
                 }()},
            };
            return build_orch_hash(kv);
        } catch (...) {
            // [SILENCE-PRIM-#2631]: scope-child failures surface as ok=false hash
            // (Agent-visible status); do not rethrow through prim dispatch.
            // (Mergebot already shipped this annotation in origin/main;
            // #2632 ship keeps the same wording rather than overwriting.)
            aura::orch::g_orch_module_stats.scope_child_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ok", make_bool(false)},
                {"schema", make_int(2631)},
                {"status",
                 [&] {
                     auto s = ev.string_heap_.size();
                     ev.string_heap_.push_back("scope-child-failed");
                     return make_string(s);
                 }()},
            };
            return build_orch_hash(kv);
        }
    });

    // Issue #2588 AC1 + AC4: orch:scope-watch — scope-level liveness + optional
    // RestartN. Maps to AgentScope::watch_all(stall_timeout_ms, AgentFailurePolicy).
    // Returns hash {ok, alive, stalled, cancelled, done, closed,
    //                restart-count, exhausted, schema=2588}.
    add("orch:scope-watch",
        [&ev, build_orch_hash, orch_keyword_key](std::span<const EvalValue> a) -> EvalValue {
            orch_sched.ensure(2);
            auto* scope = aura::orch::find_agent_scope(static_cast<void*>(&ev));
            if (!scope) {
                return make_primitive_error(
                    ev.string_heap_, ev.error_values_,
                    "orch:scope-watch: no scope (call orch:scope-spawn first)",
                    ev.primitive_error_counter_ptr());
            }
            aura::orch::AgentFailurePolicy policy{};
            std::uint32_t stall_ms = 0;
            for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
                auto k = orch_keyword_key(a[i]);
                auto& val = a[i + 1];
                if ((k == "stall-ms" || k == "stall_ms") && types::is_int(val)) {
                    stall_ms =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                } else if ((k == "policy" || k == "on-stall" || k == "on_stall") &&
                           types::is_string(val)) {
                    auto pol = heap_str_from(ev.string_heap_, val);
                    if (pol == "cancel")
                        policy.on_stall = aura::orch::AgentFailureAction::Cancel;
                    else if (pol == "report-only" || pol == "report_only")
                        policy.on_stall = aura::orch::AgentFailureAction::ReportOnly;
                    else if (pol == "restart-n" || pol == "restart_n")
                        policy.on_stall = aura::orch::AgentFailureAction::RestartN;
                } else if ((k == "max-restarts" || k == "max_restarts") && types::is_int(val)) {
                    policy.max_restarts =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                } else if ((k == "consecutive-stall-limit" || k == "consecutive_stall_limit") &&
                           types::is_int(val)) {
                    policy.consecutive_stall_limit =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                } else if ((k == "restart-backoff-ms" || k == "restart_backoff_ms") &&
                           types::is_int(val)) {
                    policy.restart_backoff_ms =
                        static_cast<std::uint32_t>(std::max<std::int64_t>(0, types::as_int(val)));
                }
            }
            const auto before_restart =
                aura::orch::g_orch_module_stats.agent_restart_total.load(std::memory_order_relaxed);
            const auto wr = scope->watch_all(stall_ms, policy);
            const auto after_restart =
                aura::orch::g_orch_module_stats.agent_restart_total.load(std::memory_order_relaxed);
            const auto restart_count =
                after_restart > before_restart
                    ? static_cast<std::uint64_t>(after_restart - before_restart)
                    : std::uint64_t{0};
            aura::orch::g_orch_module_stats.scope_watch_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            aura::orch::g_orch_module_stats.scope_watch_restart_count.fetch_add(
                restart_count, std::memory_order_relaxed);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ok", make_bool(true)},
                {"alive", make_int(static_cast<std::int64_t>(wr.alive))},
                {"stalled", make_int(static_cast<std::int64_t>(wr.stalled))},
                {"cancelled", make_int(static_cast<std::int64_t>(wr.cancelled))},
                {"done", make_int(static_cast<std::int64_t>(wr.done))},
                {"closed", make_int(static_cast<std::int64_t>(wr.closed))},
                {"restart-count", make_int(static_cast<std::int64_t>(restart_count))},
                {"policy",
                 [&] {
                     const char* p = "cancel";
                     switch (policy.on_stall) {
                         case aura::orch::AgentFailureAction::ReportOnly:
                             p = "report-only";
                             break;
                         case aura::orch::AgentFailureAction::RestartN:
                             p = "restart-n";
                             break;
                         default:
                             break;
                     }
                     auto s = ev.string_heap_.size();
                     ev.string_heap_.push_back(p);
                     return make_string(s);
                 }()},
                {"schema", make_int(2588)},
                {"schema-2161", make_int(2161)},
                {"schema-2229", make_int(2229)},
            };
            return build_orch_hash(kv);
        });

    // Issue #2588 AC1 + AC4: orch:scope-join-all — cancel + drain + reservation
    // release for every handle in the scope. Returns hash {ok, status,
    // wait-us, joined-count, cancelled, schema=2588}.
    add("orch:scope-join-all",
        [&ev, build_orch_hash, orch_keyword_key](std::span<const EvalValue> a) -> EvalValue {
            orch_sched.ensure(2);
            auto* scope = aura::orch::find_agent_scope(static_cast<void*>(&ev));
            if (!scope) {
                return make_primitive_error(
                    ev.string_heap_, ev.error_values_,
                    "orch:scope-join-all: no scope (call orch:scope-spawn first)",
                    ev.primitive_error_counter_ptr());
            }
            aura::orch::JoinPolicy policy{};
            policy.drain_ms = aura::orch::kDefaultJoinDrainMs;
            for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
                auto k = orch_keyword_key(a[i]);
                auto& val = a[i + 1];
                if ((k == "timeout-ms" || k == "timeout_ms") && types::is_int(val)) {
                    policy.primary_ms =
                        static_cast<std::uint64_t>(std::max<std::int64_t>(0, types::as_int(val)));
                } else if ((k == "drain-ms" || k == "drain_ms") && types::is_int(val)) {
                    policy.drain_ms =
                        static_cast<std::uint64_t>(std::max<std::int64_t>(0, types::as_int(val)));
                }
            }
            const auto jr = scope->join_all(policy);
            // ~AgentScope semantics: after join_all, scope holds no live
            // handles (drop the per-Evaluator storage slot so the next
            // scope-spawn creates a fresh scope with empty handles_).
            if (scope->empty())
                aura::orch::drop_agent_scope(static_cast<void*>(&ev));
            const char* st = "ok";
            switch (jr.status) {
                case aura::serve::JoinStatus::Ok:
                    st = "ok";
                    break;
                case aura::serve::JoinStatus::Timeout:
                    st = "timeout";
                    break;
                case aura::serve::JoinStatus::Cancelled:
                    st = "cancelled";
                    break;
                case aura::serve::JoinStatus::Invalid:
                    st = "invalid";
                    break;
                case aura::serve::JoinStatus::Reclaimed:
                    st = "reclaimed";
                    break;
            }
            const auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(st);
            aura::orch::g_orch_module_stats.scope_join_all_total.fetch_add(
                1, std::memory_order_relaxed);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ok", make_bool(jr.status == aura::serve::JoinStatus::Ok)},
                {"status", make_string(sidx)},
                {"wait-us", make_int(static_cast<std::int64_t>(jr.wait_us))},
                {"drain-ms", make_int(static_cast<std::int64_t>(policy.drain_ms))},
                {"schema", make_int(2588)},
                {"schema-2083", make_int(2083)},
                {"schema-2153", make_int(aura::orch::kJoinDrainTimeoutIssue)},
            };
            return build_orch_hash(kv);
        });

    // Issue #2588 AC1 + AC4: orch:scope-cancel-all — best-effort cancel
    // propagation across the scope's handles. Idempotent (request_cancel
    // is idempotent). Returns hash {ok, cancelled-count, schema=2588}.
    add("orch:scope-cancel-all", [&ev, build_orch_hash](std::span<const EvalValue> a) -> EvalValue {
        orch_sched.ensure(2);
        auto* scope = aura::orch::find_agent_scope(static_cast<void*>(&ev));
        if (!scope) {
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                "orch:scope-cancel-all: no scope (call orch:scope-spawn first)",
                ev.primitive_error_counter_ptr());
        }
        const auto before = scope->size();
        scope->cancel_all();
        aura::orch::g_orch_module_stats.scope_cancel_all_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"ok", make_bool(true)},
            {"cancelled-count", make_int(static_cast<std::int64_t>(before))},
            {"schema", make_int(2588)},
            {"schema-2083", make_int(2083)},
            {"schema-2161", make_int(2161)},
        };
        return build_orch_hash(kv);
    });

    // Issue #2751: orch:agent-directory — session-level Agent directory
    // surface (per-Evaluator AgentScope snapshot; NOT a process-global
    // registry). Optional filters:
    //   [:alive-only #t|#f]           (default #f)
    //   [:name-prefix "prefix"]       (default "")
    //   [:include-descendants #t|#f]  (default #t; hierarchy #2537)
    // Empty session (no scope-spawn yet) → ok=#t, agents=#(), count=0.
    // Soft / sandbox never denies. Snapshot is best-effort (AC2).
    // Returns hash {ok, agents, count, scopes-visited, schema=2751, …}.
    add("orch:agent-directory",
        [&ev, build_orch_hash, orch_keyword_key](std::span<const EvalValue> a) -> EvalValue {
            aura::orch::AgentDirectoryFilter filter{};
            for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
                auto k = orch_keyword_key(a[i]);
                auto& val = a[i + 1];
                if ((k == "alive-only" || k == "alive_only") && types::is_bool(val)) {
                    filter.alive_only = types::as_bool(val);
                } else if ((k == "include-descendants" || k == "include_descendants") &&
                           types::is_bool(val)) {
                    filter.include_descendants = types::as_bool(val);
                } else if ((k == "name-prefix" || k == "name_prefix") && types::is_string(val)) {
                    filter.name_prefix = heap_str_from(ev.string_heap_, val);
                }
            }
            aura::orch::AgentDirectorySnapshot snap;
            snap.schema = aura::orch::kAgentDirectoryIssue;
            auto* scope = aura::orch::find_agent_scope(static_cast<void*>(&ev));
            if (scope) {
                snap = scope->directory_snapshot(filter);
            }
            // Build agents vector of per-entry hashes.
            std::vector<EvalValue> agent_elems;
            agent_elems.reserve(snap.entries.size());
            for (const auto& e : snap.entries) {
                const auto nidx = ev.string_heap_.size();
                ev.string_heap_.push_back(e.name);
                const auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(e.status);
                const auto pidx = ev.string_heap_.size();
                ev.string_heap_.push_back(e.scope_path);
                std::vector<std::pair<std::string, EvalValue>> ekv = {
                    {"name", make_string(nidx)},
                    {"id", make_int(static_cast<std::int64_t>(e.id))},
                    {"status", make_string(sidx)},
                    {"scope-path", make_string(pidx)},
                    {"ok", make_bool(e.ok)},
                    {"schema", make_int(aura::orch::kAgentDirectoryIssue)},
                };
                agent_elems.push_back(build_orch_hash(ekv));
            }
            const auto vidx = ev.vector_heap_.size();
            ev.vector_heap_.push_back(std::move(agent_elems));
            aura::orch::g_orch_module_stats.agent_directory_total.fetch_add(
                1, std::memory_order_relaxed);
            aura::orch::g_orch_module_stats.agent_directory_entries_total.fetch_add(
                static_cast<std::uint64_t>(snap.entries.size()), std::memory_order_relaxed);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ok", make_bool(true)},
                {"agents", make_vector(vidx)},
                {"count", make_int(static_cast<std::int64_t>(snap.entries.size()))},
                {"scopes-visited", make_int(static_cast<std::int64_t>(snap.scopes_visited))},
                {"schema", make_int(aura::orch::kAgentDirectoryIssue)},
                {"schema-2751", make_int(aura::orch::kAgentDirectoryIssue)},
                {"schema-2588", make_int(2588)},
                {"schema-2537", make_int(aura::orch::kAgentScopeHierarchyIssue)},
                {"schema-2083", make_int(2083)},
                {"issue-2751", make_int(aura::orch::kAgentDirectoryIssue)},
            };
            return build_orch_hash(kv);
        });

    // Issue #2231 / #2538: orch:agent-ask name payload [:timeout-ms n] → hash
    // {ok, status, payload, correlation-id, schema-2231, schema-2538}.
    // Standard cross-agent request/response without a global registry
    // (#1966): C++ helper agent_ask builds a per-ask reply mailbox,
    // stamps MailKind::Ask + correlation_id (#2538 typed path) and
    // dual-writes the legacy "ask:<id>:<body>" prefix (#2231).
    add("orch:agent-ask", [&ev, build_orch_hash](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0])) {
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                "orch:agent-ask: usage (orch:agent-ask name payload [:timeout-ms n])",
                ev.primitive_error_counter_ptr());
        }
        auto name = heap_str_from(ev.string_heap_, a[0]);
        auto* hp = ev.agent_names_->find(name);
        if (!hp || !hp->ok) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:agent-ask: unknown agent",
                                        ev.primitive_error_counter_ptr());
        }
        // Coerce payload to a string (mirror orch:agent-send).
        std::string payload;
        if (types::is_string(a[1]))
            payload = heap_str_from(ev.string_heap_, a[1]);
        else if (types::is_int(a[1]))
            payload = std::to_string(types::as_int(a[1]));
        else if (types::is_bool(a[1]))
            payload = types::as_bool(a[1]) ? "#t" : "#f";
        else
            payload = "payload";
        // Default timeout 5000ms; optional 3rd arg (int ms).
        std::uint64_t timeout_ms = 5000;
        if (a.size() >= 3 && types::is_int(a[2])) {
            const auto t = types::as_int(a[2]);
            if (t > 0)
                timeout_ms = static_cast<std::uint64_t>(t);
        }
        // Delegate to the C++ helper (typed + dual-write legacy protocol).
        const auto r = aura::orch::agent_ask(*hp, payload, timeout_ms);
        // Encode the structured hash: ok / status / payload /
        // correlation-id / schema-2231 / schema-2538.
        auto st_s = r.status; // already "ok" | "timeout" | "no-mailbox" | "malformed"
        auto st_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(st_s);
        auto payload_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(r.payload);
        auto corr_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::to_string(r.correlation_id));
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"ok", make_bool(r.ok)},
            {"status", make_string(st_idx)},
            {"payload", make_string(payload_idx)},
            {"correlation-id", make_string(corr_idx)},
            {"schema", make_int(aura::orch::kAgentAskIssue)},
            {"schema-2231", make_int(aura::orch::kAgentAskIssue)},
            {"schema-2538", make_int(aura::orch::kAgentAskTypedCorrIssue)},
        };
        return build_orch_hash(kv);
    });

    // Issue #2401 / #2538: orch:agent-reply corr payload → hash
    // {ok, status, schema-2401, schema-2538}.
    // Standard worker-side response for orch:agent-ask. Corr is int or
    // string. Stamps MailKind::Reply + correlation_id (#2538) and
    // dual-writes reply:<id>: prefix. Looks up the pending-ask reply
    // mailbox (no AgentRegistry). Unknown corr / closed → structured
    // fail, no hang.
    add("orch:agent-reply", [&ev, build_orch_hash](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:agent-reply: usage (orch:agent-reply corr payload)",
                                        ev.primitive_error_counter_ptr());
        }
        std::uint64_t corr_id = 0;
        if (types::is_int(a[0])) {
            const auto v = types::as_int(a[0]);
            if (v < 0) {
                return make_primitive_error(ev.string_heap_, ev.error_values_,
                                            "orch:agent-reply: corr must be non-negative",
                                            ev.primitive_error_counter_ptr());
            }
            corr_id = static_cast<std::uint64_t>(v);
        } else if (types::is_string(a[0])) {
            const auto s = heap_str_from(ev.string_heap_, a[0]);
            try {
                corr_id = static_cast<std::uint64_t>(std::stoull(s));
            } catch (...) {
                // [SILENCE-PRIM-#615] corr parse failure is a typed primitive error.
                return make_primitive_error(ev.string_heap_, ev.error_values_,
                                            "orch:agent-reply: corr string not a number",
                                            ev.primitive_error_counter_ptr());
            }
        } else {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:agent-reply: corr must be int or string",
                                        ev.primitive_error_counter_ptr());
        }
        std::string payload;
        if (types::is_string(a[1]))
            payload = heap_str_from(ev.string_heap_, a[1]);
        else if (types::is_int(a[1]))
            payload = std::to_string(types::as_int(a[1]));
        else if (types::is_bool(a[1]))
            payload = types::as_bool(a[1]) ? "#t" : "#f";
        else
            payload = "payload";
        const auto r = aura::orch::agent_reply(corr_id, payload);
        auto st_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(r.status);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"ok", make_bool(r.ok)},
            {"status", make_string(st_idx)},
            {"schema", make_int(aura::orch::kAgentReplyIssue)},
            {"schema-2401", make_int(aura::orch::kAgentReplyIssue)},
            {"schema-2231", make_int(aura::orch::kAgentAskIssue)},
            {"schema-2538", make_int(aura::orch::kAgentAskTypedCorrIssue)},
        };
        return build_orch_hash(kv);
    });

    // Issue #2011: language surface for agent_send / agent_recv.
    add("orch:agent-send", [&ev, build_orch_hash](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0])) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:agent-send: usage (orch:agent-send name payload)",
                                        ev.primitive_error_counter_ptr());
        }
        auto name = heap_str_from(ev.string_heap_, a[0]);
        auto* hp = ev.agent_names_->find(name);
        if (!hp || !hp->ok) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:agent-send: unknown agent",
                                        ev.primitive_error_counter_ptr());
        }
        std::string payload;
        if (types::is_string(a[1]))
            payload = heap_str_from(ev.string_heap_, a[1]);
        else if (types::is_int(a[1]))
            payload = std::to_string(types::as_int(a[1]));
        else if (types::is_bool(a[1]))
            payload = types::as_bool(a[1]) ? "#t" : "#f";
        else
            payload = "payload";

        aura::serve::mf_mailbox::MailMessage msg;
        msg.payload = std::move(payload);
        msg.priority = aura::serve::mf_mailbox::MailPriority::Normal;
        auto st = aura::orch::agent_send(*hp, std::move(msg));
        const char* st_s = "ok";
        if (st == aura::serve::mf_mailbox::PushStatus::Backpressure)
            st_s = "backpressure";
        else if (st == aura::serve::mf_mailbox::PushStatus::Closed)
            st_s = "closed";
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(st_s);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"ok", make_bool(st == aura::serve::mf_mailbox::PushStatus::Ok)},
            {"status", make_string(sidx)},
            {"schema", make_int(1588)},
            {"schema-2011", make_int(2011)},
        };
        return build_orch_hash(kv);
    });

    add("orch:agent-recv",
        [&ev, build_orch_hash, orch_keyword_key](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !types::is_string(a[0])) {
                return make_primitive_error(
                    ev.string_heap_, ev.error_values_,
                    "orch:agent-recv: usage (orch:agent-recv name [:wait bool] [:timeout-ms n])",
                    ev.primitive_error_counter_ptr());
            }
            auto name = heap_str_from(ev.string_heap_, a[0]);
            auto* hp = ev.agent_names_->find(name);
            if (!hp || !hp->ok) {
                return make_primitive_error(ev.string_heap_, ev.error_values_,
                                            "orch:agent-recv: unknown agent",
                                            ev.primitive_error_counter_ptr());
            }
            bool wait = true;
            int timeout_ms = -1;
            for (std::size_t i = 1; i + 1 < a.size(); i += 2) {
                auto k = orch_keyword_key(a[i]);
                if ((k == "wait") && types::is_bool(a[i + 1])) {
                    wait = types::as_bool(a[i + 1]);
                } else if ((k == "timeout-ms" || k == "timeout_ms") && types::is_int(a[i + 1])) {
                    timeout_ms =
                        static_cast<int>(std::max<std::int64_t>(0, types::as_int(a[i + 1])));
                }
            }
            if (!wait && timeout_ms < 0)
                timeout_ms = 0;
            auto msg = aura::orch::agent_recv(*hp, wait, timeout_ms);
            if (!msg) {
                std::vector<std::pair<std::string, EvalValue>> kv = {
                    {"ok", make_bool(false)},
                    {"empty", make_bool(true)},
                    {"payload",
                     [&] {
                         auto s = ev.string_heap_.size();
                         ev.string_heap_.push_back("");
                         return make_string(s);
                     }()},
                    {"schema", make_int(1588)},
                    {"schema-2011", make_int(2011)},
                };
                return build_orch_hash(kv);
            }
            auto pidx = ev.string_heap_.size();
            ev.string_heap_.push_back(msg->payload);
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"ok", make_bool(true)},         {"empty", make_bool(false)},
                {"payload", make_string(pidx)},  {"schema", make_int(1588)},
                {"schema-2011", make_int(2011)},
            };
            return build_orch_hash(kv);
        });

    // Issue #2080: explicit progress touch for ProgressClock mode agents
    // (attach_mailbox=#f + keepalive_interval_ms > 0). Updates the shared
    // last_keepalive_us clock so watch_agent_liveness can distinguish Alive
    // vs Stalled. No-op for MailboxKeepalive (host helper owns the clock)
    // or Off (no liveness). Issue #2540: also a recommended coop poll edge.
    add("orch:agent-touch", [&ev, build_orch_hash](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:agent-touch: usage (orch:agent-touch name)",
                                        ev.primitive_error_counter_ptr());
        }
        auto name = heap_str_from(ev.string_heap_, a[0]);
        auto* hp = ev.agent_names_->find(name);
        if (!hp || !hp->ok) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:agent-touch: unknown agent",
                                        ev.primitive_error_counter_ptr());
        }
        aura::orch::note_agent_progress(*hp);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"ok", make_bool(true)},
            {"schema", make_int(1588)},
            {"schema-2008", make_int(2008)},
            {"schema-2080", make_int(2080)},
            {"schema-2540", make_int(aura::orch::kAgentMaxNoYieldIssue)},
        };
        return build_orch_hash(kv);
    });

    // Issue #2540: cooperative yield poll for long-running agent bodies.
    // (orch:agent-poll name) → hash {ok, yielded, schema-2540}.
    // Forces Fiber::yield when max_no_yield_ms window elapsed; no-op when 0.
    add("orch:agent-poll", [&ev, build_orch_hash](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:agent-poll: usage (orch:agent-poll name)",
                                        ev.primitive_error_counter_ptr());
        }
        auto name = heap_str_from(ev.string_heap_, a[0]);
        auto* hp = ev.agent_names_->find(name);
        if (!hp || !hp->ok) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:agent-poll: unknown agent",
                                        ev.primitive_error_counter_ptr());
        }
        const bool yielded = aura::orch::agent_poll(*hp);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"ok", make_bool(true)},
            {"yielded", make_bool(yielded)},
            {"schema", make_int(aura::orch::kAgentMaxNoYieldIssue)},
            {"schema-2540", make_int(aura::orch::kAgentMaxNoYieldIssue)},
        };
        return build_orch_hash(kv);
    });

    add("orch:parallel-intend", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto prim = ev.primitives_.lookup("parallel-intend");
        if (!prim) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "orch:parallel-intend: parallel-intend not registered",
                                        ev.primitive_error_counter_ptr());
        }
        return (*prim)(a);
    });

    ObservabilityPrims::register_stats_impl(
        "query:orch-module-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t spawned = 0, joined = 0, sends = 0, recvs = 0, failures = 0, batches = 0;
            aura::orch::snapshot_orch_stats(spawned, joined, sends, recvs, failures, batches);
            std::uint64_t stable_ref_refresh = 0, steal_prov = 0, linear_prev = 0;
            aura::orch::snapshot_orch_provenance_stats(stable_ref_refresh, steal_prov, linear_prev);
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            // Prefer process-wide orch counters; also surface CompilerMetrics
            // when evaluator resume/join closed loop advanced them (#1879).
            const auto cm_stable =
                m ? m->orch_stable_ref_auto_refresh_total.load(std::memory_order_relaxed) : 0;
            const auto cm_steal =
                m ? m->orch_fiber_steal_provenance_enforced_total.load(std::memory_order_relaxed)
                  : 0;
            const auto cm_lin =
                m ? m->orch_linear_violation_prevented_total.load(std::memory_order_relaxed) : 0;
            // Capacity 256: #1588 + #1879/#1880/#1881 health fields + subsequent
            // scope/directory/obs keys (#2588/#2631/#2751/…); 128 overflowed
            // (~191 insert_kv) so later schema/wired sentinels were dropped.
            auto* ht = FlatHashTable::create(256);
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
            auto& os = aura::orch::g_orch_module_stats;
            insert_kv("agents-spawned", static_cast<std::int64_t>(spawned));
            insert_kv("agents-joined", static_cast<std::int64_t>(joined));
            insert_kv("agents-send", static_cast<std::int64_t>(sends));
            insert_kv("agents-recv", static_cast<std::int64_t>(recvs));
            insert_kv("spawn-failures", static_cast<std::int64_t>(failures));
            insert_kv("parallel-batches", static_cast<std::int64_t>(batches));
            insert_kv("phase", static_cast<std::int64_t>(aura::orch::kOrchModulePhase));
            insert_kv("schema", 1588);
            // Issue #1879: StableNodeRef provenance + linear on orch paths.
            insert_kv("stable-ref-auto-refresh-total",
                      static_cast<std::int64_t>(std::max(stable_ref_refresh, cm_stable)));
            insert_kv("fiber-steal-provenance-enforced-total",
                      static_cast<std::int64_t>(std::max(steal_prov, cm_steal)));
            insert_kv("linear-violation-prevented-total",
                      static_cast<std::int64_t>(std::max(linear_prev, cm_lin)));
            insert_kv("schema-1879", 1879);
            insert_kv("provenance-mandate-wired", 1);
            // Issue #1880: ResourceQuota orch rejects (mirror process quota).
            insert_kv("resource-quota-rejects-total",
                      static_cast<std::int64_t>(
                          os.resource_quota_rejects_total.load(std::memory_order_relaxed)));
            insert_kv("agent-body-try-acquire-rejects",
                      static_cast<std::int64_t>(
                          os.agent_body_try_acquire_rejects_total.load(std::memory_order_relaxed)));
            insert_kv("agent-body-try-acquire-ok",
                      static_cast<std::int64_t>(
                          os.agent_body_try_acquire_ok_total.load(std::memory_order_relaxed)));
            insert_kv("schema-1880", 1880);
            // Issue #2118: soft mutation-boundary registration (fiber agent body).
            insert_kv("orch_agent_boundary_entered_total",
                      static_cast<std::int64_t>(
                          os.orch_agent_boundary_entered_total.load(std::memory_order_relaxed)));
            insert_kv("orch-agent-boundary-entered-total",
                      static_cast<std::int64_t>(
                          os.orch_agent_boundary_entered_total.load(std::memory_order_relaxed)));
            insert_kv("orch_agent_steal_skipped_boundary_total",
                      static_cast<std::int64_t>(os.orch_agent_steal_skipped_boundary_total.load(
                          std::memory_order_relaxed)));
            insert_kv("orch-agent-steal-skipped-boundary-total",
                      static_cast<std::int64_t>(os.orch_agent_steal_skipped_boundary_total.load(
                          std::memory_order_relaxed)));
            insert_kv("orch-agent-boundary-skip-pure-total",
                      static_cast<std::int64_t>(
                          os.orch_agent_boundary_skip_pure_total.load(std::memory_order_relaxed)));
            insert_kv("orch-agent-soft-boundary-wired", 1);
            insert_kv("schema-2118", 2118);
            insert_kv("issue-2118", 2118);
            // Issue #2163: parallel-intend :pure #t metrics.
            insert_kv("pure-parallel-batches-total",
                      static_cast<std::int64_t>(
                          os.pure_parallel_batches_total.load(std::memory_order_relaxed)));
            insert_kv("pure-parallel-tasks-total",
                      static_cast<std::int64_t>(
                          os.pure_parallel_tasks_total.load(std::memory_order_relaxed)));
            insert_kv("pure-contract-violated-total",
                      static_cast<std::int64_t>(
                          os.pure_contract_violated_total.load(std::memory_order_relaxed)));
            insert_kv("pure-fallback-locked-total",
                      static_cast<std::int64_t>(
                          os.pure_fallback_locked_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2163", 2163);
            insert_kv("issue-2163", 2163);
            // Issue #2746: parallel-intend region concurrent (#2724 integration).
            insert_kv("parallel-region-concurrent-batches-total",
                      static_cast<std::int64_t>(
                          aura::serve::parallel_orch::g_parallel_orch_stats
                              .region_concurrent_batches_total.load(std::memory_order_relaxed)));
            insert_kv("parallel-region-keys-supplied-total",
                      static_cast<std::int64_t>(
                          aura::serve::parallel_orch::g_parallel_orch_stats
                              .region_keys_supplied_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2746", 2746);
            insert_kv("issue-2746", 2746);
            // Issue #2589: unify parallel_intend residual/reclaim into the
            // orch-module-stats facade so agents/dashboards query one
            // surface for cancel-storm health. Source of truth stays
            // ParallelOrchStats (src/serve/parallel_orch.h) — facade is
            // a live read, no double-bookkeeping. Issue #2227 hard-reclaim
            // protocol shared by orch + parallel; facade mirrors the
            // parallel-side residual/reclaim/drain-us atomics so a single
            // query returns the unified cancel-storm signal.
            insert_kv("parallel-join-drain-residual-total",
                      static_cast<std::int64_t>(
                          aura::serve::parallel_orch::g_parallel_orch_stats
                              .join_drain_residual_total.load(std::memory_order_relaxed)));
            insert_kv("parallel-join-drain-residual-reclaim-total",
                      static_cast<std::int64_t>(
                          aura::serve::parallel_orch::g_parallel_orch_stats
                              .join_drain_residual_reclaim_total.load(std::memory_order_relaxed)));
            insert_kv(
                "parallel-join-drain-us-total",
                static_cast<std::int64_t>(
                    aura::serve::parallel_orch::g_parallel_orch_stats.join_drain_us_total.load(
                        std::memory_order_relaxed)));
            // Wired sentinel + source marker so dashboards can detect
            // the facade and confirm source of truth is ParallelOrchStats
            // (no mirror atomics on OrchModuleStats side).
            insert_kv("parallel-join-drain-source", static_cast<std::int64_t>(0));
            insert_kv("orch-obs-facade-unified-2589", 1);
            insert_kv("schema-2589", 2589);
            insert_kv("issue-2589", 2589);
            // Issue #1881: unified orch health (agent + mailbox + parallel mirrors).
            // New query:orch-*-stats names are frozen; fold into this hash instead.
            insert_kv("agents-active",
                      static_cast<std::int64_t>(os.agents_active.load(std::memory_order_relaxed)));
            insert_kv("quota-rejects", static_cast<std::int64_t>(
                                           os.spawn_quota_rejects.load(std::memory_order_relaxed)));
            // Issue #2155: quota-reject no-leak surface for commercial storms.
            insert_kv("spawn-quota-reject-no-leak",
                      static_cast<std::int64_t>(
                          os.spawn_quota_reject_no_leak.load(std::memory_order_relaxed)));
            insert_kv("spawn-quota-reject-no-leak-ok-total",
                      static_cast<std::int64_t>(
                          os.spawn_quota_reject_no_leak_ok_total.load(std::memory_order_relaxed)));
            insert_kv("spawn-quota-reject-leak-detect-total",
                      static_cast<std::int64_t>(
                          os.spawn_quota_reject_leak_detect_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2155", aura::orch::kSpawnQuotaNoLeakIssue);
            insert_kv("issue-2155", aura::orch::kSpawnQuotaNoLeakIssue);
            insert_kv("spawn-quota-no-leak-wired", 1);
            insert_kv("send-backpressure",
                      static_cast<std::int64_t>(
                          os.send_backpressure_total.load(std::memory_order_relaxed)));
            // Issue #2228: mailbox-backpressure admission preflight
            // (process-wide BP event counter for the spawn soft-reject
            // gate; separated from send_backpressure_total so the
            // admit preflight doesn't conflate with cumulative sends).
            insert_kv("mailbox-bp-recent-total",
                      static_cast<std::int64_t>(
                          os.mailbox_bp_recent_total.load(std::memory_order_relaxed)));
            // Issue #2228: spawn admission-reject counter — bumped
            // every time spawn_agent_with_mailbox soft-rejects a new
            // agent because the process-wide BP condition is at/above
            // the configured admit threshold. Reserved memory stays
            // 0 (no leak per #2155 parity).
            insert_kv("spawn-bp-admit-reject-total",
                      static_cast<std::int64_t>(
                          os.spawn_bp_admit_reject_total.load(std::memory_order_relaxed)));
            // Issue #2228: schema lineage (preserved by #2398).
            insert_kv("schema-2228", aura::orch::kMailboxBpAdmitIssue);
            insert_kv("issue-2228", aura::orch::kMailboxBpAdmitIssue);
            // Issue #2398: quiet-period window for mailbox_bp_recent_total
            // so BP admit recovers after storms without process restart.
            // Additive keys; #2228 residual keys intact. threshold=0 skips
            // decay work on the admit path (zero cost when gate off).
            insert_kv("mailbox-bp-window-ms",
                      static_cast<std::int64_t>(aura::orch::resolve_mailbox_bp_window_ms()));
            insert_kv("mailbox-bp-admit-threshold",
                      static_cast<std::int64_t>(aura::orch::resolve_mailbox_bp_admit_threshold()));
            // Issue #2535: production default threshold (compile-time constant)
            // so Agents can distinguish env override vs default-on mild gate.
            insert_kv("mailbox-bp-admit-threshold-default",
                      static_cast<std::int64_t>(aura::orch::kMailboxBpAdmitThresholdDefault));
            insert_kv("schema-2535", aura::orch::kMailboxBpAdmitDefaultOnIssue);
            insert_kv("issue-2535", aura::orch::kMailboxBpAdmitDefaultOnIssue);
            insert_kv("mailbox-bp-admit-default-on-wired", 1);
            insert_kv("schema-2398", aura::orch::kMailboxBpRecentWindowIssue);
            insert_kv("issue-2398", aura::orch::kMailboxBpRecentWindowIssue);
            insert_kv("mailbox-bp-decay-wired", 1);
            // Issue #2399: AgentScope concurrent misuse detection (metric path;
            // optional AURA_AGENT_SCOPE_CONCURRENT_ABORT=1 hard abort). Additive.
            insert_kv("agent-scope-concurrent-misuse-total",
                      static_cast<std::int64_t>(
                          os.agent_scope_concurrent_misuse_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2399", aura::orch::kAgentScopeConcurrentMisuseIssue);
            insert_kv("issue-2399", aura::orch::kAgentScopeConcurrentMisuseIssue);
            insert_kv("agent-scope-concurrent-detect-wired", 1);
            // Issue #2231 / #2401 / #2538: agent-ask + agent-reply metrics (additive).
            insert_kv("agent-ask-total", static_cast<std::int64_t>(
                                             os.agent_ask_total.load(std::memory_order_relaxed)));
            insert_kv("agent-ask-timeout-total",
                      static_cast<std::int64_t>(
                          os.agent_ask_timeout_total.load(std::memory_order_relaxed)));
            insert_kv("agent-reply-total", static_cast<std::int64_t>(os.agent_reply_total.load(
                                               std::memory_order_relaxed)));
            insert_kv("agent-reply-fail-total",
                      static_cast<std::int64_t>(
                          os.agent_reply_fail_total.load(std::memory_order_relaxed)));
            // Issue #2538: typed correlation (MailKind + correlation_id).
            insert_kv("agent-ask-typed-match-total",
                      static_cast<std::int64_t>(
                          os.agent_ask_typed_match_total.load(std::memory_order_relaxed)));
            insert_kv("agent-reply-typed-total",
                      static_cast<std::int64_t>(
                          os.agent_reply_typed_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2231", aura::orch::kAgentAskIssue);
            insert_kv("issue-2231", aura::orch::kAgentAskIssue);
            insert_kv("schema-2401", aura::orch::kAgentReplyIssue);
            insert_kv("issue-2401", aura::orch::kAgentReplyIssue);
            insert_kv("schema-2538", aura::orch::kAgentAskTypedCorrIssue);
            insert_kv("issue-2538", aura::orch::kAgentAskTypedCorrIssue);
            insert_kv("agent-ask-typed-corr-wired", 1);
            insert_kv("agent-reply-wired", 1);
            insert_kv("send-closed", static_cast<std::int64_t>(
                                         os.send_closed_total.load(std::memory_order_relaxed)));
            insert_kv("recv-empty", static_cast<std::int64_t>(
                                        os.recv_empty_total.load(std::memory_order_relaxed)));
            insert_kv("join-ok",
                      static_cast<std::int64_t>(os.join_ok_total.load(std::memory_order_relaxed)));
            insert_kv("join-fail", static_cast<std::int64_t>(
                                       os.join_fail_total.load(std::memory_order_relaxed)));
            insert_kv("join-wait-us-total", static_cast<std::int64_t>(os.join_wait_us_total.load(
                                                std::memory_order_relaxed)));
            const auto jn = os.join_ok_total.load(std::memory_order_relaxed) +
                            os.join_fail_total.load(std::memory_order_relaxed);
            insert_kv("avg-join-us",
                      jn > 0 ? static_cast<std::int64_t>(
                                   os.join_wait_us_total.load(std::memory_order_relaxed) / jn)
                             : 0);
            // Mailbox submodule (prefix mailbox-*)
            using namespace aura::serve::mf_mailbox;
            std::uint64_t mp = 0, mo = 0, mbc = 0, mbp = 0, ma = 0, mph = 0, mw = 0, mt = 0,
                          mlc = 0, mlv = 0, mfbp = 0;
            MultiFiberMailbox::snapshot_global_full(mp, mo, mbc, mbp, ma, mph, mw, mt, mlc, mlv,
                                                    &mfbp);
            insert_kv("mailbox-pushes", static_cast<std::int64_t>(mp));
            insert_kv("mailbox-pops", static_cast<std::int64_t>(mo));
            insert_kv("mailbox-backpressure-rejects", static_cast<std::int64_t>(mbp));
            // Issue #2010: fanout BP alias (also mirrored into send-backpressure via hook).
            insert_kv("mailbox-fanout-backpressure-rejects", static_cast<std::int64_t>(mfbp));
            insert_kv("mailbox-priority-high", static_cast<std::int64_t>(mph));
            insert_kv("mailbox-recv-waits", static_cast<std::int64_t>(mw));
            insert_kv("mailbox-linear-violations", static_cast<std::int64_t>(mlv));
            insert_kv("schema-2010", 2010);
            // Parallel submodule (prefix parallel-*)
            using namespace aura::serve::parallel_orch;
            std::uint64_t pb = 0, ps = 0, pj = 0, pok = 0, perr = 0, pff = 0, pto = 0, pmb = 0,
                          pqr = 0, pinv = 0, pbok = 0, pbpart = 0, pjw = 0, pel = 0;
            snapshot_global_ext(pb, ps, pj, pok, perr, pff, pto, pmb, pqr, pinv, pbok, pbpart, pjw,
                                pel);
            insert_kv("parallel-batches-total", static_cast<std::int64_t>(pb));
            insert_kv("parallel-tasks-ok", static_cast<std::int64_t>(pok));
            insert_kv("parallel-tasks-err", static_cast<std::int64_t>(perr));
            insert_kv("parallel-fail-fast", static_cast<std::int64_t>(pff));
            insert_kv("parallel-timeouts", static_cast<std::int64_t>(pto));
            insert_kv("parallel-batch-ok", static_cast<std::int64_t>(pbok));
            insert_kv("parallel-avg-join-us", pb > 0 ? static_cast<std::int64_t>(pjw / pb) : 0);
            // Composite health score 0..100 for AI backoff decisions.
            std::int64_t health = 100;
            if (spawned > 0) {
                const auto rej = os.spawn_quota_rejects.load(std::memory_order_relaxed) +
                                 os.resource_quota_rejects_total.load(std::memory_order_relaxed);
                const auto fail = os.join_fail_total.load(std::memory_order_relaxed);
                const auto bp = os.send_backpressure_total.load(std::memory_order_relaxed);
                const auto sp = std::max<std::uint64_t>(spawned, 1);
                const auto join_n = std::max<std::uint64_t>(jn, 1);
                const auto send_n = std::max<std::uint64_t>(sends + bp, 1);
                health = 100;
                health -= static_cast<std::int64_t>(std::min<std::uint64_t>(40, (rej * 40) / sp));
                health -=
                    static_cast<std::int64_t>(std::min<std::uint64_t>(30, (fail * 30) / join_n));
                health -=
                    static_cast<std::int64_t>(std::min<std::uint64_t>(30, (bp * 30) / send_n));
                if (health < 0)
                    health = 0;
            }
            insert_kv("health-score", health);
            insert_kv("schema-1881", 1881);
            insert_kv("agent-stats-wired", 1);    // alias for query:orch-agent-stats intent
            insert_kv("mailbox-stats-wired", 1);  // alias for query:orch-mailbox-stats intent
            insert_kv("parallel-stats-wired", 1); // alias for query:orch-parallel-stats intent
            // Issue #2008: keepalive / liveness surfaces
            insert_kv("keepalive-emitted-total",
                      static_cast<std::int64_t>(
                          os.keepalive_emitted_total.load(std::memory_order_relaxed)));
            insert_kv(
                "stalled-agents-total",
                static_cast<std::int64_t>(os.stalled_agents_total.load(std::memory_order_relaxed)));
            insert_kv("last-keepalive-us", static_cast<std::int64_t>(os.last_keepalive_us.load(
                                               std::memory_order_relaxed)));
            insert_kv("keepalive-cancels-total",
                      static_cast<std::int64_t>(
                          os.keepalive_cancels_total.load(std::memory_order_relaxed)));
            insert_kv("keepalive-helpers-spawned",
                      static_cast<std::int64_t>(
                          os.keepalive_helpers_spawned.load(std::memory_order_relaxed)));
            insert_kv("schema-2008", 2008);
            insert_kv("keepalive-wired", 1);
            // Issue #2159: fiber-native keepalive helper (no detached host thread).
            insert_kv("keepalive-helpers-joined-total",
                      static_cast<std::int64_t>(
                          os.keepalive_helpers_joined_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2159", aura::orch::kFiberNativeKeepaliveIssue);
            insert_kv("issue-2159", aura::orch::kFiberNativeKeepaliveIssue);
            insert_kv("fiber-native-keepalive-wired", 1);
            // Issue #2229: supervision policy metrics (RestartN path).
            // Bumped by AgentScope::watch_all(stall_timeout_ms,
            // AgentFailurePolicy) when on_stall == RestartN, plus
            // the circuit-like consecutive_stall_limit and the
            // exhausted-after-max-restarts signal. Reset-for-test
            // helper (reset_orch_module_stats_for_test) clears
            // these together with the other supervision counters.
            insert_kv("agent-restart-total", static_cast<std::int64_t>(os.agent_restart_total.load(
                                                 std::memory_order_relaxed)));
            insert_kv("agent-restart-exhausted-total",
                      static_cast<std::int64_t>(
                          os.agent_restart_exhausted_total.load(std::memory_order_relaxed)));
            insert_kv("agent-consecutive-stall-total",
                      static_cast<std::int64_t>(
                          os.agent_consecutive_stall_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2229", 2229);
            insert_kv("issue-2229", 2229);
            insert_kv("agent-failure-policy-wired", 1);
            // Issue #2756: workflow-level FailurePolicy composition
            // (batch + AgentScope + residual preference). Additive —
            // #2007/#2229/#2539 surfaces above preserved.
            insert_kv("workflow-compose-total",
                      static_cast<std::int64_t>(
                          os.workflow_compose_total.load(std::memory_order_relaxed)));
            insert_kv(
                "workflow-retry-total",
                static_cast<std::int64_t>(os.workflow_retry_total.load(std::memory_order_relaxed)));
            insert_kv("workflow-circuit-open-total",
                      static_cast<std::int64_t>(
                          os.workflow_circuit_open_total.load(std::memory_order_relaxed)));
            insert_kv(
                "workflow-residual-reclaim-under-policy-total",
                static_cast<std::int64_t>(os.workflow_residual_reclaim_under_policy_total.load(
                    std::memory_order_relaxed)));
            insert_kv("workflow-failure-policy-wired",
                      static_cast<std::int64_t>(
                          os.workflow_failure_policy_wired.load(std::memory_order_relaxed)));
            insert_kv("schema-2756", 2756);
            insert_kv("issue-2756", 2756);
            // Issue #2588: Aura language surface for AgentScope supervision
            // (orch:scope-spawn / orch:scope-watch / orch:scope-join-all /
            // orch:scope-cancel-all). Per-Evaluator scope map
            // (g_evaluator_agent_scopes in src/orch/agent_scope.h) is the
            // storage container; the AgentScope objects themselves are
            // per-Evaluator and do NOT become a global agent registry
            // (#1966 / scripts/coverage/checks/check_orch_mvp_scope.py still guards
            // AgentRegistry / global_agent_registry).
            insert_kv("scope-spawn-total", static_cast<std::int64_t>(os.scope_spawn_total.load(
                                               std::memory_order_relaxed)));
            insert_kv("scope-watch-total", static_cast<std::int64_t>(os.scope_watch_total.load(
                                               std::memory_order_relaxed)));
            insert_kv("scope-watch-restart-count",
                      static_cast<std::int64_t>(
                          os.scope_watch_restart_count.load(std::memory_order_relaxed)));
            insert_kv(
                "scope-join-all-total",
                static_cast<std::int64_t>(os.scope_join_all_total.load(std::memory_order_relaxed)));
            insert_kv("scope-cancel-all-total",
                      static_cast<std::int64_t>(
                          os.scope_cancel_all_total.load(std::memory_order_relaxed)));
            insert_kv("scope-dropped-total", static_cast<std::int64_t>(os.scope_dropped_total.load(
                                                 std::memory_order_relaxed)));
            insert_kv("schema-2588", 2588);
            insert_kv("issue-2588", 2588);
            insert_kv("orch-scope-wired", 1);
            // Issue #2631: hierarchical AgentScope — orch:scope-child prim
            // (parent.spawn_child() on existing per-Evaluator scope). The
            // child is owned by the parent; cancel_all propagates top-down
            // (#2537). Not a global registry — linter stays green.
            insert_kv("scope-child-total", static_cast<std::int64_t>(os.scope_child_total.load(
                                               std::memory_order_relaxed)));
            insert_kv("scope-child-wired", 1);
            insert_kv("schema-2631", 2631);
            insert_kv("issue-2631", 2631);
            // Issue #2751: session-level Agent directory
            // (orch:agent-directory). Per-Evaluator AgentScope snapshot —
            // not a process-global registry (MVP linter stays green).
            insert_kv("agent-directory-total",
                      static_cast<std::int64_t>(
                          os.agent_directory_total.load(std::memory_order_relaxed)));
            insert_kv("agent-directory-entries-total",
                      static_cast<std::int64_t>(
                          os.agent_directory_entries_total.load(std::memory_order_relaxed)));
            insert_kv("agent-directory-wired", 1);
            insert_kv("schema-2751", aura::orch::kAgentDirectoryIssue);
            insert_kv("issue-2751", aura::orch::kAgentDirectoryIssue);
            // Issue #2153: secondary drain residual / wait after non-Ok cancel.
            insert_kv("join-drain-residual-total",
                      static_cast<std::int64_t>(
                          os.join_drain_residual_total.load(std::memory_order_relaxed)));
            // Issue #2227: hard-reclaim counter — bumps every time the
            // orch join path registers a residual fiber for force-reclaim
            // (Scheduler::note_orphan_fiber). Paired with the residual
            // counter (delta = residual-reclaim ≤ residual). Reset-for-test
            // helper (reset_orch_stats) clears both together.
            insert_kv("join-drain-residual-reclaim-total",
                      static_cast<std::int64_t>(
                          os.join_drain_residual_reclaim_total.load(std::memory_order_relaxed)));
            // Issue #2743: Aura orch:agent-join status="reclaimed" outcomes.
            insert_kv("agent-join-reclaimed-total",
                      static_cast<std::int64_t>(
                          os.agent_join_reclaimed_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2743", 2743);
            insert_kv("issue-2743", 2743);
            // Issue #2397: reclaimed vs body-still-running (additive; #2227 keys preserved).
            // Prefer Fiber process-truth gauges when available so serve-only and
            // orch-linked binaries agree; orch mirrors track the same transitions.
            insert_kv(
                "join-drain-residual-still-running-total",
                static_cast<std::int64_t>(aura::serve::Fiber::join_drain_residual_still_running()));
            insert_kv("join-drain-residual-body-retired-total",
                      static_cast<std::int64_t>(
                          aura::serve::Fiber::join_drain_residual_body_retired_total()));
            insert_kv("schema-2397", aura::orch::kJoinDrainReclaimStillRunningIssue);
            insert_kv("issue-2397", aura::orch::kJoinDrainReclaimStillRunningIssue);
            insert_kv("join-drain-reclaim-still-running-wired", 1);
            // Issue #2533: residual force-safepoint / CPU budget (additive).
            insert_kv(
                "residual-force-safepoint-total",
                static_cast<std::int64_t>(aura::serve::Fiber::residual_force_safepoint_total()));
            insert_kv("residual-cpu-budget-exceeded-total",
                      static_cast<std::int64_t>(
                          aura::serve::Fiber::residual_cpu_budget_exceeded_total()));
            insert_kv("schema-2533", 2533);
            insert_kv("issue-2533", 2533);
            insert_kv("residual-force-safepoint-wired", 1);
            // Issue #2636: residual reclaim observability — body-age tracking
            // (steady_clock ns at mark_reclaimed → body exit / Fiber dtor).
            // gauge on max/sum (CAS on max under contention); counter on samples.
            // Source of truth is Fiber statics; OrchModuleStats mirrors via
            // aura_orch_note_residual_body_age_ms weak hook. force_safepoint_on_orphan_total
            // tracks the env-opt-in path explicitly (separate from #2533
            // unconditional residual_force_safepoint_total_ semantic).
            insert_kv("join-drain-residual-body-age-ms-max",
                      static_cast<std::int64_t>(
                          aura::serve::Fiber::join_drain_residual_body_age_ms_max()));
            insert_kv("join-drain-residual-body-age-ms-sum",
                      static_cast<std::int64_t>(
                          aura::serve::Fiber::join_drain_residual_body_age_ms_sum()));
            insert_kv("join-drain-residual-body-age-samples",
                      static_cast<std::int64_t>(
                          aura::serve::Fiber::join_drain_residual_body_age_samples()));
            insert_kv(
                "force-safepoint-on-orphan-total",
                static_cast<std::int64_t>(aura::serve::Fiber::force_safepoint_on_orphan_total()));
            insert_kv("force-safepoint-on-orphan-enabled",
                      static_cast<std::int64_t>(
                          aura::serve::Fiber::force_safepoint_on_orphan_enabled() ? 1 : 0));
            insert_kv("schema-2636", 2636);
            insert_kv("issue-2636", 2636);
            insert_kv("residual-body-age-wired", 1);
            insert_kv("force-safepoint-on-orphan-wired", 1);
            // Issue #2540: cooperative yield contract (max_no_yield_ms).
            insert_kv("agent-forced-yield-total",
                      static_cast<std::int64_t>(
                          os.agent_forced_yield_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2540", aura::orch::kAgentMaxNoYieldIssue);
            insert_kv("issue-2540", aura::orch::kAgentMaxNoYieldIssue);
            insert_kv("agent-max-no-yield-wired", 1);
            // Issue #2585: production default coop window — bumped once per
            // spawn that injects the 50ms default under !dev_off (opt-out via
            // AURA_AGENT_MAX_NO_YIELD_MS=0 keeps zero-cost).
            insert_kv("agent-no-yield-default-applied-total",
                      static_cast<std::int64_t>(
                          os.agent_no_yield_default_applied_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2585", 2585);
            insert_kv("issue-2585", 2585);
            insert_kv("agent-no-yield-default-applied-wired", 1);
            // Issue #2543: orch self-throttle over aot-hot-update-health.
            insert_kv("orch-hot-update-health-throttle-total",
                      static_cast<std::int64_t>(os.orch_hot_update_health_throttle_total.load(
                          std::memory_order_relaxed)));
            insert_kv("orch-hot-update-health-last-force-reason",
                      os.orch_hot_update_health_last_force_reason.load(std::memory_order_relaxed));
            insert_kv("orch-hot-update-health-checks-total",
                      static_cast<std::int64_t>(
                          aura::compiler::g_orch_hot_update_health_checks_total.load(
                              std::memory_order_relaxed)));
            insert_kv("schema-2543", aura::compiler::kAotHotUpdateHealthThrottleIssue);
            insert_kv("issue-2543", aura::compiler::kAotHotUpdateHealthThrottleIssue);
            insert_kv("orch-hot-update-health-throttle-wired", 1);
            insert_kv("join-drain-us-total", static_cast<std::int64_t>(os.join_drain_us_total.load(
                                                 std::memory_order_relaxed)));
            insert_kv("join-drain-default-ms",
                      static_cast<std::int64_t>(aura::orch::kDefaultJoinDrainMs));
            insert_kv("schema-2153", aura::orch::kJoinDrainTimeoutIssue);
            insert_kv("issue-2153", aura::orch::kJoinDrainTimeoutIssue);
            insert_kv("join-drain-wired", 1);
            // Issue #2158: per-Evaluator agent apply mutex (no process-static orch_eval_mu).
            insert_kv("agent-apply-lock-acquisitions-total",
                      static_cast<std::int64_t>(
                          os.agent_apply_lock_acquisitions_total.load(std::memory_order_relaxed)));
            insert_kv("agent-apply-lock-wait-us-total",
                      static_cast<std::int64_t>(
                          os.agent_apply_lock_wait_us_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2158", aura::orch::kAgentApplyPerEvalMutexIssue);
            insert_kv("issue-2158", aura::orch::kAgentApplyPerEvalMutexIssue);
            insert_kv("agent-apply-per-eval-mutex-wired", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
