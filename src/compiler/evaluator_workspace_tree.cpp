// evaluator_workspace_tree.cpp — P1-h: workspace tree lifecycle + policy hash
// aura.compiler.evaluator module partition. #1566: tenant isolation gates.

module;

#include "runtime_shared.h"
#include "hash_meta.h" // FNV constants (#901)
#include "observability_metrics.h"
#include "core/workspace_isolation.hh"
#include "core/self_healing_hooks.h"
#include "security_capabilities.h"
#include <cstdio>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
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

// Build a 6-key policy hash from a MemoryPolicy. Interned in string_heap_.
// Defined as a member function (not a local lambda) so it can be referenced
// from std::function-captured primitives without dangling.
EvalValue Evaluator::build_policy_hash(const MemoryPolicy& p) {
    std::vector<std::pair<std::string, EvalValue>> kv;
    kv.push_back({"auto-gc", make_bool(p.auto_gc)});
    kv.push_back({"warn-pct", make_int(p.warn_pct)});
    kv.push_back({"critical-pct", make_int(p.critical_pct)});
    kv.push_back({"sample-every", make_int(static_cast<std::int64_t>(p.sample_every))});
    kv.push_back({"cooldown-evals", make_int(static_cast<std::int64_t>(p.cooldown_evals))});
    kv.push_back(
        {"recent-gc-temp-window", make_int(static_cast<std::int64_t>(p.recent_gc_temp_window))});
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
        auto kidx = string_heap_.size();
        string_heap_.push_back(k);
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
}

EvalValue
Evaluator::build_ast_lifecycle_hash(std::span<const std::pair<std::string, EvalValue>> kv) {
    auto* ht = FlatHashTable::create(16);
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
        auto kidx = string_heap_.size();
        string_heap_.push_back(k);
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
}

// eval_in(ast::Expr*) removed — all evaluation uses eval_flat(FlatAST&) now


void* Evaluator::create_workspace_tree() {
    auto* tree = new WorkspaceTree();
    WorkspaceNode root;
    root.name = "root";
    root.is_root = true;
    root.has_own_flat = true;
    root.flat = nullptr;
    root.pool = nullptr;
    tree->nodes_.push_back(std::move(root));
    return tree;
}

void Evaluator::destroy_workspace_tree(void* wt) {
    if (!wt)
        return;
    auto* tree = static_cast<WorkspaceTree*>(wt);
    // Delete owned flats (child workspaces that had COW triggered)
    for (auto& node : tree->nodes_) {
        if (!node.is_root && node.has_own_flat) {
            delete node.flat;
            delete node.pool;
        }
    }
    delete tree;
}

