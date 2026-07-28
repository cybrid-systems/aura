// shape_profiler.cpp — Shape profiling implementation
//
// Phase 1: Shape infrastructure (#53 Shape-based Speculative JIT)
//
// NOT a C++ module — uses traditional header to avoid C++ module
// recursive type issues.
//
#include "shape_profiler.h"
#include "shape_jit_pass_closedloop_stats.h"
#include "value_tags.h"
#include "core/cpp26_contract_stats.h"
#include "core/arena_auto_policy_stats.h" // Issue #1621: shape churn → arena policy
#include "core/workspace_epoch.hh" // Issue #1964 cycle 2b: aura::core::current_mutation_epoch()
#include <algorithm>
#include <contracts>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include "hash_meta.h" // FNV constants (#901)

// We need EvalValue tag helpers. Since value is a C++ module,
// include the relevant inline functions directly (they're constexpr/header-only style).
// The actual EvalValue struct and tag helpers are inline in value.ixx.
// To avoid module dependency, this file only uses the shape.h types.
// The shape_of function is implemented below using raw integer bit tests.
namespace aura::compiler::shape {

// Issue #2256: process-wide Moving-compact observability for pure unit
// tests. shape_profiler is intentionally a non-module TU and cannot
// import aura.core.lifetime_pin; module-inline atomics are not linkable
// via plain extern. Names match lifetime_pin.ixx counters for source-cite
// (scripts/check_arena_moving_compaction_coverage.py / test_moving_compact_2166).
namespace {
    std::atomic<std::uint64_t> g_moving_compact_count_total{0};
    std::atomic<std::uint64_t> g_moving_compact_remap_us_total{0};
} // namespace

namespace {

    ShapeDeoptHook g_shape_deopt_hook = nullptr;

    void fire_shape_deopt_hook(FnKey fn, std::uint64_t version,
                               std::uint32_t dirty_scope) noexcept {
        shape_deopt_hook_fire_count.fetch_add(1, std::memory_order_relaxed);
        if (dirty_scope != 0) {
            shape_dirty_hook_fire_count.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_shape_deopt_hook)
            g_shape_deopt_hook(fn, version, dirty_scope);
    }

