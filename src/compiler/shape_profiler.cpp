// shape_profiler.cpp — Shape profiling implementation
//
// Phase 1: Shape infrastructure (#53 Shape-based Speculative JIT)
//
// NOT a C++ module — uses traditional header to avoid C++ module
// recursive type issues.
//
#include "shape_profiler.h"
#include "value_tags.h"
#include <algorithm>
#include <contracts>
#include <cstdint>
#include <unordered_set>

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
            if (val == 3 || val == 7)
                return finish_inline_shape_id(SHAPE_BOOL);
            if (val == 11)
                return finish_inline_shape_id(SHAPE_VOID);
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
    ShapeID h = 0xcbf29ce484222325ULL; // FNV offset
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
    return best;
}

bool ShapeProfiler::record_shape(FnKey fn, ShapeID shape_id) {
    // The pre (shape_id != SHAPE_UNKNOWN) is on the declaration
    // in shape_profiler.h.
    auto& profile = profiles_[fn];
    auto& history = profile.history;
    std::uint64_t now = ++global_time_;

    history.push({shape_id, now}, window_size_);

    profile.total_calls++;

    if (history.size() < kStableThreshold)
        return false;

    auto dominant = profile.compute_dominant();
    auto dominant_count = 0;
    history.for_each([&](const ShapeRecord& rec) {
        if (rec.shape_id == dominant)
            dominant_count++;
    });

    const auto hist_size = history.size();
    double ratio = static_cast<double>(dominant_count) / hist_size;
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
        return true;
    }

    if (profile.is_stable) {
        mutation_shape_churn_count.fetch_add(1, std::memory_order_relaxed);
        fire_shape_deopt_hook(fn, profile.version, kShapeDirtyScopeStabilityLoss);
        if (dirty_hook_)
            dirty_hook_(fn, kShapeDirtyScopeStabilityLoss);
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
    }
    fire_shape_deopt_hook(fn, it->second.version, kShapeDirtyScopeInvalidate);
    if (dirty_hook_)
        dirty_hook_(fn, kShapeDirtyScopeInvalidate);
    return was_stable;
}

void ShapeProfiler::invalidate_all() noexcept {
    const auto keys = tracked_fns();
    for (FnKey fn : keys)
        (void)invalidate(fn);
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