// Issue #141 AC: lazy COW trigger. Called by mutate:* primitives
// before they modify workspace_flat_. If the active workspace is a
// child that still shares parent's flat, clone it now (COW) so the
// mutation doesn't pollute the parent. No-op for root, already-
// cloned, or read-only workspaces (those return false).
bool Evaluator::trigger_lazy_cow(void* wt) {
    // Issue #1566: COW clone is a mutate-side effect — enforce isolation.
    {
        using aura::compiler::security::kEffectMutate;
        if (!check_workspace_isolation(capability_tenant_id(), capability_tenant_id(),
                                       kEffectMutate, "workspace:cow")) {
            return false;
        }
    }
    if (!wt)
        return true; // no tree yet, nothing to clone
    auto* tree = static_cast<WorkspaceTree*>(wt);
    auto idx = tree->active_idx();
    if (idx == 0 || idx >= tree->size())
        return true; // root, nothing to do
    auto& node = tree->nodes_[idx];
    if (node.has_own_flat)
        return true; // already cloned
    if (node.read_only)
        return false; // can't clone read-only
    const bool cloned = tree->ensure_local_flat(idx);
    if (cloned && tree->nodes_[idx].has_own_flat) {
        auto& node = tree->nodes_[idx];
        if (node.flat) {
            node.flat->set_workspace_cow_epoch(node.cow_epoch);
            node.flat->reset_boundary_observability_counters();
        }
        // Issue #1257: auto-remap + pin COW boundary refs into the child layer.
        const auto pins_before = cow_boundary_pinned_ref_count();
        propagate_cow_pins_after_clone(idx, node.cow_epoch);
        const auto pins_after = cow_boundary_pinned_ref_count();
        const auto remapped = pins_after > pins_before ? pins_after - pins_before : 0;
        bump_stable_ref_cross_layer_validation();
        bump_stable_ref_cow_boundary_pin();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
            m->workspace_provenance_auto_remapped.fetch_add(remapped, std::memory_order_relaxed);
            m->workspace_cross_layer_validations_on_merge.fetch_add(1, std::memory_order_relaxed);
            if (remapped > 0)
                m->workspace_merge_mismatch_prevented.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (!cloned) {
        // Issue #978: budget-exceeded / refuse path — surface a diagnostic
        // so the mutation is not silently dropped (cow_refused_count alone
        // was invisible to --serve / agent callers).
        auto& refused = tree->nodes_[idx];
        if (refused.cow_refused_count > 0) {
            // Prefer stderr with fixed prefix (no logger in this TU).
            std::fprintf(stderr,
                         "[aura:cow] trigger_lazy_cow refused workspace idx=%u "
                         "(budget exceeded or read-only; cow_refused_count=%llu)\n",
                         idx, static_cast<unsigned long long>(refused.cow_refused_count));
        }
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->workspace_merge_mismatch_prevented.fetch_add(1, std::memory_order_relaxed);
    }
    return cloned;
}

// After trigger_lazy_cow, the active workspace's flat/pool may have
// been reallocated. Call this to refresh the pointers without
// exposing the WorkspaceTree type to callers defined before the type.
bool Evaluator::refresh_active_flat_pool(void* wt, void** out_flat, void** out_pool) {
    if (!wt)
        return false;
    auto* tree = static_cast<WorkspaceTree*>(wt);
    auto* node = tree->active();
    if (!node)
        return false;
    if (out_flat)
        *out_flat = node->flat;
    if (out_pool)
        *out_pool = node->pool;
    return true;
}

// ── Panic auto-rollback (Issue #39) ────────────────────────────
bool Evaluator::save_panic_checkpoint() {
    if (!workspace_flat_ || !workspace_pool_)
        return false;
    auto src_fn = primitives_.lookup("current-source");
    if (!src_fn)
        return false;
    auto src = (*src_fn)({});
    if (!types::is_string(src))
        return false;
    auto idx = types::as_string_idx(src);
    if (idx >= string_heap_.size())
        return false;
    panic_safe_source_ = string_heap_[idx];
    // Issue #242 / #1360: snapshot append-only arena sizes so
    // restore_panic_checkpoint can truncate them back (including
    // env_frames_ — append-only EnvId keeps pre-checkpoint ids valid).
    panic_safe_cells_size_ = cells_.size();
    panic_safe_pairs_size_ = pairs_.size();
    panic_safe_string_heap_size_ = string_heap_.size();
    panic_safe_env_frames_size_ = env_frames_.size();
    // Issue #1489: arm process-wide GC defer for the recovery window
    // (commit/restore releases). Protects pinned COW / StableNodeRef /
    // EnvFrame from compact_sweep while checkpoint is live.
    arm_gc_defer_for_pending_panic();
    // Issue #548: bump panic_checkpoint_save_count_ so
    // (query:panic-checkpoint-lifecycle-stats) can report
    // the lifetime save count.
    bump_panic_checkpoint_save_count();
    bump_longrunning_checkpoint_success();
    return true;
}

bool Evaluator::restore_panic_checkpoint() {
    if (panic_safe_source_.empty())
        return false;
    auto set_fn = primitives_.lookup("set-code");
    if (!set_fn)
        return false;
    auto idx = string_heap_.size();
    string_heap_.push_back(panic_safe_source_);
    auto result = (*set_fn)({make_string(idx)});
    bool ok = types::is_bool(result) && types::as_bool(result);
    // Issue #548: bump the lifecycle counters regardless of
    // success — restore attempts (failed or succeeded) count
    // toward the lifetime restore counter. Successful restores
    // additionally bump rollback_success_on_panic_.
    bump_panic_checkpoint_restore_count();
    if (ok) {
        bump_rollback_success_on_panic();
        bump_longrunning_heal_triggers();
        bump_concurrent_safety_recovery_success();
        bump_linear_postmutate_post_rollback_revalidate();
        // Issue #1582: recoverable panic restore feeds the self-heal engine.
        if (aura::core::self_heal::run_self_heal_engine(
                {.kind = "recoverable-panic",
                 .message = "panic-restore",
                 .code = static_cast<std::uint64_t>(panic_safe_cells_size_)}))
            bump_runtime_self_heal();
    }
    if (ok) {
        // Issue #242 / #1360: truncate append-only arenas back to
        // checkpoint sizes — including env_frames_ (append-only
        // EnvId: pre-checkpoint indices stay valid; post ones OOB).
        //
        // The source string we just pushed back is at idx; we
        // resize string_heap_ to idx+1 (= pre-save size + 1) so
        // the source string is preserved while everything added
        // AFTER the save (idx+1 onwards) is truncated away.
        std::size_t new_string_heap_size = idx + 1;
        if (new_string_heap_size <= string_heap_.size()) {
            string_heap_.resize(new_string_heap_size);
        }
        if (panic_safe_cells_size_ > 0 && panic_safe_cells_size_ <= cells_.size()) {
            cells_.resize(panic_safe_cells_size_);
        }
        if (panic_safe_pairs_size_ > 0 && panic_safe_pairs_size_ <= pairs_.size()) {
            pairs_.resize(panic_safe_pairs_size_);
        }
        // Issue #425: post-truncate size verification. The
        // mutation stack may have been re-entered between save
        // and restore (e.g. fiber yield + nested Guard stack),
        // in which case the cells_/pairs_ sizes recorded at
        // save time might be deeper than the current values
        // (truncating past 0 would corrupt the arena). Bump a
        // observability counter when the recorded size exceeds
        // the current size — this signals that the snapshot was
        // taken in a different transactional state than the
        // restore. The fix is a future issue (likely: capture
        // a (snapshot_id, generation) pair at save and refuse
        // restore if generation drift exceeds a threshold).
        // For now: don't truncate past current size; bump a
        // counter for observability.
        if (panic_safe_cells_size_ > cells_.size()) {
            bump_panic_checkpoint_size_mismatch();
        }
        if (panic_safe_pairs_size_ > pairs_.size()) {
            bump_panic_checkpoint_size_mismatch();
        }
        // Issue #592: post-restore arena size assert — after a
        // successful truncate the live arena sizes must match the
        // checkpoint snapshot (partial state would break hygiene /
        // reflection invariants on fiber resume).
        if (panic_safe_cells_size_ > 0 && cells_.size() != panic_safe_cells_size_) {
            bump_panic_checkpoint_size_mismatch();
        }
        if (panic_safe_pairs_size_ > 0 && pairs_.size() != panic_safe_pairs_size_) {
            bump_panic_checkpoint_size_mismatch();
        }
        // Issue #1360: actually truncate env_frames_ (was mark-only
        // INVALID_VERSION in #356 — leaked ~8MB/day under panic load).
        // truncate_env_frames_to_checkpoint also soft-marks doomed
        // frames and bumps envframe_post_rollback_invalidations_.
        (void)truncate_env_frames_to_checkpoint();
        // Clear checkpoint after successful restore
        panic_safe_source_.clear();
        panic_safe_cells_size_ = 0;
        panic_safe_pairs_size_ = 0;
        panic_safe_string_heap_size_ = 0;
        panic_safe_env_frames_size_ = 0;
        // Issue #1489: recovery complete — restore GC scheduling.
        release_gc_defer_for_pending_panic();
    }
    return ok;
}

void Evaluator::restore_panic_checkpoint_on_fiber_resume_if_needed() noexcept {
    if (!has_panic_checkpoint() || !outermost_mutation_success_flag_)
        return;
    if (*outermost_mutation_success_flag_)
        return;
    if (!panic_auto_rollback_)
        return;
    if (restore_panic_checkpoint()) {
        bump_guard_panic_reflect_restores_on_resume();
        bump_macro_hygiene_panic_restamp_from_workspace();
    }
}

void Evaluator::update_shared_tree_root() {
    if (!workspace_tree_)
        return;
    auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
    if (wt->size() > 0) {
        auto active = wt->active_idx();
        if (active < wt->size()) {
            wt->nodes_[active].flat = workspace_flat_;
            wt->nodes_[active].pool = workspace_pool_;
            if (active > 0)
                wt->nodes_[active].has_own_flat = true;
        }
    }
    // Issue #1405 Option 1: bump workspace_flat_->generation so any
    // fiber that captured the previous generation sees drift on its
    // next eval_flat check. Cheap atomic increment — no extra
    // synchronization cost (the active_node mutation above is
    // already under whatever lock owns workspace_tree_).
    //
    // MUST restamp live node_gen_ after the bump. is_valid(id)
    // requires node_gen_[id] == generation_; bump alone leaves every
    // existing NodeId stale, so (eval-current) after (workspace:create)
    // + (set-code ...) aborts at contract_assert(f->is_valid) —
    // suite/edsl_self_test synthesize:optimize path.
    //
    // The fiber-side check (Option 2 follow-up) still sees the new
    // generation() value; restamp only keeps local NodeIds live for
    // the current evaluator thread.
    if (workspace_flat_) {
        workspace_flat_->bump_generation();
        workspace_flat_->restamp_all_node_generations();
    }
}

void Evaluator::ensure_stable_ref_workspace_consistency() const noexcept {
    // Issue #424: verify WorkspaceTree layer flat/pool pointers
    // stay aligned with the evaluator's active workspace after
    // COW clones, workspace:switch, and update_shared_tree_root.
    // Issue #1566: also count isolation boundary checks (read-only).
    {
        using namespace aura::core::workspace_isolation;
        // Const path: soft check only (does not deny), keeps metrics warm.
        (void)g_workspace_isolation().check_boundary_ex(
            capability_tenant_id_, /*ref_tenant=*/0, /*required_effects=*/0,
            /*sandbox_strict=*/false, "workspace:consistency");
    }
    if (!workspace_tree_)
        return;

    auto bump_violation = [&]() noexcept {
        stable_ref_workspace_tree_violations_.fetch_add(1, std::memory_order_relaxed);
    };

    auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
    if (wt->active_idx() >= wt->size())
        bump_violation();

    const auto active = wt->active_idx();
    if (active < wt->size()) {
        const auto& node = wt->nodes_[active];
        if (node.flat != workspace_flat_)
            bump_violation();
        if (node.pool != workspace_pool_)
            bump_violation();
    }

    if (!wt->nodes_.empty()) {
        const auto& root = wt->nodes_[0];
        if (root.is_root && root.flat != nullptr && workspace_flat_ != nullptr && active == 0 &&
            root.flat != workspace_flat_) {
            bump_violation();
        }
    }

    for (std::uint32_t idx = 1; idx < wt->size(); ++idx) {
        const auto& node = wt->nodes_[idx];
        if (node.has_own_flat && node.flat == nullptr)
            bump_violation();
        if (node.has_own_flat && node.pool == nullptr)
            bump_violation();
    }
}

Env* Evaluator::copy_env(const Env& e, ast::ASTArena* target) {
    contract_assert(arena_ != nullptr);
    auto* ar = target ? target : arena_;
    return ar ? ar->create<Env>(e) : nullptr;
}

} // namespace aura::compiler
