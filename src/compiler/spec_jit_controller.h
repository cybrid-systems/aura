// spec_jit_controller.h — Speculative JIT compilation lifecycle
//
// Manages shape-specialized function compilation and caching.
// Phase 2: L1 type specialization (#53 Shape-based Speculative JIT)
//
#ifndef AURA_COMPILER_SPEC_JIT_CONTROLLER_H
#define AURA_COMPILER_SPEC_JIT_CONTROLLER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "shape.h"
#include "aura_jit.h"

namespace aura::compiler::shape {

// ── SpecJITController ─────────────────────────────────────────
// Manages specialized compilation of functions with known shapes.
//
// Flow:
//   1. ShapeProfiler detects stable shape for function Fn
//   2. SpecJITController::compile_specialized(Fn, ShapeID) is called
//   3. Generates a FlatFunction with shape_map from known shapes
//   4. Compiles through AuraJIT
//   5. Caches the specialized version
//   6. On subsequent calls, guard check determines if specialized path is used
//
class SpecJITController {
public:
    SpecJITController(aura::jit::AuraJIT& jit);

    // Compile a specialized version of a function for a given shape.
    // Returns the compiled function pointer, or null on failure.
    aura::jit::ScalarFn compile_specialized(
        const std::string& fn_name,
        const uint8_t* shape_map,
        uint32_t shape_map_size,
        aura::jit::ScalarFn generic_fn,
        uint32_t arg_count,
        uint32_t local_count);

    // Check if a specialized version exists for a function+shape combo.
    bool has_specialization(const std::string& fn_name, ShapeID shape) const;

    // Get the specialized function pointer.
    aura::jit::ScalarFn get_specialized(const std::string& fn_name, ShapeID shape) const;

    // Invalidate all specializations for a function.
    void invalidate(const std::string& fn_name);

    // Clear all cached specializations.
    void clear();

    // Shape map codes (must match shape.h ShapeID conventions)
    static constexpr uint8_t kDynamic   = 0;
    static constexpr uint8_t kShapeInt  = 1;
    static constexpr uint8_t kShapeFloat = 2;
    static constexpr uint8_t kShapeBool = 3;
    static constexpr uint8_t kShapeString = 4;
    static constexpr uint8_t kShapeVoid = 5;
    static constexpr uint8_t kShapePair = 10;
    static constexpr uint8_t kShapeVector = 11;
    static constexpr uint8_t kShapeHash = 12;
    static constexpr uint8_t kShapeClosure = 13;
    static constexpr uint8_t kShapeRef = 14;

private:
    struct SpecEntry {
        ShapeID shape;
        aura::jit::ScalarFn fn_ptr;
        uint32_t version;
    };

    aura::jit::AuraJIT& jit_;
    std::unordered_map<std::string, std::vector<SpecEntry>> specializations_;
    uint32_t global_version_ = 0;
};

// ── Runtime shape guard helper ────────────────────────────────
// Checks if runtime values match expected shapes.
// Returns true if all arg shapes match the shape_map.
bool check_shape_guard(const int64_t* args, uint32_t arg_count,
                       const uint8_t* shape_map, uint32_t map_size);

}  // namespace aura::compiler::shape

#endif  // AURA_COMPILER_SPEC_JIT_CONTROLLER_H
