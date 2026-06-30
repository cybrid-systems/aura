// shape.h — Shape system for speculative JIT (traditional header, not C++ module)
//
// Defines Shape/ShapeID/ShapeTag for tracking value shapes at runtime.
// Kept as a traditional header because C++ modules can't easily express
// recursive union types (value.ixx limitation).
//
// The shape tag ↔ value-tag mapping (e.g. SHAPE_PAIR ↔ RefPair=0) is
// defined alongside the value tag set in value_tags.h. See that header
// for the module-vs-header boundary rationale (issue #58), and
// Issue #58 (archived: git tag docs-archive-pre-2026-06) for the full rule set.
//
// Part of Phase 1: Shape infrastructure (#53 Shape-based Speculative JIT)
//
#ifndef AURA_COMPILER_SHAPE_H
#define AURA_COMPILER_SHAPE_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <functional>
#include <vector>
#include <memory>

namespace aura::compiler::shape {

// ── ShapeID: unique identifier for a shape ─────────────────────
using ShapeID = std::uint64_t;

constexpr ShapeID SHAPE_UNKNOWN = 0;
constexpr ShapeID SHAPE_ANY = 1; // Any/generic
constexpr ShapeID SHAPE_INT = 2;
constexpr ShapeID SHAPE_FLOAT = 3;
constexpr ShapeID SHAPE_BOOL = 4;
constexpr ShapeID SHAPE_STRING = 5;
constexpr ShapeID SHAPE_VOID = 6;
constexpr ShapeID SHAPE_PAIR = 10;
constexpr ShapeID SHAPE_VECTOR = 11;
constexpr ShapeID SHAPE_HASH = 12;
constexpr ShapeID SHAPE_CLOSURE = 13;
constexpr ShapeID SHAPE_REF = 14;

// Issue #570: constexpr guard for inline_shape_of results.
inline constexpr bool is_known_inline_shape_id(ShapeID id) noexcept {
    if (id == SHAPE_UNKNOWN || id == SHAPE_ANY)
        return true;
    if (id >= SHAPE_INT && id <= SHAPE_VOID)
        return true;
    if (id >= SHAPE_PAIR && id <= SHAPE_REF)
        return true;
    return false;
}

static_assert(is_known_inline_shape_id(SHAPE_INT), "SHAPE_INT in inline range");
static_assert(is_known_inline_shape_id(SHAPE_PAIR), "SHAPE_PAIR in inline range");

// Issue #570 observability (relaxed-ordering; advisory counts).
inline std::atomic<std::uint64_t> shape_stability_hit_count{0};
inline std::atomic<std::uint64_t> shape_version_bump_count{0};
inline std::atomic<std::uint64_t> shape_fiber_refresh_count{0};
inline std::atomic<std::uint64_t> mutation_shape_churn_count{0};
inline std::atomic<std::uint64_t> shape_deopt_hook_fire_count{0};

inline void record_shape_fiber_refresh() noexcept {
    shape_fiber_refresh_count.fetch_add(1, std::memory_order_relaxed);
}

// ── ShapeTag: top-level classification ─────────────────────────
enum class ShapeTag : std::uint8_t {
    Any = 0,
    Int = 1,
    Float = 2,
    Bool = 3,
    String = 4,
    Void = 5,
    Pair = 6,
    Vector = 7,
    Hash = 8,
    Closure = 9,
    Struct = 10,
    Union = 11,
    Ref = 12, // Generic ref (not further specialized)
};

// ── FnKey: identifies a function uniquely ──────────────────────
using FnKey = std::uint64_t; // hash of (session_id + function_name)

inline FnKey make_fn_key(const std::string& session, const std::string& name) {
    auto h1 = std::hash<std::string>{}(session);
    auto h2 = std::hash<std::string>{}(name);
    return h1 ^ (h2 << 1);
}

// ── Shape: value shape description ─────────────────────────────
// Recursive structure. Heap-allocated child shapes use Shape*.
// compute_id() should be called after constructing the full tree.
struct Shape {
    ShapeTag tag = ShapeTag::Any;
    ShapeID id = SHAPE_UNKNOWN;
    std::int32_t type_id = -1; // TypeRegistry type ID (if known)

    // Child shapes (heap-allocated)
    Shape* car_shape = nullptr;
    Shape* cdr_shape = nullptr;
    Shape* key_shape = nullptr;
    Shape* value_shape = nullptr;
    Shape* elem_shape = nullptr;
    Shape* ret_shape = nullptr;
    Shape** union_variants = nullptr;
    std::uint32_t union_count = 0;
    std::uint32_t min_len = 0;
    std::uint32_t max_len = 0;
    std::uint32_t arity = 0;
    std::uint32_t field_count = 0;
    std::int32_t int_range[2] = {0, 0};
};

// ── Shape hash function ────────────────────────────────────────
// Deterministic hash: ShapeTag + type_id + recursive fields.
ShapeID compute_shape_id(const Shape& shape);

// ── Quick inline shape ID from EvalValue raw bits ──────────────
// Fast path: extract ShapeID from an int64_t tagged value.
// Full implementation (with heap traversal) is in shape_profiler.cpp.
ShapeID inline_shape_of(std::int64_t val);

// ── String conversion ──────────────────────────────────────────
const char* shape_tag_name(ShapeTag tag) noexcept;
std::string format_shape_id(ShapeID id);

// ── ShapeFnMetrics: per-function shape statistics ──────────────
struct ShapeFnMetrics {
    std::uint64_t total_calls = 0;
    std::uint64_t deopt_count = 0;
    double shape_stability_ratio = 0.0;
    std::uint64_t shape_change_frequency = 0;
    std::uint32_t unique_shapes_seen = 0;
    bool is_good_deopt_candidate = false;
};

// ── ShapeSnapshot: point-in-time shape state for guard comparison ──
struct ShapeSnapshot {
    ShapeID id = SHAPE_UNKNOWN;
    std::uint64_t version = 0;
};

} // namespace aura::compiler::shape

#endif // AURA_COMPILER_SHAPE_H
