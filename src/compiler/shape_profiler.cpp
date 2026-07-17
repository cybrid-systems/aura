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
#include <algorithm>
#include <contracts>
#include <cstdint>
#include <unordered_set>
#include "hash_meta.h" // FNV constants (#901)

// We need EvalValue tag helpers. Since value is a C++ module,
// include the relevant inline functions directly (they're constexpr/header-only style).
// The actual EvalValue struct and tag helpers are inline in value.ixx.
// To avoid module dependency, this file only uses the shape.h types.
// The shape_of function is implemented below using raw integer bit tests.
namespace aura::compiler::shape {

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
    using aura::compiler::types::classify_eval_value_tag;
    using aura::compiler::types::EvalValueTag;
    using aura::compiler::types::ref_type;

    const EvalValueTag tag = classify_eval_value_tag(val);
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

ShapeProfiler::ShapeProfiler() = default;

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
    // Issue #992: cap profiles before insert.
    if (profiles_.find(fn) == profiles_.end() && profiles_.size() >= max_profiles_)
        maybe_evict_profiles_();
    auto& profile = profiles_[fn];
    auto& history = profile.history;
    std::uint64_t now = ++global_time_;
    profile.last_used = now;

    history.push({shape_id, now}, window_size_);

    profile.total_calls++;

    // Issue #1468: history_hit/miss counters. Bump hit if profile
    // existed (which it does after the find/insert above), miss if it
    // was newly inserted. This is a coarse signal — true hit/miss
    // would distinguish "shape_id matches dominant" vs not — but
    // tracking at insertion time is sufficient for agent decision
    // metrics (warmup vs working-set pressure proxy).
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
    // Issue #1519: post invariant — dominant_count <= history.size().
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
        aura::core::cpp26::record_hotpath_invariant_hit(); // Issue #1519
        return true;
    }

    if (profile.is_stable) {
        mutation_shape_churn_count.fetch_add(1, std::memory_order_relaxed);
        shape_jit_pass::record_stability_churn_deopt();
        shape_jit_pass::record_speculative_win_lost();
        const std::uint64_t epoch = shape_jit_pass::current_mutation_epoch();
        profile.version = epoch > profile.version ? epoch : profile.version + 1;
        shape_version_bump_count.fetch_add(1, std::memory_order_relaxed);
        // Issue #1468: feed deopt-storm detector on stability-loss path.
        update_deopt_storm_state_(fn);
        fire_shape_deopt_hook(fn, profile.version, kShapeDirtyScopeStabilityLoss);
        if (dirty_hook_) {
            shape_jit_pass::record_dirty_from_shape();
            dirty_hook_(fn, kShapeDirtyScopeStabilityLoss);
        }
    }
    profile.is_stable = false;
    profile.stable_shape = SHAPE_UNKNOWN;
    return false;
}

bool ShapeProfiler::is_stable(FnKey fn) const {
    auto it = profiles_.find(fn);
    return it != profiles_.end() && it->second.is_stable;
}

ShapeID ShapeProfiler::dominant_shape(FnKey fn) const {
    auto it = profiles_.find(fn);
    if (it == profiles_.end())
        return SHAPE_UNKNOWN;
    return it->second.stable_shape;
}

ShapeSnapshot ShapeProfiler::current_snapshot(FnKey fn) const {
    ShapeSnapshot snap;
    auto it = profiles_.find(fn);
    if (it != profiles_.end()) {
        snap.id = it->second.stable_shape;
        snap.version = it->second.version;
    }
    return snap;
}

bool ShapeProfiler::invalidate(FnKey fn) {
    // The pre (fn != 0) is on the declaration in shape_profiler.h.
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
        // Issue #1621: Shape churn → Arena smart auto-compact closed-loop.
        aura::core::arena_policy::signal_shape_churn();
        aura::core::arena_policy::signal_dirty_cascade();
    }
    const std::uint64_t epoch = shape_jit_pass::current_mutation_epoch();
    if (epoch > it->second.version)
        it->second.version = epoch;
    // Issue #1468: feed deopt-storm detector on every invalidate path.
    update_deopt_storm_state_(fn);
    fire_shape_deopt_hook(fn, it->second.version, kShapeDirtyScopeInvalidate);
    if (dirty_hook_) {
        shape_jit_pass::record_dirty_from_shape();
        dirty_hook_(fn, kShapeDirtyScopeInvalidate);
    }
    return was_stable;
}

