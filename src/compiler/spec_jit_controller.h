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
    aura::jit::ScalarFn compile_specialized(const std::string& fn_name, const uint8_t* shape_map,
                                            uint32_t shape_map_size, aura::jit::ScalarFn generic_fn,
                                            uint32_t arg_count, uint32_t local_count);

    // Check if a specialized version exists for a function+shape combo.
    bool has_specialization(const std::string& fn_name, ShapeID shape) const;

    // Get the specialized function pointer.
    aura::jit::ScalarFn get_specialized(const std::string& fn_name, ShapeID shape) const;

    // Invalidate all specializations for a function.
    void invalidate(const std::string& fn_name);

    // Clear all cached specializations.
    void clear();

    // Issue #170 Phase 2 / item #1: deopt signal.
    //
    // Returns true if the underlying AuraJIT has reported any
    // unhandled opcodes since process start (i.e. the global
    // Metrics::unhandled_opcode_count is > 0). Callers SHOULD
    // skip shape-based specialization when this returns true —
    // the specialized path would inherit the unhandled-opcode
    // bug, leading to silent wrong results.
    //
    // Conservative: a single unhandled opcode anywhere in the
    // process disables ALL specialization. A finer-grained
    // per-function check is a follow-up (would require tracking
    // which function compiled the unhandled opcode).
    bool should_deopt_specialization() const;

    // Issue #170 Phase 2 / item #1: explicit deopt-decision entry.
    // Returns the underlying AuraJIT's unhandled_opcode_count, so
    // callers can implement custom thresholding (e.g. only deopt
    // after N unhandled opcodes in a hot function). The default
    // should_deopt_specialization() uses threshold = 1.
    std::uint64_t unhandled_opcode_count() const;

    // Issue #193: per-function deopt signal. Returns true if the
    // specific function `fn_name` has reported any unhandled
    // opcodes during its compiles. This is the proper replacement
    // for the conservative should_deopt_specialization() in the
    // common case: deopt the affected function, not the whole
    // process. Other functions with clean compiles are unaffected.
    //
    // Optional `threshold` parameter (default 0 = current
    // behavior; any hit triggers deopt). Production code should
    // pass a non-zero threshold (e.g. 10) to avoid thrashing on
    // transient bugs — a function that hits an unhandled opcode
    // once or twice during initial JIT warmup shouldn't
    // permanently disable specialization for it.
    bool should_deopt_specialization_for(const std::string& fn_name,
                                         std::uint64_t threshold = 0) const;

    // Issue #193: explicit per-function counter accessor.
    std::uint64_t unhandled_opcode_count_for(const std::string& fn_name) const;

    // Shape map codes (must match shape.h ShapeID conventions)
    static constexpr uint8_t kDynamic = 0;
    static constexpr uint8_t kShapeInt = 1;
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
bool check_shape_guard(const int64_t* args, uint32_t arg_count, const uint8_t* shape_map,
                       uint32_t map_size);

} // namespace aura::compiler::shape

#endif // AURA_COMPILER_SPEC_JIT_CONTROLLER_H
