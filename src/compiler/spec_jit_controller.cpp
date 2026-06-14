// spec_jit_controller.cpp — Speculative JIT controller implementation
//
// Phase 2: L1 type specialization (#53 Shape-based Speculative JIT)
//
#include "spec_jit_controller.h"
#include "value_tags.h"
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

// ── FLOAT_BIAS / STRING_BIAS from value_tags.h (issue #58, #181) ──
// Issue #181 Cycle 2: STRING_BIAS_VAL_2 is the v2 upper bound of
// the string range (was STRING_BIAS_VAL before Cycle 2).
static constexpr std::int64_t kFloatBias  = aura::compiler::types::FLOAT_BIAS_VAL;
static constexpr std::int64_t kStringBias = aura::compiler::types::STRING_BIAS_VAL_2;

static inline bool is_fixnum_val(std::int64_t v) noexcept { return (v & 1) == 0; }
static inline bool is_ref_val(std::int64_t v) noexcept    { return (v & 3) == 1; }
static inline std::uint64_t ref_type_val(std::int64_t v) noexcept {
    return (static_cast<std::uint64_t>(v) >> 2) & 0xF;
}

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
            case 10: // Pair (ref type RefPair=0)
                match = is_ref_val(val) && ref_type_val(val) == aura::compiler::types::RefPair;
                break;
            case 11: // Vector (ref type RefVector=3)
                match = is_ref_val(val) && ref_type_val(val) == aura::compiler::types::RefVector;
                break;
            case 12: // Hash (ref type RefHash=4)
                match = is_ref_val(val) && ref_type_val(val) == aura::compiler::types::RefHash;
                break;
            case 13: // Closure (ref type RefClosure=1)
                match = is_ref_val(val) && ref_type_val(val) == aura::compiler::types::RefClosure;
                break;
            case 14: // Ref (generic)
                match = is_ref_val(val);
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

    // Issue #170 Phase 2 / item #1: deopt gate. If the
    // underlying AuraJIT has seen ANY unhandled opcode,
    // skip shape specialization — the specialized path would
    // inherit the bug. The caller falls back to the generic
    // (non-specialized) function, which goes through the
    // IR executor or a different code path that's safe for
    // unhandled opcodes.
    if (should_deopt_specialization()) {
        std::fprintf(stderr,
            "spec: deopting specialization for '%s' "
            "(AuraJIT unhandled_opcode_count=%llu > 0)\n",
            fn_name.c_str(),
            (unsigned long long)unhandled_opcode_count());
        return nullptr;
    }

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

// Issue #170 Phase 2 / item #1: deopt signal for the shape
// specializer. Returns true if the underlying AuraJIT has
// reported ANY unhandled opcode since process start. This is
// the conservative version — any unhandled opcode in any
// function disables shape specialization globally. A
// per-function version is a future enhancement (would need
// to track which function compiled the unhandled opcode).
bool SpecJITController::should_deopt_specialization() const {
    return unhandled_opcode_count() > 0;
}

std::uint64_t SpecJITController::unhandled_opcode_count() const {
    return jit_.metrics().unhandled_opcode_count.load(std::memory_order_relaxed);
}

// Issue #193: per-function deopt signal. Replaces the
// conservative should_deopt_specialization() for callers that
// know which function they're specializing. The optional
// `threshold` parameter avoids thrashing on transient bugs
// during initial JIT warmup: a function that hits an
// unhandled opcode once or twice isn't immediately deopted
// for the rest of the session.
bool SpecJITController::should_deopt_specialization_for(const std::string& fn_name,
                                                         std::uint64_t threshold) const {
    return unhandled_opcode_count_for(fn_name) > threshold;
}

std::uint64_t SpecJITController::unhandled_opcode_count_for(const std::string& fn_name) const {
    return jit_.unhandled_opcode_count_for_function(fn_name.c_str());
}

}  // namespace aura::compiler::shape
