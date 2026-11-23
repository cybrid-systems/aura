// service_ctor.cpp - CompilerService::CompilerService moved out of the module
// interface (#5242). Move-only: the body is unchanged.

module;

#include "compiler/ir_cache_boundary_restamp.hh"      // push_ir_cache_boundary_restamp
#include "compiler/messaging_bridge.h"                // g_messaging_bridge / g_mailbox_*
#include "compiler/hot_update_registry.hh"            // hot_update_registry()
#include "compiler/rebind_phase_stats.h"              // phase_stats::Scope
#include "compiler/shape_jit_pass_closedloop_stats.h" // shape_jit_pass::
#include "compiler/shape_profiler.h"                  // shape::set_shape_deopt_hook
#include "core/gc_hooks.h"                            // gc_hooks::g_arena_auto_compact_trigger
#include "core/workspace_epoch.hh"                    // current_mutation_epoch
#include "lock_order_audit.h"                         // lock_order
#include "runtime_shared.h"                           // aura_set_aot_metrics

module aura.compiler.service;

import std;

// Issue #3680: same weak probe declaration the interface carries, so
// the null-check below stays valid in this unit.
extern "C" int aura_production_defaults_active_probe() noexcept __attribute__((weak));

namespace aura::compiler {

CompilerService::CompilerService()
    : user_bindings_{"#t", "#f", "nil"}
    , session_id_("default") {
    // Issue #4682 (6b): opt-in TW native tier (off by default).
    if (tier_jit_enabled_4682_())
        evaluator_.set_tw_tier_hook_4682(&CompilerService::tw_tier_hook_thunk_4682_, this);
    // Issue #4705: the `exit` primitive ends the process with
    // std::exit(), which does NOT unwind the stack -- so this service
    // is never destroyed and the tier-JIT worker is never joined.
    // Install the quiesce hook so the exit primitive can stop+join the
    // worker before the exit phase tears LLVM's globals down.
    evaluator_.set_exit_quiesce_hook(&CompilerService::exit_quiesce_thunk_, this);
    // Issue #456: register this service's Evaluator as
    // the active query-evaluator for the current thread.
    // query:mutation-impact / query:epoch-stats /
    // query:dirty-subtree read counters from it; without
    // this registration, those primitives see nullptr
    // whenever no MutationBoundaryGuard is active.
    prev_query_evaluator_ = Evaluator::get_query_evaluator();
    Evaluator::set_query_evaluator(&evaluator_);
    evaluator_.set_arena(&arena_);
    evaluator_.set_temp_arena(&temp_arena_);
    // Issue #685/#743/#1518/#1521: ShapeProfiler + IRSoA dirty synergy on
    // compact/defrag. Prefer on_arena_compact (soft version bump + preserve
    // stability, no deopt-storm ring feed) over invalidate_all. Hard
    // invalidate_all only when throttle allows AND no profiles yet need
    // the soft path (legacy fallback when profiler empty is a no-op).
    // 500ms throttle still gates aggressive JIT cache thrash accounting.
    //
    // Issue #1666 / #3124: set_arena already installed Evaluator
    // re_pin into a compact-hook slot. Install ShapeProfiler as a
    // sibling slot (no take+chain, no heap std::function).
    {
        arena_.set_on_compact_hook(&CompilerService::on_arena_compact_hook_thunk, this);
    }
    // Issue #743 / #2438: static lambdas (no capture) load
    // g_current_compiler_service at call time. Teardown clears hooks
    // via clear_arena_compact_notify_hooks() before nulling g_current.
    aura::gc_hooks::g_arena_auto_compact_trigger.store(
        +[]() noexcept {
            auto* raw = aura::messaging::g_current_compiler_service;
            if (!raw)
                return;
            auto& m = static_cast<CompilerService*>(raw)->metrics();
            m.arena_auto_compact_trigger_total.fetch_add(1, std::memory_order_relaxed);
        },
        std::memory_order_release);
    aura::gc_hooks::g_arena_fiber_safe_compact.store(
        +[]() noexcept {
            auto* raw = aura::messaging::g_current_compiler_service;
            if (!raw)
                return;
            auto& m = static_cast<CompilerService*>(raw)->metrics();
            m.arena_live_move_yield_total.fetch_add(1, std::memory_order_relaxed);
        },
        std::memory_order_release);
    // Issue #686 / #3271: shape stability loss / invalidate → IRSoA
    // block_dirty_ via fn ptr (no capturing lambda / std::function).
    shape_profiler_.set_dirty_hook(&CompilerService::shape_dirty_hook_trampoline);
    // Issue #492: stability loss / invalidate → JIT eviction +
    // fiber refresh (closes ShapeProfiler → JIT hot-swap loop).
    shape::set_shape_deopt_hook(&CompilerService::shape_deopt_hook_trampoline);
    // Issue #744: Shape stability → Pass/JIT closed-loop probes.
    shape_jit_pass::g_mutation_epoch_fn.store(+[]() noexcept -> std::uint64_t {
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return 0;
        return static_cast<CompilerService*>(raw)->bridge_epoch();
    });
    aura::compiler::set_fn_shape_stable_probe(+[](std::string_view name) noexcept -> bool {
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return false;
        return static_cast<CompilerService*>(raw)->is_shape_stable(std::string(name));
    });
    // Issue #494 / #2823: policy + action for pass-pipeline cooperative yield.
    // Policy: true when on a fiber. Action: Fiber::yield(PassPipeline).
    aura::compiler::set_pipeline_yield_hook(&CompilerService::pipeline_yield_trampoline);
    aura::compiler::set_pipeline_fiber_yield_action(&CompilerService::pipeline_fiber_yield_action);
    evaluator_.set_type_registry(&type_registry_);
    // Issue #2148: precise meet lattice hits → CompilerMetrics
    // (also re-wired on each typecheck path with poly metrics).
    type_registry_.set_meet_precision_counter(&metrics_.meet_precision_hit_total);
    // Issue #252: wire the shared CompilerMetrics to the
    // Evaluator. apply_closure increments the closure_*
    // counters on this struct (also incremented by the
    // IR's IROpcode::Call/Apply via IRContext). This
    // gives us a single source of truth for the dispatch
    // counters, so the snapshot and closure:stats read
    // a consistent value.
    evaluator_.set_compiler_metrics(&metrics_);
    // Issue #2106: wire dirty cascade summary-skip counter into
    // CompilerMetrics so Agents see cascade_skip_subtree_total via
    // query:dirty-cascade-stats / optimization-passes-stats.
    // Exchange happens inside cascade_mark_dirty / flush_pipeline_cascade_roots.
    aura::compiler::dirty::set_cascade_skip_subtree_metrics(&metrics_.cascade_skip_subtree_total);
    // Issue #708: wire AOT bridge counters at construction so
    // aura_reload_aot_module / checkpoint probes bump metrics_
    // before the first JIT eval (register_jit_primitives is lazy).
    prev_aot_metrics_ = aura_get_aot_metrics();
    aura_set_aot_metrics(&metrics_);
    // Issue #1522: register AuraJIT for bridge C-API batch_deopt_for.
    aura_set_jit_batch_deopt_target(&jit_);
    // Issue #1540 / #1545: linear hooks. Push so ~inner restores outer.
    g_linear_hook_eval_stack.push_back(&evaluator_);
    install_linear_hooks_for_eval(&evaluator_);
    evaluator_.set_compiler_service(this);
    // Issue #681: wire mutation_epoch / bridge_epoch for
    // apply_closure + IRClosure lifetime checks.
    evaluator_.install_bridge_epoch_fn([](void* svc) -> std::uint64_t {
        return static_cast<CompilerService*>(svc)->bridge_epoch();
    });
    // Issue #1510 / #1526: compact_env_frames dual-epoch bump —
    // bridge_epoch + AOT func table (lockstep with defuse_version_
    // inside compact). Survivors then restamp Closure::bridge_epoch
    // to the new value under the same interlock.
    // Issue #3680: the bump is owner-scoped like the #3605 facade.
    // Stamp the compacting Evaluator as reemit/register owner TLS
    // (same CascadeEvalOwnerGuard shape as
    // atomic_bump_epochs_and_stamp_bridge), then freeze the process
    // clocks entirely when the facade predicate
    // (aura_aot_bump_will_be_owner_scoped) predicts an owner-scoped
    // table bump: no C-bridge dual-write, no g_aot_table_epoch
    // fetch_add, no notify — peers' dual-fresh stays green on
    // unrelated defines (#2841/#2951/#3605 tax removed from compact).
    // Soft / Off / single-eval keep the joint bump (probe false or
    // map <= 1). No second epoch domain: the skip path advances
    // nothing and compact survivors restamp to the unchanged current
    // epoch via the existing #1526 restamp (no-op value-wise);
    // defuse_version_ stays evaluator-local. Shared hook callers
    // (truncate #1739, commit_panic_checkpoint #1728) inherit the
    // same owner-scoping.
    evaluator_.install_bridge_epoch_bump_fn([](void* svc) {
        if (!svc)
            return;
        auto* s = static_cast<CompilerService*>(svc);
        struct CompactBumpOwnerGuard {
            void* prev_reemit;
            void* prev_reg;
            explicit CompactBumpOwnerGuard(void* e) noexcept
                : prev_reemit(aura_aot_get_reemit_owner_eval())
                , prev_reg(aura_aot_get_register_owner_eval()) {
                aura_aot_set_reemit_owner_eval(e);
                aura_aot_set_register_owner_eval(e);
            }
            ~CompactBumpOwnerGuard() noexcept {
                aura_aot_set_reemit_owner_eval(prev_reemit);
                aura_aot_set_register_owner_eval(prev_reg);
            }
            CompactBumpOwnerGuard(const CompactBumpOwnerGuard&) = delete;
            CompactBumpOwnerGuard& operator=(const CompactBumpOwnerGuard&) = delete;
            CompactBumpOwnerGuard(CompactBumpOwnerGuard&&) = delete;
            CompactBumpOwnerGuard& operator=(CompactBumpOwnerGuard&&) = delete;
        } owner_guard(static_cast<void*>(&s->evaluator_));
        // Issue #3680: freeze process clocks when the facade predicate
        // says the table bump would be owner-scoped anyway (production
        // multi-eval throttle armed). Probe via weak symbol with a
        // null-check so JIT-less links fail open to the joint bump.
        if (aura_production_defaults_active_probe != nullptr &&
            aura_production_defaults_active_probe() != 0 &&
            aura_aot_bump_will_be_owner_scoped() != 0)
            return;
        s->bump_bridge_epoch();
        // Dual-domain with JIT aura_closure_call (#1508 / #1524).
        aura_aot_bump_func_table_epoch();
    });
    // Issue #1526: IR runtime_closures_ env_id remap + bridge restamp.
    evaluator_.install_compact_env_remap_fn(
        [](void* ctx, const std::int64_t* remap, std::size_t n) {
            if (!ctx || !remap)
                return;
            static_cast<CompilerService*>(ctx)->remap_ir_closure_env_ids_on_compact_(remap, n);
        },
        this);
    evaluator_.install_compact_env_restamp_fn(
        [](void* ctx, std::uint64_t new_epoch) -> std::size_t {
            if (!ctx)
                return 0;
            return static_cast<CompilerService*>(ctx)->restamp_ir_closure_bridge_epochs_(new_epoch);
        });
    // Issue #682: register compiler IRClosure/EnvId GC roots at safepoint.
    evaluator_.install_compiler_gc_roots_fn([](void* svc, void* roots) {
        static_cast<CompilerService*>(svc)->flush_compiler_gc_roots(roots);
    });
    evaluator_.set_workspace_adt_sync_fn([](void* svc) {
        static_cast<CompilerService*>(svc)->refresh_adt_constructors_from_workspace();
    });
    evaluator_.set_session_id(session_id_);

    // Issue #272: route cached-define closures through persistent IR runtimes.
    install_persistent_define_closure_bridge();
    // Phase 2: EDSL IR cache V2 hooks. Let evaluator partition TUs mark
    // cached defines dirty via these std::function pointers, without
    // needing to import CompilerService (which would be circular).
    evaluator_.set_mark_define_dirty_fn(
        [this](const std::string& name) { this->mark_define_dirty(name); });
    // Issue #4678: restamp batch bracket (drain_cascade_bfs_invalidate).
    evaluator_.set_dirty_batch_fn([this](bool begin) { this->set_dirty_batch(begin); });
    evaluator_.set_boundary_prep_batch_fn([this](bool begin) {
        if (begin) {
            if (boundary_prep_depth_.fetch_add(1, std::memory_order_acq_rel) == 0) {
                boundary_prep_done_.store(false, std::memory_order_relaxed);
                boundary_finalize_done_.store(false, std::memory_order_relaxed);
                boundary_rate_sampled_.store(false, std::memory_order_relaxed);
            }
        } else if (boundary_prep_depth_.load(std::memory_order_acquire) > 0 &&
                   boundary_prep_depth_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            boundary_prep_done_.store(false, std::memory_order_relaxed);
            boundary_finalize_done_.store(false, std::memory_order_relaxed);
            boundary_rate_sampled_.store(false, std::memory_order_relaxed);
        }
    });
    // Issue #3681: the id captured before set-body / rebind is the IR
    // define closure. Dropping the owner entry makes dispatch return
    // nullopt; the env refresh below installs a fresh closure.
    evaluator_.set_drop_ir_define_binding_fn(
        [this](const std::string& name) { this->clear_ir_define_env_binding(name); });
    // Issue #4349 / #4753: ast:restore is a workspace REPLACE. Its cone
    // invalidate retires the pre-replace publication unconditionally -
    // the #4753 deferral (cell still advertises the id) is for a
    // supersede-by-new-publication inside one workspace only, and here
    // the caller must not observe the replaced workspace's body (#4349).
    evaluator_.set_invalidate_ir_define_binding_fn(
        [this](const std::string& name) { this->invalidate_ir_define_binding_for_replace(name); });
    // Issue #4753: a define's dropped tree-walker generation stays
    // resolvable to the define's current publication.
    evaluator_.set_note_retired_define_closure_fn(
        [this](std::uint64_t cid, const std::string& name) {
            this->note_retired_tw_closure_(cid, name);
        });
    evaluator_.set_mark_all_defines_dirty_fn([this]() { this->mark_all_defines_dirty(); });
    // Issue #3033: dual-topology abort → force-dirty + zero-restamp every
    // IR cache entry so should_relower is forced true (abort path leaves
    // version stamps pointing at intermediate/pre-abort state).
    evaluator_.set_abort_ir_cache_begin_force_fn(
        [this]() { this->begin_abort_ir_cache_force_fence(); });
    evaluator_.set_abort_ir_cache_force_dirty_fn(
        [this]() { this->force_ir_cache_dirty_after_abort(); });
    // Issue #3985: Phase-5 densify success invalidates source_to_ir_map
    // + content latch (abort path stays force_ir_cache_dirty_after_abort).
    evaluator_.set_densify_ir_cache_map_invalid_fn(
        [this]() { this->force_ir_cache_map_invalid_after_densify(); });
    // Issue #2730: rebind/set-body publish new source before dirty cascade.
    evaluator_.set_update_function_source_fn(
        [this](const std::string& name, const std::string& src) {
            // Issue #4598: ast:restore passes "" for a define the
            // snapshot dropped. Assigning that kept a stale row.
            if (src.empty())
                this->function_sources_.erase(name);
            else
                this->function_sources_[name] = src;
        });
    // Issue #680: precise IR/JIT/bridge invalidation for closure-heavy Defines.
    evaluator_.set_invalidate_function_fn(
        [this](const std::string& name) { this->invalidate_function(name); });
    evaluator_.set_define_impact_scope_fn(
        [this](aura::ast::NodeId root) { this->run_define_impact_scope(root); });
    // Issue #657: compiler-core incremental hooks (lowering bridge epoch +
    // linear metadata + JIT unhandled invalidate).
    install_lowering_compiler_core_hooks();
    aura_set_jit_unhandled_invalidate_fn(&CompilerService::jit_unhandled_invalidate_trampoline);
    // Issue #3373: production CompilerService wires the candidate
    // iterator so aura_reemit_aot_for_dirty sees the dirty set under
    // production. Soft / Off: aura_install_production_dirty_iterator
    // is a no-op (returns 0) so reemit_provider_wired==0 stays zero-cost
    // and the Phase-1 skeleton path remains in effect (#3373 AC3).
    // Test fixtures may overwrite via aura_set_reemit_candidate_fn
    // after construction; the install is idempotent on the registry
    // wired bit. Single-workspace MVP scope; no BFS / cross-COW.
    (void)aura_install_production_dirty_iterator();
    // Phase 2: pre-populate v2 IR cache from workspace defines.
    // Called from (set-code ...) primitive after a successful parse.
    // Plan A + Follow-up 3: hook calls BOTH the lightweight
    // populate_dep_graph (no side effects) AND the heavy
    // populate_ir_cache_v2 (which uses bind_in_env=false to skip
    // the env pollution that broke tests in early Phase 3).
    //
    // Issue #687: dep-graph doubling is now prevented via
    // record_dependency idempotence. The multi-define binding
    // issue (a/b/c not bound in top_ after eval-current) is
    // fixed by eval-current explicitly syncing workspace
    // defines into top_env after eval_flat (see eval-current
    // implementation). The heavy populate stays as opt-in.
    evaluator_.set_pre_cache_workspace_defines_fn([this]() {
        this->populate_dep_graph_from_workspace();
        this->populate_ir_cache_v2_from_workspace();
    });
    // Issue #4377: boundary exit acknowledges defuse on in-boundary stores.
    push_ir_cache_boundary_restamp(&CompilerService::restamp_boundary_stored_trampoline, this);
    // Issue #2579: after tree-walker eval-current, record value-define
    // cell ids for IR Variable lowering without re-binding env.
    evaluator_.set_sync_workspace_value_cells_fn(
        [this]() { this->sync_workspace_value_cells_from_env(); });
    // Issue #63723: lightweight dep_graph-only repopulate hook
    // (called from mutate:rebind / mutate:set-body after the
    // rebind success). mutate:rebind does NOT call
    // invalidate_function (it uses mark_define_dirty + the
    // dep_graph BFS cascade), so the IR cache for `name` is
    // just marked dirty and re-lower happens lazily on the
    // next (eval-current). The dep_graph however must be
    // repopulated eagerly so that subsequent
    // public_invalidate_function(name) sees the same caller
    // edges the original (set-code ...) recorded — otherwise
    // the BFS cascade finds no dependents and silently
    // evicts only the mutated function itself. This is the
    // dep_graph integrity contract test_issue_401 AC5
    // verifies. Skips populate_ir_cache_v2 (heavy lower)
    // because the lazy re-lower on next (eval-current) is
    // sufficient and avoids an O(n^2) cost on rebind
    // storms.
    evaluator_.set_repopulate_workspace_dep_graph_fn(
        [this]() { this->populate_dep_graph_from_workspace(); });
    // Issue #4682 step 2: per-define repopulate for a single rebind.
    evaluator_.set_repopulate_dep_graph_one_fn_4682(
        [this](const std::string& name, std::uint64_t log_before) {
            this->populate_dep_graph_one_4682_(name, log_before);
        });
    // Issue #4793: re-walk only the changed defines (stamp path).
    evaluator_.set_restore_dep_graph_fns(
        [this]() { return this->restore_dep_graph_synced_(); },
        [this](const std::vector<std::string>& names) { this->restore_dep_graph_names_(names); },
        [this]() { this->restore_dep_graph_advance_(); });
    // Issue #1495 / #2813: lazy partial re-lower of dirty defines on
    // (eval-current) + push_post_mutate_incremental_cascade under Guard.
    // Production requirement: this MUST be wired. Leaving it null
    // causes cascade_relower_skipped_total + one-shot stderr (#2813)
    // when mutate:* marks defines dirty (stale ir_cache_v2 risk).
    // Consumes body-only dirty bitmask from mark_define_dirty without
    // requiring a set-code pre-cache.
    evaluator_.set_relower_dirty_defines_fn([this]() {
        aura::compiler::phase_stats::Scope phase_relower(aura::compiler::phase_stats::Relower);
        (void)this->relower_dirty_defines_from_workspace();
    });
    // Issue #4357: hot workspace lambdas leave the tree walker.
    evaluator_.set_bind_workspace_lambda_via_ir_fn(
        [this](const std::string& name) { this->specialize_workspace_lambdas_via_ir(name); });
    // Phase 3 debugging: expose is_define_dirty + get_dependents.
    evaluator_.set_is_define_dirty_fn([this](const std::string& name) -> bool {
        const auto* entry = this->get_define_v2(name);
        if (!entry)
            return false;
        return entry->dirty;
    });
    evaluator_.set_slot_edge_skip_fn(
        [this](const std::string& callee, const std::string& caller) -> bool {
            return slot_edge_skippable_4681_(callee, caller, /*callee_changed=*/true);
        });
    evaluator_.set_get_dependents_fn([this](const std::string& name) -> std::vector<std::string> {
        std::shared_lock dep_read(dep_graph_mtx_);
        auto it = dep_graph_.find(name);
        if (it == dep_graph_.end())
            return {};
        return it->second.called_by;
    });
    // Phase 4: hook for (eval-current :jit). Re-runs the workspace
    // through eval_ir (which has the JIT pipeline) and returns the
    // result. Falls back to the IR interpreter if LLVM isn't available
    // or the JIT compile fails.
    evaluator_.set_try_jit_fn([this](const std::string& source) -> std::optional<types::EvalValue> {
        auto result = this->eval_ir(source);
        if (!result)
            return std::nullopt;
        return *result;
    });
    // Phase 4 / Issue #2922: workspace source via unparse_to_string
    // (SSOT with (current-source :workspace); no Evaluator re-entry).
    evaluator_.set_get_workspace_source_fn([this]() -> std::string {
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (!ws_flat || !ws_pool)
            return "";
        return aura::ast::unparse_to_string(*ws_flat, *ws_pool, ws_flat->root, {});
    });
    // Issue #194: hook to query the runtime→intrinsic migration
    // counter from the AuraJIT. Used by the (jit:intrinsic-count)
    // Aura-level primitive. Returns 0 if no JIT is attached
    // (e.g. --no-llvm build, unit-test Evaluator).
    evaluator_.set_get_intrinsic_count_fn([this]() -> std::uint64_t {
        // The Metrics struct on AuraJIT includes
        // intrinsic_count (the per-lowering migration counter
        // shipped in 9901a91). Access via Metrics() — returns
        // a fresh struct snapshot, so reads are atomic.
        // The intrinsic_count field is std::atomic<uint64_t>.
        return jit_.metrics().intrinsic_count.load(std::memory_order_relaxed);
    });
    // Issue #193: hook to query the per-function
    // unhandled-opcode count. Used by the (query :deopt-fn?)
    // primitive (Issue #193 follow-up).
    evaluator_.set_get_jit_unhandled_count_fn([this](const char* name) -> std::uint64_t {
        return jit_.unhandled_opcode_count_for_function(name);
    });
    // Issue #427: hook for (query:jit-stats) — returns the
    // full JIT metrics line (same string AuraJIT::Metrics::format
    // produces). The closure formats into a thread-local
    // buffer to avoid lifetime issues across the std::function
    // boundary. Returns "" if no JIT is attached.
    evaluator_.set_get_jit_stats_fn([this]() -> const char* {
        // Issue #1917: format line grew (critical opcode + PrimCall keys).
        thread_local char buf[1536];
        // Always format — even a no-JIT build produces a
        // valid string with all-zero counters. The primitive
        // (query:jit-stats) just returns it as-is. Saves
        // us from having to plumb a "no jit" flag through
        // the service.
        jit_.metrics().format(buf, sizeof(buf));
        return buf;
    });
    // Issue #196: hook to query the incremental-compilation
    // observability struct. Used by the (compile:cache-size),
    // (compile:dirty-count), (compile:epoch), (compile:dep-edges)
    // primitives.
    evaluator_.set_get_incremental_stats_fn([this]() -> std::uint64_t {
        // Four 16-bit lanes: cache, dirty, epoch, edges. The word
        // cannot grow past uint64.
        // Issue #4395: Mutation epoch is a uint64. Shifting it raw
        // ORs bits 16-31 into dirty-count and bits 32-47 into
        // cache-size. The epoch lane is 0xFFFF when the value does
        // not fit (overflow sentinel, not a sibling alias).
        // compile:epoch and query:jit-stats-hash publish
        // current_mutation_epoch() in full.
        std::uint64_t cache_size = static_cast<std::uint64_t>(ir_cache_v2_.size());
        std::uint64_t dirty_count = 0;
        for (auto& [_, e] : ir_cache_v2_) {
            if (e.dirty)
                ++dirty_count;
        }
        const std::uint64_t epoch = aura::core::current_mutation_epoch();
        std::uint64_t edges = 0;
        {
            std::shared_lock dep_read(dep_graph_mtx_);
            for (auto& [_, dep_entry] : dep_graph_) {
                edges += static_cast<std::uint64_t>(dep_entry.calls.size());
                edges += static_cast<std::uint64_t>(dep_entry.called_by.size());
            }
        }
        constexpr std::uint64_t kLane = 0xFFFFu;
        const std::uint64_t epoch_lane = epoch > kLane ? kLane : epoch;
        return (cache_size << 48) | (dirty_count << 32) | (epoch_lane << 16) | (edges & kLane);
    });
    // Issue #196: per-block dirty hooks. Mirror the
    // get_incremental_stats hook pattern: stateless
    // lambdas that read the current state of
    // ir_cache_v2_ on demand. No caching at the
    // hook layer — the underlying reads are O(1) or
    // O(num_functions_in_entry) which is also small
    // (typically <10 functions per define).
    // Issue #2684: empty / null name → total dirty blocks across
    // ir_cache_v2_ so (compile:block-dirty-count) zero-arg works for
    // denseness hosts that don't know the rebound define name yet.
    evaluator_.set_get_dirty_block_count_fn([this](const char* name) -> std::uint64_t {
        if (!name || !*name)
            return static_cast<std::uint64_t>(total_dirty_block_count());
        std::string n(name);
        auto it = ir_cache_v2_.find(n);
        if (it == ir_cache_v2_.end())
            return 0;
        return static_cast<std::uint64_t>(it->second.dirty_block_count());
    });
    evaluator_.set_get_func_dirty_block_count_fn(
        [this](const char* name, std::size_t func_idx) -> std::uint64_t {
            std::string n = name ? std::string(name) : std::string();
            auto it = ir_cache_v2_.find(n);
            if (it == ir_cache_v2_.end())
                return 0;
            return static_cast<std::uint64_t>(it->second.func_dirty_block_count(func_idx));
        });
    evaluator_.set_is_block_dirty_fn(
        [this](const char* name, std::size_t func_idx, std::uint32_t block_idx) -> bool {
            std::string n = name ? std::string(name) : std::string();
            auto it = ir_cache_v2_.find(n);
            if (it == ir_cache_v2_.end())
                return false;
            return it->second.is_block_dirty(func_idx, block_idx);
        });
    evaluator_.set_mark_block_dirty_fn(
        [this](const char* name, std::size_t func_idx, std::uint32_t block_idx) -> bool {
            std::string n = name ? std::string(name) : std::string();
            return mark_block_dirty_v2(n, func_idx, block_idx);
        });
    evaluator_.set_clear_block_dirty_fn(
        [this](const char* name, std::size_t func_idx, std::uint32_t block_idx) -> bool {
            std::string n = name ? std::string(name) : std::string();
            return clear_block_dirty_v2(n, func_idx, block_idx);
        });
    // Issue #429: hook for (query:soa-dirty-stats).
    // The closure reads the live SoaDirtyStats in one
    // pass over ir_cache_v2_ (cheap; ~1ns per entry).
    evaluator_.set_get_soa_dirty_stats_fn(
        [this]() -> Evaluator::SoaDirtyStats { return get_soa_dirty_stats(); });
    // Issue #460: per-instruction dirty hooks. The
    // P0 ship wires no-op implementations (always
    // return false) because the underlying
    // ir_cache_v2_ doesn't have an instruction-level
    // dirty bitmask yet. The hooks are exposed so
    // the primitive path is exercisable; the
    // follow-up adds the bitmask and the
    // mark/clear implementations.
    evaluator_.set_is_instruction_dirty_fn([this](const char* name, std::size_t func_idx,
                                                  std::uint32_t block_idx,
                                                  std::uint32_t inst_idx) -> bool {
        std::string n = name ? std::string(name) : std::string();
        auto it = ir_cache_v2_.find(n);
        if (it == ir_cache_v2_.end())
            return false;
        return it->second.is_instruction_dirty(func_idx, block_idx, inst_idx);
    });
    evaluator_.set_mark_instruction_dirty_fn([this](const char* name, std::size_t func_idx,
                                                    std::uint32_t block_idx,
                                                    std::uint32_t inst_idx) -> bool {
        std::string n = name ? std::string(name) : std::string();
        auto it = ir_cache_v2_.find(n);
        if (it == ir_cache_v2_.end())
            return false;
        it->second.mark_instruction_dirty(func_idx, block_idx, inst_idx);
        return true;
    });
    evaluator_.set_clear_instruction_dirty_fn([this](const char* name, std::size_t func_idx,
                                                     std::uint32_t block_idx,
                                                     std::uint32_t inst_idx) -> bool {
        std::string n = name ? std::string(name) : std::string();
        auto it = ir_cache_v2_.find(n);
        if (it == ir_cache_v2_.end())
            return false;
        it->second.clear_instruction_dirty(func_idx, block_idx, inst_idx);
        return true;
    });
    // Issue #240: per-node occurrence-dirty hook. The
    // hook reads / writes the kOccurrenceDirty bit on
    // the workspace FlatAST's dirty bitmask
    // (DirtyReason::kOccurrenceDirty = 0x04). The bit
    // gates find_occurrence_contexts in
    // type_checker_impl.cpp so the post-mutation
    // invariant check scopes its diagnostic to
    // contexts where narrowing is genuinely stale.
    //
    // The set hook returns the PRIOR bit state (true if
    // it was already set, false otherwise).
    // Issue #1779: read-only (compile:narrowing-dirty?) uses
    // query_occurrence_dirty_fn_ — never set+restore (race).
    evaluator_.set_set_occurrence_dirty_fn([this](std::uint32_t node_id, bool set) -> bool {
        auto* ws = evaluator_.workspace_flat();
        if (!ws || node_id >= ws->size())
            return false;
        bool prior = ws->is_dirty_for(
            node_id, static_cast<std::uint8_t>(aura::ast::FlatAST::DirtyReason::kOccurrenceDirty));
        if (set) {
            ws->mark_dirty(node_id, static_cast<std::uint8_t>(
                                        aura::ast::FlatAST::DirtyReason::kOccurrenceDirty));
            // Issue #518: mirror kOccurrenceDirty into the
            // occurrence-stale column for observability
            // (query:occurrence-stale-count) and re-narrow.
            ws->mark_occurrence_stale(node_id);
        } else {
            ws->clear_dirty_for(node_id, static_cast<std::uint8_t>(
                                             aura::ast::FlatAST::DirtyReason::kOccurrenceDirty));
        }
        return prior;
    });
    evaluator_.set_query_occurrence_dirty_fn([this](std::uint32_t node_id) -> bool {
        auto* ws = evaluator_.workspace_flat();
        if (!ws || node_id >= ws->size())
            return false;
        return ws->is_dirty_for(
            node_id, static_cast<std::uint8_t>(aura::ast::FlatAST::DirtyReason::kOccurrenceDirty));
    });
    // Issue #197: hook for (compile:inline-pass-stats).
    // The hook reads the static lifetime counters
    // maintained by InlinePass (process-wide totals).
    // If the InlinePass hasn't been run yet, both
    // counters are 0.
    // Issue #1827: total_* getters are acquire-loads on
    // atomics (writers use fetch_add relaxed).
    evaluator_.set_get_inline_stats_fn([]() -> std::uint64_t {
        // Issue #1784: pack as two uint32 halves (low=inlined,
        // high=branch_aware). Consumers unpack via uint32_t.
        const std::uint32_t inlined_u32 =
            static_cast<std::uint32_t>(aura::compiler::InlinePass::total_inlined() & 0xFFFFFFFFu);
        const std::uint32_t branch_aware_u32 = static_cast<std::uint32_t>(
            aura::compiler::InlinePass::total_inlined_branch_aware() & 0xFFFFFFFFu);
        return (static_cast<std::uint64_t>(branch_aware_u32) << 32) |
               static_cast<std::uint64_t>(inlined_u32);
    });
    // Issue #388: macro-hygiene skipped total (separate
    // getter so the packed uint64 layout stays unchanged).
    evaluator_.set_get_macro_hygiene_skipped_fn([]() -> std::uint64_t {
        return static_cast<std::uint64_t>(
            aura::compiler::InlinePass::total_macro_hygiene_skipped());
    });
    // Issue #1420 AC3: hook for (compile:bidirectional-stats).
    // Reads the 4 CompilerMetrics counters incremented
    // by InferenceEngine::check_flat_call and packs
    // them into a single uint64. Bit layout (see
    // Evaluator::get_bidirectional_stats_fn_ comment):
    //   bits  0-23: compile_bidirectional_check_call_total
    //   bits 24-39: compile_bidirectional_annotation_pass_total
    //   bits 40-55: compile_bidirectional_annotation_fail_total
    //   bits 56-63: compile_bidirectional_coercion_deferred_total
    // Mode (full / disabled) is read separately via
    // CompilerService::bidirectional_mode() because
    // the persistent CompilerService flag is the
    // authoritative source (per-call TypeChecker
    // instances are short-lived and pick up the flag
    // via tc.set_bidirectional_mode(bidirectional_mode_)
    // at construction).
    evaluator_.set_get_bidirectional_stats_fn([this]() -> std::uint64_t {
        auto& m = this->metrics_;
        std::uint64_t check_call =
            m.compile_bidirectional_check_call_total.load(std::memory_order_relaxed);
        std::uint64_t pass =
            m.compile_bidirectional_annotation_pass_total.load(std::memory_order_relaxed);
        std::uint64_t fail =
            m.compile_bidirectional_annotation_fail_total.load(std::memory_order_relaxed);
        std::uint64_t coercion =
            m.compile_bidirectional_coercion_deferred_total.load(std::memory_order_relaxed);
        return ((coercion & 0xFF) << 56) | ((fail & 0xFFFF) << 40) | ((pass & 0xFFFF) << 24) |
               (check_call & 0xFFFFFF);
    });
    prev_compiler_service_ = aura::messaging::g_current_compiler_service;
    aura::messaging::g_current_compiler_service = this;
    // Issue #2100: register deopt→AST MacroIntroduced restore hook so
    // aura_jit_macro_introduced_deopt_inc can re-stamp workspace markers.
    aura_jit_set_macro_deopt_restore_fn(+[]() noexcept -> std::uint64_t {
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return 0;
        return static_cast<CompilerService*>(raw)->restore_macro_introduced_from_ir_after_deopt();
    });
    // Setup messaging bridge (avoids circular module dependency)
    aura::messaging::g_messaging_bridge.send = [](const std::string& target,
                                                  const std::string& msg) -> bool {
        auto* svc = CompilerService::lookup(target);
        if (!svc)
            return false;
        auto* self = static_cast<CompilerService*>(aura::messaging::g_current_compiler_service);
        auto sender = self ? self->session_id() : std::string("(unknown)");
        svc->push_message(sender, msg);
        return true;
    };
    aura::messaging::g_messaging_bridge.recv = [](int timeout_ms) -> std::optional<std::string> {
        // This relies on the compiler_service_ being set correctly,
        // which doesn't work across sessions. Instead, use the
        // current CompilerService from context.
        // For now, return empty — recv is session-specific.
        return std::nullopt;
    };
    aura::messaging::g_messaging_bridge.my_id = []() -> std::string { return "(unknown)"; };
    // Set per-service access functions
    // Arena reset callback for benchmark task cleanup
    aura::messaging::g_reset_arena = [](void* svc) {
        if (!svc)
            return;
        static_cast<CompilerService*>(svc)->reset();
    };

    aura::messaging::g_mailbox_read = [](void* svc, int timeout_ms) -> std::optional<std::string> {
        if (!svc)
            return std::nullopt;
        return static_cast<CompilerService*>(svc)->pop_message(timeout_ms);
    };
    aura::messaging::g_mailbox_last_sender = [](void* svc) -> std::string {
        if (!svc)
            return "";
        return static_cast<CompilerService*>(svc)->last_sender();
    };
    aura::messaging::g_mailbox_count = [](void* svc) -> std::size_t {
        if (!svc)
            return 0;
        return static_cast<CompilerService*>(svc)->mailbox_size();
    };
    aura::messaging::g_session_id = [](void* svc) -> std::string {
        if (!svc)
            return "";
        return static_cast<CompilerService*>(svc)->session_id();
    };
    aura::messaging::g_session_exists = [](const std::string& id) -> bool {
        return CompilerService::lookup(id) != nullptr;
    };
    // Cache module defines in IR after each import (incl. recursive fns)
    evaluator_.set_module_loaded_callback(
        [this](const std::string& content, const std::string& path,
               aura::compiler::Env* module_env) {
            cache_module(content, path, module_env);
        });

    // Issue #97 Action 1: hot-swap callback. Allows (mutate :hot-swap-fn "name" "new-src")
    // primitive to replace a function's body while keeping its id.
    evaluator_.set_hot_swap_fn([this](const std::string& name, const std::string& new_source) {
        return hot_swap_function_impl(name, new_source);
    });

    // Issue #2301 / #2370: wire SpecJIT as a deopt-storm listener.
    // HotUpdateRegistry::notify_deopt_storm_locked fans out
    // to storm_listeners_ while eval_mutex_ is held (the
    // notify_deopt_storm path lives on the eval thread under
    // the same higher-level lock). The lambda captures
    // `this` and calls spec_jit_.on_deopt_storm() which
    // clear()s the spec cache + bumps the per-controller +
    // process-global storm-clear counters. Under PerEval
    // isolation (#2370) the clear is no-op for foreign evals
    // (TLS storm eval context + eval_owner match).
    // Issue #2370: bind SpecJIT to this CompilerService as owner.
    spec_jit_.set_eval_owner(static_cast<void*>(this));
    storm_listener_id_ = hot_update_registry().register_storm_listener(
        [this](std::uint64_t /*deopts_in_window*/, std::uint64_t /*window_ms*/) noexcept {
            // Publish TLS eval context for PerEval storm isolation (#2370).
            aura_set_storm_eval_context(static_cast<void*>(this));
            spec_jit_.on_deopt_storm();
        });
}

} // namespace aura::compiler
