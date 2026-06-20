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
#include <unordered_set>
#include <cstdint>

// We need EvalValue tag helpers. Since value is a C++ module,
// include the relevant inline functions directly (they're constexpr/header-only style).
// The actual EvalValue struct and tag helpers are inline in value.ixx.
// To avoid module dependency, this file only uses the shape.h types.
// The shape_of function is implemented below using raw integer bit tests.

namespace aura::compiler::shape {

// ═══════════════════════════════════════════════════════════════
// shape_of — fast shape classification from tagged int64_t bits
// ═══════════════════════════════════════════════════════════════
//
// Matches the Aura value encoding from value.ixx:
//   bit0=0 : Fixnum (int), range-checked against FLOAT_BIAS
//   bit0-1=11 : Special (#f=3, #t=7, void=11)
//   bit0-1=01 : Ref (pool-indexed heap type, bits 2-5 = subtype)
//   FLOAT_BIAS (≤ -10000000000000000) : Float
//   STRING_BIAS (≤ STRING_BIAS_VAL_2) : String
//   (Issue #181 Cycle 2: STRING_BIAS_VAL_2 = STRING_BIAS_VAL + 2,
//   the v2 upper bound of the string range.)
//
// Timing: ~2-5ns (bit ops, no heap access)
//
// Tag/bit constants come from value_tags.h (issue #58); we just
// alias them under the local names this file used before.
static constexpr std::int64_t kFloatBias = aura::compiler::types::FLOAT_BIAS_VAL;
static constexpr std::int64_t kStringBias = aura::compiler::types::STRING_BIAS_VAL_2;

static inline bool is_fixnum_val(std::int64_t v) noexcept {
    return (v & 1) == 0;
}
static inline bool is_ref_val(std::int64_t v) noexcept {
    return (v & 3) == 1;
}
static inline bool is_special_val(std::int64_t v) noexcept {
    return (v & 3) == 3;
}
static inline std::uint64_t ref_type_val(std::int64_t v) noexcept {
    return (static_cast<std::uint64_t>(v) >> 2) & 0xF;
}

ShapeID inline_shape_of(std::int64_t val) {
    if (is_fixnum_val(val) && val > kFloatBias)
        return SHAPE_INT;
    if (val == 3 || val == 7)
        return SHAPE_BOOL;
    if (val == 11)
        return SHAPE_VOID;
    if (val <= kFloatBias && val > kStringBias)
        return SHAPE_FLOAT;
    if (val <= kStringBias)
        return SHAPE_STRING;
    if (is_ref_val(val)) {
        auto rt = ref_type_val(val);
        switch (rt) {
            case aura::compiler::types::RefPair:
                return 10; // SHAPE_PAIR
            case aura::compiler::types::RefVector:
                return 11; // SHAPE_VECTOR
            case aura::compiler::types::RefHash:
                return 12; // SHAPE_HASH
            case aura::compiler::types::RefClosure:
                return 13; // SHAPE_CLOSURE
            default:
                return 14; // SHAPE_REF
        }
    }
    return SHAPE_ANY;
}

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

ShapeID ShapeProfiler::FnProfile::compute_dominant() const {
    if (history.empty())
        return SHAPE_UNKNOWN;

    std::unordered_map<ShapeID, std::uint32_t> counts;
    for (auto& rec : history)
        counts[rec.shape_id]++;

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

    history.push_back({shape_id, now});
    if (history.size() > window_size_)
        history.erase(history.begin());

    profile.total_calls++;

    if (history.size() < kStableThreshold)
        return false;

    auto dominant = profile.compute_dominant();
    auto dominant_count = 0;
    for (auto& rec : history) {
        if (rec.shape_id == dominant)
            dominant_count++;
    }

    double ratio = static_cast<double>(dominant_count) / history.size();
    if (ratio >= stability_ratio_ && profile.is_stable && profile.stable_shape == dominant) {
        return true;
    }

    if (ratio >= stability_ratio_) {
        profile.is_stable = true;
        profile.stable_shape = dominant;
        profile.last_metric_time = now;
        return true;
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

void ShapeProfiler::invalidate(FnKey fn) {
    // The pre (fn != 0) is on the declaration in shape_profiler.h.
    auto it = profiles_.find(fn);
    if (it != profiles_.end()) {
        it->second.history.clear();
        it->second.is_stable = false;
        it->second.stable_shape = SHAPE_UNKNOWN;
        it->second.deopt_count++;
        it->second.version++;
    }
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
    for (auto& rec : p.history)
        seen.insert(rec.shape_id);
    m.unique_shapes_seen = static_cast<std::uint32_t>(seen.size());

    if (p.history.size() >= kStableThreshold) {
        auto dominant = p.compute_dominant();
        auto dominant_count = 0;
        for (auto& rec : p.history) {
            if (rec.shape_id == dominant)
                dominant_count++;
        }
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
    for (auto& [k, _] : profiles_)
        keys.push_back(k);
    return keys;
}

} // namespace aura::compiler::shape