    ShapeID finish_inline_shape_id(ShapeID id) noexcept {
        contract_assert(is_known_inline_shape_id(id));
        return id;
    }

} // namespace

void set_shape_deopt_hook(ShapeDeoptHook hook) noexcept {
    g_shape_deopt_hook = hook;
}

ShapeDeoptHook shape_deopt_hook() noexcept {
    return g_shape_deopt_hook;
}

// ═══════════════════════════════════════════════════════════════
// shape_of — fast shape classification from tagged int64_t bits
// ═══════════════════════════════════════════════════════════════
//
// Issue #571: uses classify_eval_value_tag() from value_tags.h
// (consteval low-2-bit table + v2 string range check). Contracts
// guard the hot path in debug builds (zero release cost).
//
// Timing: ~2-5ns (bit ops + table dispatch, no heap access)

ShapeID inline_shape_of(std::int64_t val) {
    aura::core::cpp26::record_hotpath_invariant_hit();
    using aura::compiler::types::classify_eval_value_tag_consteval;
    using aura::compiler::types::EvalValueTag;
    using aura::compiler::types::note_value_tag_stability;
    using aura::compiler::types::ref_type;

    // Issue #2259: pure consteval-path classify on the shape hot path
    // (no per-call atomics). Tag stability feed raises Fixnum/Ref
    // confidence for ShapeProfiler speculative decisions.
    const EvalValueTag tag = classify_eval_value_tag_consteval(val);
    note_value_tag_stability(tag);
    // Issue #378 follow-up: test_shape's v1-style boundary cases
    // (kFloatBias - 1, kStringBias + 1, kStringBias - 1) hit values
    // that have no valid v2 encoding (v&3 != 0 for floats, or v&3
    // in a "gap" tag). The old contract_assert would abort; we now
    // map Unknown → SHAPE_UNKNOWN so inline_shape_of is total. Debug
    // builds still assert to catch unintentional calls with garbage.
    if (tag == EvalValueTag::Unknown)
        return finish_inline_shape_id(SHAPE_UNKNOWN);

    switch (tag) {
        case EvalValueTag::Fixnum:
            return finish_inline_shape_id(SHAPE_INT);
        case EvalValueTag::Float:
            return finish_inline_shape_id(SHAPE_FLOAT);
        case EvalValueTag::StringV2:
            return finish_inline_shape_id(SHAPE_STRING);
        case EvalValueTag::Special:
            // Issue #1620: Special encoding contracts (bool true/false/void).
            // Matches cxx26_invariants kSpecial* constants.
            if (val == 3 || val == 7) {
                contract_assert((val & 3) == 3); // Special low2
                return finish_inline_shape_id(SHAPE_BOOL);
            }
            if (val == 11) {
                contract_assert((val & 3) == 3);
                return finish_inline_shape_id(SHAPE_VOID);
            }
            return finish_inline_shape_id(SHAPE_ANY);
        case EvalValueTag::Ref: {
            inline_shape_ref_dispatch_count.fetch_add(1, std::memory_order_relaxed);
            const auto rt = ref_type(val);
            switch (rt) {
                case aura::compiler::types::RefPair:
                    return finish_inline_shape_id(SHAPE_PAIR);
                case aura::compiler::types::RefVector:
                    return finish_inline_shape_id(SHAPE_VECTOR);
                case aura::compiler::types::RefHash:
                    return finish_inline_shape_id(SHAPE_HASH);
                case aura::compiler::types::RefClosure:
                    return finish_inline_shape_id(SHAPE_CLOSURE);
                default:
                    return finish_inline_shape_id(SHAPE_REF);
            }
        }
        default:
            return finish_inline_shape_id(SHAPE_ANY);
    }
}

static_assert(is_known_inline_shape_id(SHAPE_INT),
              "inline_shape_of int path must be a known ShapeID");

// ═══════════════════════════════════════════════════════════════
// ShapeID computation (FNV-1a style)
// ═══════════════════════════════════════════════════════════════

static ShapeID hash_combine(ShapeID h, ShapeID v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

static ShapeID hash_uint8(ShapeID h, std::uint8_t v) {
    return hash_combine(h, static_cast<ShapeID>(v));
}

static ShapeID hash_int32(ShapeID h, std::int32_t v) {
    return hash_combine(h, static_cast<ShapeID>(v));
}

ShapeID compute_shape_id(const Shape& shape) {
    ShapeID h = ::aura::compiler::stats::kFnvOffsetBasis; // FNV offset
    h = hash_uint8(h, static_cast<std::uint8_t>(shape.tag));
    h = hash_int32(h, shape.type_id);

    switch (shape.tag) {
        case ShapeTag::Any:
        case ShapeTag::Int:
        case ShapeTag::Float:
        case ShapeTag::Bool:
        case ShapeTag::String:
        case ShapeTag::Void:
        case ShapeTag::Ref:
            break;

        case ShapeTag::Pair:
            if (shape.car_shape)
                h = hash_combine(h, shape.car_shape->id);
            if (shape.cdr_shape)
                h = hash_combine(h, shape.cdr_shape->id);
            break;

        case ShapeTag::Vector:
            if (shape.elem_shape)
                h = hash_combine(h, shape.elem_shape->id);
            h = hash_combine(h, static_cast<ShapeID>(shape.min_len));
            h = hash_combine(h, static_cast<ShapeID>(shape.max_len));
            break;

        case ShapeTag::Hash:
            if (shape.key_shape)
                h = hash_combine(h, shape.key_shape->id);
            if (shape.value_shape)
                h = hash_combine(h, shape.value_shape->id);
            break;

        case ShapeTag::Closure:
            h = hash_combine(h, static_cast<ShapeID>(shape.arity));
            if (shape.ret_shape)
                h = hash_combine(h, shape.ret_shape->id);
            break;

        case ShapeTag::Struct:
            h = hash_combine(h, static_cast<ShapeID>(shape.field_count));
            for (std::uint32_t i = 0; i < shape.field_count && shape.union_variants; ++i) {
                if (shape.union_variants[i])
                    h = hash_combine(h, shape.union_variants[i]->id);
            }
            break;

        case ShapeTag::Union:
            h = hash_combine(h, static_cast<ShapeID>(shape.union_count));
            for (std::uint32_t i = 0; i < shape.union_count && shape.union_variants; ++i) {
                if (shape.union_variants[i])
                    h = hash_combine(h, shape.union_variants[i]->id);
            }
            break;
    }
    return h;
}

// ═══════════════════════════════════════════════════════════════
// String conversion
// ═══════════════════════════════════════════════════════════════

const char* shape_tag_name(ShapeTag tag) noexcept {
    switch (tag) {
        case ShapeTag::Any:
            return "any";
        case ShapeTag::Int:
            return "int";
        case ShapeTag::Float:
            return "float";
        case ShapeTag::Bool:
            return "bool";
        case ShapeTag::String:
            return "string";
        case ShapeTag::Void:
            return "void";
        case ShapeTag::Pair:
            return "pair";
        case ShapeTag::Vector:
            return "vector";
        case ShapeTag::Hash:
            return "hash";
        case ShapeTag::Closure:
            return "closure";
        case ShapeTag::Struct:
            return "struct";
        case ShapeTag::Union:
            return "union";
        case ShapeTag::Ref:
            return "ref";
    }
    return "?";
}

std::string format_shape_id(ShapeID id) {
    switch (id) {
        case SHAPE_UNKNOWN:
            return "?";
        case SHAPE_ANY:
            return "any";
        case SHAPE_INT:
            return "Int";
        case SHAPE_FLOAT:
            return "Float";
        case SHAPE_BOOL:
            return "Bool";
        case SHAPE_STRING:
            return "String";
        case SHAPE_VOID:
            return "()";
        case 10:
            return "Pair";
        case 11:
            return "Vector";
        case 12:
            return "Hash";
        case 13:
            return "Closure";
        case 14:
            return "Ref";
        default:
            return "shape#" + std::to_string(id);
    }
}

// ═══════════════════════════════════════════════════════════════
// ShapeProfiler implementation
// ═══════════════════════════════════════════════════════════════

ShapeProfiler::ShapeProfiler() {
    // Issue #2257: production-default HighMutation preset. Soft
    // path remains available via AURA_SHAPE_HIGH_MUTATION=0
    // (unit tests / sandbox). Adaptive partial-relower threshold
    // (#2112) feeds on stability_ratio which HighMutation widens
    // (window_size=2000 vs default 1000; deopt_storm_window=512
    // vs default 256; deopt_storm_threshold=6 vs default 4) so
    // long-running Agent sessions stay more permissive while
    // still isolating storms when they hit the threshold.
    if (shape_high_mutation_default_enabled())
        active_preset_ = kHighMutationPreset;
}

// ── Issue #2141: lock helpers ─────────────────────────────────
std::unique_lock<std::shared_mutex> ShapeProfiler::unique_lock_() const {
    std::unique_lock<std::shared_mutex> lock(mtx_, std::try_to_lock);
    if (!lock.owns_lock()) {
        lock_contended_total_.fetch_add(1, std::memory_order_relaxed);
        lock = std::unique_lock<std::shared_mutex>(mtx_);
    }
    return lock;
}

std::shared_lock<std::shared_mutex> ShapeProfiler::shared_lock_() const {
    std::shared_lock<std::shared_mutex> lock(mtx_, std::try_to_lock);
    if (!lock.owns_lock()) {
        lock_contended_total_.fetch_add(1, std::memory_order_relaxed);
        lock = std::shared_lock<std::shared_mutex>(mtx_);
    }
    return lock;
}

void ShapeProfiler::set_window_size(std::uint32_t n) {
    auto lock = unique_lock_();
    window_size_ = n;
}

void ShapeProfiler::set_stability_ratio(double r) {
    auto lock = unique_lock_();
    stability_ratio_ = r;
}

std::uint32_t ShapeProfiler::window_size() const noexcept {
    auto lock = shared_lock_();
    return window_size_;
}

double ShapeProfiler::stability_ratio() const noexcept {
    auto lock = shared_lock_();
    return stability_ratio_;
}

void ShapeProfiler::apply_preset(Preset p) {
    auto lock = unique_lock_();
    window_size_ = p.window_size;
    stability_ratio_ = p.stability_ratio;
    min_samples_for_stable_ = p.min_samples_for_stable;
    deopt_storm_window_ = p.deopt_storm_window;
    deopt_storm_threshold_ = p.deopt_storm_threshold;
    active_preset_ = p;
}

ShapeProfiler::Preset ShapeProfiler::active_preset() const noexcept {
    auto lock = shared_lock_();
    return active_preset_;
}

std::uint32_t ShapeProfiler::min_samples_for_stable() const noexcept {
    auto lock = shared_lock_();
    return min_samples_for_stable_;
}

std::uint32_t ShapeProfiler::deopt_storm_window() const noexcept {
    auto lock = shared_lock_();
    return deopt_storm_window_;
}

std::uint32_t ShapeProfiler::deopt_storm_threshold() const noexcept {
    auto lock = shared_lock_();
    return deopt_storm_threshold_;
}

void ShapeProfiler::set_max_profiles(std::size_t n) {
    auto lock = unique_lock_();
    max_profiles_ = n ? n : 1;
}

std::size_t ShapeProfiler::max_profiles() const noexcept {
    auto lock = shared_lock_();
    return max_profiles_;
}

std::size_t ShapeProfiler::profile_count() const noexcept {
    auto lock = shared_lock_();
    return profiles_.size();
}

std::uint64_t ShapeProfiler::profile_evictions() const noexcept {
    auto lock = shared_lock_();
    return profile_evictions_;
}

void ShapeProfiler::set_dirty_hook(std::function<void(FnKey fn, std::uint32_t dirty_scope)> hook) {
    auto lock = unique_lock_();
    dirty_hook_ = std::move(hook);
}

void ShapeProfiler::ShapeHistoryRing::push(const ShapeRecord& rec, std::uint32_t window_size) {
    ensure_capacity(window_size);
    if (count < window_size) {
        slots[count++] = rec;
        return;
    }
    slots[head] = rec;
    head = (head + 1) % window_size;
    history_jitter_reduction_count.fetch_add(1, std::memory_order_relaxed);
}

ShapeID ShapeProfiler::FnProfile::compute_dominant() const {
    if (history.size() == 0)
        return SHAPE_UNKNOWN;

    std::unordered_map<ShapeID, std::uint32_t> counts;
    history.for_each([&](const ShapeRecord& rec) { counts[rec.shape_id]++; });

    ShapeID best = SHAPE_UNKNOWN;
    std::uint32_t best_count = 0;
    for (auto& [sid, cnt] : counts) {
        if (cnt > best_count) {
            best_count = cnt;
            best = sid;
        }
    }
    // Issue #1519: dominant count cannot exceed history size.
    contract_assert(best_count <= history.size());
    aura::core::cpp26::record_hotpath_invariant_hit();
    return best;
}

bool ShapeProfiler::record_shape(FnKey fn, ShapeID shape_id) {
    // The pre (shape_id != SHAPE_UNKNOWN) is on the declaration
    // in shape_profiler.h.
    aura::core::cpp26::record_hotpath_invariant_hit();
    contract_assert(is_known_inline_shape_id(shape_id) || shape_id != SHAPE_UNKNOWN);

    // Issue #2141: unique lock; fire external hooks after unlock.
    bool fire_stability_loss = false;
    std::uint64_t fire_version = 0;
    std::function<void(FnKey, std::uint32_t)> dirty_hook_copy;
    bool result = false;

    {
        auto lock = unique_lock_();
        // Issue #992: cap profiles before insert.
        if (profiles_.find(fn) == profiles_.end() && profiles_.size() >= max_profiles_)
            maybe_evict_profiles_();
        auto& profile = profiles_[fn];
        auto& history = profile.history;
        std::uint64_t now = ++global_time_;
        profile.last_used = now;

        history.push({shape_id, now}, window_size_);

        profile.total_calls++;

        // Issue #1468: history_hit/miss counters.
        history_hit_count_.fetch_add(1, std::memory_order_relaxed);

        if (history.size() < kStableThreshold)
            return false;

        auto dominant = profile.compute_dominant();
        auto dominant_count = 0;
        history.for_each([&](const ShapeRecord& rec) {
            if (rec.shape_id == dominant)
                dominant_count++;
        });

        const auto hist_size = history.size();
        contract_assert(dominant_count >= 0);
        contract_assert(static_cast<std::uint32_t>(dominant_count) <= hist_size);
        double ratio = static_cast<double>(dominant_count) / static_cast<double>(hist_size);
        contract_assert(ratio >= 0.0 && ratio <= 1.0);
        if (ratio >= stability_ratio_ && profile.is_stable && profile.stable_shape == dominant) {
            return true;
        }

        if (ratio >= stability_ratio_) {
            if (!profile.is_stable) {
                shape_stability_hit_count.fetch_add(1, std::memory_order_relaxed);
            }
            profile.is_stable = true;
            profile.stable_shape = dominant;
            profile.last_metric_time = now;
            contract_assert(ratio >= 0.0 && ratio <= 1.0);
            aura::core::cpp26::record_hotpath_invariant_hit();
            return true;
        }

        if (profile.is_stable) {
            mutation_shape_churn_count.fetch_add(1, std::memory_order_relaxed);
            shape_jit_pass::record_stability_churn_deopt();
            shape_jit_pass::record_speculative_win_lost();
            const std::uint64_t epoch = aura::core::current_mutation_epoch();
            profile.version = epoch > profile.version ? epoch : profile.version + 1;
            shape_version_bump_count.fetch_add(1, std::memory_order_relaxed);
            update_deopt_storm_state_(fn);
            fire_stability_loss = true;
            fire_version = profile.version;
            dirty_hook_copy = dirty_hook_;
        }
        profile.is_stable = false;
        profile.stable_shape = SHAPE_UNKNOWN;
        result = false;
    }

    if (fire_stability_loss) {
        fire_shape_deopt_hook(fn, fire_version, kShapeDirtyScopeStabilityLoss);
        if (dirty_hook_copy) {
            shape_jit_pass::record_dirty_from_shape();
            dirty_hook_copy(fn, kShapeDirtyScopeStabilityLoss);
        }
    }
    return result;
}

bool ShapeProfiler::is_stable(FnKey fn) const {
    auto lock = shared_lock_();
    auto it = profiles_.find(fn);
    return it != profiles_.end() && it->second.is_stable;
}

ShapeID ShapeProfiler::dominant_shape(FnKey fn) const {
    auto lock = shared_lock_();
    auto it = profiles_.find(fn);
    if (it == profiles_.end())
        return SHAPE_UNKNOWN;
    return it->second.stable_shape;
}

ShapeSnapshot ShapeProfiler::current_snapshot(FnKey fn) const {
    auto lock = shared_lock_();
    ShapeSnapshot snap;
    auto it = profiles_.find(fn);
    if (it != profiles_.end()) {
        snap.id = it->second.stable_shape;
        snap.version = it->second.version;
    }
    return snap;
}

bool ShapeProfiler::invalidate_unlocked_(FnKey fn) {
    // Caller must hold unique lock on mtx_.
    auto it = profiles_.find(fn);
    if (it == profiles_.end())
        return false;

    const bool was_stable = it->second.is_stable;
    it->second.history.clear();
    it->second.is_stable = false;
    it->second.stable_shape = SHAPE_UNKNOWN;
    it->second.deopt_count++;
    it->second.version++;
    shape_version_bump_count.fetch_add(1, std::memory_order_relaxed);
    if (was_stable) {
        mutation_shape_churn_count.fetch_add(1, std::memory_order_relaxed);
        shape_jit_pass::record_stability_churn_deopt();
        shape_jit_pass::record_speculative_win_lost();
        aura::core::arena_policy::signal_shape_churn();
        aura::core::arena_policy::signal_dirty_cascade();
    }
    const std::uint64_t epoch = aura::core::current_mutation_epoch();
    if (epoch > it->second.version)
        it->second.version = epoch;
    update_deopt_storm_state_(fn);
    return was_stable;
}

bool ShapeProfiler::invalidate(FnKey fn) {
    // The pre (fn != 0) is on the declaration in shape_profiler.h.
    bool was_stable = false;
    std::uint64_t version = 0;
    bool found = false;
    std::function<void(FnKey, std::uint32_t)> dirty_hook_copy;
    {
        auto lock = unique_lock_();
        auto it = profiles_.find(fn);
        if (it == profiles_.end())
            return false;
        found = true;
        was_stable = invalidate_unlocked_(fn);
        it = profiles_.find(fn);
        if (it != profiles_.end())
            version = it->second.version;
        dirty_hook_copy = dirty_hook_;
    }
    if (found) {
        fire_shape_deopt_hook(fn, version, kShapeDirtyScopeInvalidate);
        if (dirty_hook_copy) {
            shape_jit_pass::record_dirty_from_shape();
            dirty_hook_copy(fn, kShapeDirtyScopeInvalidate);
        }
    }
    return was_stable;
}

void ShapeProfiler::invalidate_all() noexcept {
    // Collect keys under unique lock, then invalidate each (hook-safe).
    std::vector<FnKey> keys;
    {
        auto lock = unique_lock_();
        keys.reserve(profiles_.size());
        for (const auto& [k, _] : profiles_)
            keys.push_back(k);
        mutation_induced_invalidations_.fetch_add(keys.size(), std::memory_order_relaxed);
    }
    for (FnKey fn : keys)
        (void)invalidate(fn);
}

// Issue #1521: Arena compact soft coordination.
// Value-tag shapes (int/float/bool/string/ref-kind) do not depend on
// arena addresses; full invalidate_all would clear history and feed
// the deopt-storm ring, thrashing JIT under multi-round AI self-modify
// + GC. Instead: version bump + compact-scoped deopt hook, keep stable.
std::uint32_t ShapeProfiler::on_arena_compact() noexcept {
    arena_compact_calls_.fetch_add(1, std::memory_order_relaxed);
    shape_inval_on_compact_triggered.fetch_add(1, std::memory_order_relaxed);
    // Issue #2256: feed Moving-compact observability counters
    // (process atomics; mirrors the CompilerMetrics fields for pure
    // unit tests). pin_hits + remap_us honor the LifetimePin
    // hard contract under Moving mode.
    const auto t0 = std::chrono::steady_clock::now();

    // Issue #2141: mutate under unique lock; fire hooks after unlock.
    struct HookWork {
        FnKey fn;
        std::uint64_t version;
    };
    std::vector<HookWork> hooks_to_fire;
    std::function<void(FnKey, std::uint32_t)> dirty_hook_copy;
    std::uint32_t touched = 0;

    {
        auto lock = unique_lock_();
        if (profiles_.empty()) {
            deopt_storm_compact_suppressed.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }

        const std::uint64_t epoch = aura::core::current_mutation_epoch();
        dirty_hook_copy = dirty_hook_;
        hooks_to_fire.reserve(profiles_.size());

        // flat_map iterator yields pair-by-value proxy; use auto&&.
        for (auto&& [fn, profile] : profiles_) {
            const bool was_stable = profile.is_stable;
            profile.version++;
            if (epoch > profile.version)
                profile.version = epoch;
            shape_version_bump_count.fetch_add(1, std::memory_order_relaxed);
            ++touched;

            if (was_stable) {
                shape_stability_post_compact_preserved.fetch_add(1, std::memory_order_relaxed);
                arena_compact_stable_preserved_.fetch_add(1, std::memory_order_relaxed);
            }

            hooks_to_fire.push_back(HookWork{fn, profile.version});
            deopt_from_arena_compact_total.fetch_add(1, std::memory_order_relaxed);
            arena_compact_deopt_hooks_.fetch_add(1, std::memory_order_relaxed);
            // Explicitly do NOT call update_deopt_storm_state_(fn).
            deopt_storm_compact_suppressed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    for (const auto& h : hooks_to_fire) {
        fire_shape_deopt_hook(h.fn, h.version, kShapeDirtyScopeArenaCompact);
        if (dirty_hook_copy) {
            shape_jit_pass::record_dirty_from_shape();
            dirty_hook_copy(h.fn, kShapeDirtyScopeArenaCompact);
        }
    }
    // Issue #2256: Moving-compact hard-contract observability
    // counters (process atomics; mirror the per-CompilerMetrics
    // fields for pure unit tests). Cumulative across all ShapeProfiler
    // instances — pin-or-remap honor + remap cost.
    const auto t1 = std::chrono::steady_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    g_moving_compact_count_total.fetch_add(1, std::memory_order_relaxed);
    g_moving_compact_remap_us_total.fetch_add(static_cast<std::uint64_t>(us),
                                              std::memory_order_relaxed);
    return touched;
}

// Issue #1521: boundary / fiber-steal exit — re-read stability and
// soft-clear storm flag when it was never justified by mutation churn
// after pure compact pressure (defensive; compact never sets the ring).
double ShapeProfiler::on_boundary_or_fiber_sync(bool clear_compact_only_storm) noexcept {
    boundary_fiber_sync_calls_.fetch_add(1, std::memory_order_relaxed);
    shape_boundary_post_compact_checks.fetch_add(1, std::memory_order_relaxed);
    shape_fiber_steal_sync_total.fetch_add(1, std::memory_order_relaxed);
    record_shape_fiber_refresh();

    {
        auto lock = unique_lock_();
        if (clear_compact_only_storm && deopt_storm_active_.load(std::memory_order_acquire)) {
            if (deopt_ring_count_ < deopt_storm_threshold_) {
                deopt_storm_active_.store(false, std::memory_order_release);
            }
        }
    }
    return shape_stable_ratio();
}

// Issue #1468: deopt-storm detection helper. Called from
// invalidate() and from the deopt-hook path. Maintains a ring of
// (time, fn) pairs and sets deopt_storm_active_ when the count
// in the last deopt_storm_window_ events exceeds the threshold.
//
// Why a per-instance ring (vs global): the storm is local to this
// ShapeProfiler's workload — different evaluators (eval / IR / JIT)
// can run with isolated profilers and not stomp each other.
void ShapeProfiler::update_deopt_storm_state_(FnKey fn) noexcept {
    // Push the new event into the ring.
    if (deopt_ring_.size() != deopt_storm_window_) {
        deopt_ring_.assign(deopt_storm_window_ > 0 ? deopt_storm_window_ : 1, DeoptEvent{0, 0});
    }
    deopt_ring_[deopt_ring_head_] = DeoptEvent{global_time_, fn};
    deopt_ring_head_ = (deopt_ring_head_ + 1) % static_cast<std::uint32_t>(deopt_ring_.size());
    if (deopt_ring_count_ < deopt_ring_.size())
        ++deopt_ring_count_;
    // Count deopts in the most-recent `deopt_storm_window_` ring entries.
    // Conservative: count everything in the ring if it just filled, else
    // count up to the head pointer. Since the ring is sized to
    // deopt_storm_window_, this is exact.
    const std::uint32_t window = static_cast<std::uint32_t>(deopt_ring_.size());
    std::uint32_t recent = 0;
    for (std::uint32_t i = 0; i < deopt_ring_count_; ++i)
        ++recent; // all entries in the ring are within the window
    if (recent >= deopt_storm_threshold_ && !deopt_storm_active_.load(std::memory_order_acquire)) {
        deopt_storm_active_.store(true, std::memory_order_release);
        deopt_storm_total_.fetch_add(1, std::memory_order_relaxed);
        // Issue #2257: on storm enter, bump shape_version (file-scope
        // atomic) so any speculative opt from the previous version is
        // invalidated on the next observation cycle. Mirrors the
        // per-CompilerMetrics deopt_storm_isolations_total field.
        g_deopt_storm_isolations_total_atomic().fetch_add(1, std::memory_order_relaxed);
        bump_shape_version_on_storm_enter();
    }
}

// Issue #1468 / #2141: ratio accessors under shared lock (multi-fiber safe).
double ShapeProfiler::shape_stable_ratio() const noexcept {
    auto lock = shared_lock_();
    if (profiles_.empty())
        return 0.0;
    std::uint32_t stable = 0;
    for (const auto& [k, p] : profiles_) {
        (void)k;
        if (p.is_stable)
            ++stable;
    }
    return static_cast<double>(stable) / static_cast<double>(profiles_.size());
}

double ShapeProfiler::deopt_rate_per_fn() const noexcept {
    auto lock = shared_lock_();
    if (profiles_.empty())
        return 0.0;
    std::uint64_t total_deopt = 0;
    for (const auto& [k, p] : profiles_) {
        (void)k;
        total_deopt += p.deopt_count;
    }
    return static_cast<double>(total_deopt) / static_cast<double>(profiles_.size());
}

double ShapeProfiler::history_hit_rate() const noexcept {
    const auto hits = history_hit_count_.load(std::memory_order_relaxed);
    const auto misses = history_miss_count_.load(std::memory_order_relaxed);
    const auto total = hits + misses;
    if (total == 0)
        return 0.0;
    return static_cast<double>(hits) / static_cast<double>(total);
}

ShapeFnMetrics ShapeProfiler::metrics(FnKey fn) const {
    auto lock = shared_lock_();
    ShapeFnMetrics m;
    auto it = profiles_.find(fn);
    if (it == profiles_.end())
        return m;

    auto& p = it->second;
    m.total_calls = p.total_calls;
    m.deopt_count = p.deopt_count;

    std::unordered_set<ShapeID> seen;
    p.history.for_each([&](const ShapeRecord& rec) { seen.insert(rec.shape_id); });
    m.unique_shapes_seen = static_cast<std::uint32_t>(seen.size());

    if (p.history.size() >= kStableThreshold) {
        auto dominant = p.compute_dominant();
        auto dominant_count = 0;
        p.history.for_each([&](const ShapeRecord& rec) {
            if (rec.shape_id == dominant)
                dominant_count++;
        });
        m.shape_stability_ratio = static_cast<double>(dominant_count) / p.history.size();
    }

    if (p.total_calls > 0 && p.deopt_count > 0) {
        m.shape_change_frequency = (p.deopt_count * 1000) / p.total_calls;
    }
    m.is_good_deopt_candidate = (m.shape_change_frequency < 10);

    return m;
}

void ShapeProfiler::reset() {
    auto lock = unique_lock_();
    profiles_.clear();
    global_time_ = 0;
    profile_evictions_ = 0;
    deopt_ring_.clear();
    deopt_ring_head_ = 0;
    deopt_ring_count_ = 0;
    deopt_storm_active_.store(false, std::memory_order_release);
}

void ShapeProfiler::maybe_evict_profiles_() {
    // Caller must hold unique lock. Issue #992: drop oldest last_used.
    while (profiles_.size() >= max_profiles_ && !profiles_.empty()) {
        auto victim = profiles_.begin();
        std::uint64_t oldest = victim->second.last_used;
        for (auto it = profiles_.begin(); it != profiles_.end(); ++it) {
            if (it->second.last_used < oldest) {
                oldest = it->second.last_used;
                victim = it;
            }
        }
        profiles_.erase(victim);
        ++profile_evictions_;
    }
}

std::vector<FnKey> ShapeProfiler::tracked_fns() const {
    auto lock = shared_lock_();
    std::vector<FnKey> keys;
    keys.reserve(profiles_.size());
    for (const auto& [k, _] : profiles_)
        keys.push_back(k);
    return keys;
}

} // namespace aura::compiler::shape
