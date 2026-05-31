// spec_jit_controller.cpp — Speculative JIT controller implementation
//
// Phase 2: L1 type specialization (#53 Shape-based Speculative JIT)
//
#include "spec_jit_controller.h"
#include <algorithm>
#include <cstdio>

// Shape guard runtime — checks runtime arg values against expected shape map.
// Returns true if all args match. Used at the call site before invoking
// a specialized JIT function.
//
// Matching rules:
//   shape_map entry 0 (Dynamic) → always matches
//   shape_map entry 1 (Int)     → value must be a fixnum (bit0=0, > FLOAT_BIAS)
//   shape_map entry 2 (Float)   → value must be in float bias range
//
namespace aura::compiler::shape {

// ── FLOAT_BIAS from shape_profiler.cpp / value.ixx ────────────
static constexpr std::int64_t kFloatBias  = -10000000000000000LL;
static constexpr std::int64_t kStringBias = -9000000000000000000LL;

static inline bool is_fixnum_val(std::int64_t v) noexcept { return (v & 1) == 0; }

bool check_shape_guard(const std::int64_t* args, std::uint32_t arg_count,
                       const std::uint8_t* shape_map, std::uint32_t map_size) {
    if (!shape_map)
        return true;  // No specialization → always match

    std::uint32_t check_count = std::min(arg_count, map_size);
    for (std::uint32_t i = 0; i < check_count; ++i) {
        std::uint8_t expected = shape_map[i];
        if (expected == 0)  // Dynamic — always matches
            continue;

        std::int64_t val = args[i];
        bool match = false;

        switch (expected) {
            case 1:  // Int
                match = is_fixnum_val(val) && val > kFloatBias;
                break;
            case 2:  // Float
                match = (val <= kFloatBias && val > kStringBias);
                break;
            case 3:  // Bool
                match = (val == 3 || val == 7);
                break;
            case 5:  // Void
                match = (val == 11);
                break;
            default:
                match = true;  // Unknown shapes → pass through
                break;
        }

        if (!match)
            return false;
    }
    return true;
}

// ── SpecJITController ─────────────────────────────────────────

SpecJITController::SpecJITController(aura::jit::AuraJIT& jit) : jit_(jit) {}

aura::jit::ScalarFn SpecJITController::compile_specialized(
    const std::string& fn_name,
    const std::uint8_t* shape_map,
    std::uint32_t shape_map_size,
    aura::jit::ScalarFn generic_fn,
    std::uint32_t arg_count,
    std::uint32_t local_count)
{
    (void)generic_fn;  // Unused in Phase 2 — the guard is at the call site

    // Build a FlatFunction with shape_map
    // For L1, we just need a skeleton function that the AuraJIT can compile
    // The actual compilation happens through the existing JIT path, but with
    // shape_map attached to skip tag checks.
    //
    // For now, we cache the specialization key but rely on the existing
    // AuraJIT::compile path (which is called from service.ixx with shape_map).
    // This controller tracks what has been specialized.

    auto& entries = specializations_[fn_name];
    ShapeID shape_key = 0;
    for (std::uint32_t i = 0; i < shape_map_size; ++i)
        shape_key = (shape_key << 1) ^ static_cast<ShapeID>(shape_map[i]);

    // Check if already cached
    for (auto& e : entries) {
        if (e.shape == shape_key) {
            if (e.fn_ptr)
                return e.fn_ptr;
        }
    }

    // The actual compilation happens in service.ixx via AuraJIT::compile().
    // Here we just record the specialization attempt.
    entries.push_back({shape_key, nullptr, global_version_});

    std::fprintf(stderr, "spec: cached specialization request for '%s' (shape_key=%lu)\n",
                 fn_name.c_str(), (unsigned long)shape_key);
    return nullptr;
}

bool SpecJITController::has_specialization(const std::string& fn_name, ShapeID shape) const {
    auto it = specializations_.find(fn_name);
    if (it == specializations_.end())
        return false;
    for (auto& e : it->second) {
        if (e.shape == shape && e.fn_ptr != nullptr)
            return true;
    }
    return false;
}

aura::jit::ScalarFn SpecJITController::get_specialized(
    const std::string& fn_name, ShapeID shape) const
{
    auto it = specializations_.find(fn_name);
    if (it == specializations_.end())
        return nullptr;
    for (auto& e : it->second) {
        if (e.shape == shape)
            return e.fn_ptr;
    }
    return nullptr;
}

void SpecJITController::invalidate(const std::string& fn_name) {
    auto it = specializations_.find(fn_name);
    if (it != specializations_.end()) {
        global_version_++;
        // Clear fn_ptrs so next call re-compiles
        for (auto& e : it->second)
            e.fn_ptr = nullptr;
    }
}

void SpecJITController::clear() {
    specializations_.clear();
    global_version_ = 0;
}

}  // namespace aura::compiler::shape