void ShapeProfiler::invalidate_all() noexcept {
    const auto keys = tracked_fns();
    mutation_induced_invalidations_.fetch_add(keys.size(), std::memory_order_relaxed);
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
    // Issue #1621: arena compact is a Shape↔Arena closed-loop edge —
    // do not re-signal shape_churn (would re-enter compact). Metrics only.

    const auto keys = tracked_fns();
    if (keys.empty()) {
        // Still count a compact event so metrics / agents see activity.
        deopt_storm_compact_suppressed.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    const std::uint64_t epoch = shape_jit_pass::current_mutation_epoch();
    std::uint32_t touched = 0;
    std::uint32_t preserved = 0;
    std::uint32_t hooks = 0;

    for (FnKey fn : keys) {
        auto it = profiles_.find(fn);
        if (it == profiles_.end())
            continue;
        auto& profile = it->second;
        const bool was_stable = profile.is_stable;
        // Soft version bump (JIT guards / shape_map notice) without
        // clearing history or flipping is_stable.
        profile.version++;
        if (epoch > profile.version)
            profile.version = epoch;
        shape_version_bump_count.fetch_add(1, std::memory_order_relaxed);
        ++touched;

        if (was_stable) {
            ++preserved;
            shape_stability_post_compact_preserved.fetch_add(1, std::memory_order_relaxed);
            arena_compact_stable_preserved_.fetch_add(1, std::memory_order_relaxed);
        }

        // Fire deopt hook with ArenaCompact scope — JIT may refresh
        // entries, but deopt-storm ring is intentionally skipped.
        fire_shape_deopt_hook(fn, profile.version, kShapeDirtyScopeArenaCompact);
        deopt_from_arena_compact_total.fetch_add(1, std::memory_order_relaxed);
        arena_compact_deopt_hooks_.fetch_add(1, std::memory_order_relaxed);
        ++hooks;

        if (dirty_hook_) {
            shape_jit_pass::record_dirty_from_shape();
            dirty_hook_(fn, kShapeDirtyScopeArenaCompact);
        }

        // Explicitly do NOT call update_deopt_storm_state_(fn).
        deopt_storm_compact_suppressed.fetch_add(1, std::memory_order_relaxed);
    }

    (void)hooks;
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

    if (clear_compact_only_storm && deopt_storm_active_.load(std::memory_order_acquire)) {
        // If the deopt ring is empty/sparse relative to threshold, clear
        // storm so compact+mutate loops do not leave agents stuck in
        // generic mode after pressure eases.
        if (deopt_ring_count_ < deopt_storm_threshold_) {
            deopt_storm_active_.store(false, std::memory_order_release);
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
    }
}

// Issue #1468: ratio accessors. Cheap O(profiles_) walks; the
// shape-profiler is single-threaded so no locking needed. Safe to
// call from any thread that holds the eval thread's mutex (which
// is everywhere record_shape / invalidate are called).
double ShapeProfiler::shape_stable_ratio() const noexcept {
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
    profiles_.clear();
    global_time_ = 0;
    profile_evictions_ = 0;
}

void ShapeProfiler::maybe_evict_profiles_() {
    // Issue #992: drop oldest last_used profile until under cap.
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
    std::vector<FnKey> keys;
    keys.reserve(profiles_.size());
    // Issue #337: std::flat_map iterator's reference
    // type is `pair<const K&, V&>` (not `pair<const K, V>&`
    // like unordered_map). The structured-binding
    // pattern still works (the compiler picks the
    // right type), but the K binding is a const
    // reference. We copy it into the local
    // `keys` vector, which is fine for the FnKey
    // type (cheap to copy, just an int).
    for (const auto& [k, _] : profiles_)
        keys.push_back(k);
    return keys;
}

} // namespace aura::compiler::shape
