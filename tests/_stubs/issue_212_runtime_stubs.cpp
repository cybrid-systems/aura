// test_issue_212_runtime_stubs.cpp — minimal stubs for
// test_issue_212.
//
// The pure evaluator functions in aura.compiler.evaluator_pure
// reference `aura_alloc_float` and `aura_float_ref` for the
// float path. test_issue_212 doesn't link aura_jit.cpp /
// aura_jit_runtime.cpp (it would be a circular dep), so we
// provide minimal stubs here. The float path is NOT exercised
// by this test (only the int path is); the stubs just satisfy
// the linker so the int path can compile and link.
//
// Real float behavior is exercised by test_issue_210, which
// links the full JIT runtime.

#include <cstdint>

extern "C" {

// `aura_alloc_float`: tags a double as a float-pool reference.
// The encoding matches aura_jit_runtime.cpp's float pool: each
// float gets a monotonically-increasing index, and the
// reference stores the index in the value's payload. Since
// the test doesn't actually exercise floats, we just return a
// dummy index that's "valid" (in the float range per
// is_float's check). The test never inspects the result via
// as_float, so the content of the float doesn't matter.
std::int64_t aura_alloc_float(double d) {
    // Return a value in the float range (per is_float):
    //   FLOAT_BIAS_VAL < val <= STRING_BIAS_VAL_2 is false;
    //   actually the float range is STRING_BIAS_VAL_2 < val <= FLOAT_BIAS_VAL.
    //   (See value.ixx is_float.)
    // We just return 1 — the test never inspects the result
    // as a float. The `is_float` check might fail (depends
    // on the FLOAT_BIAS_VAL constant), but the test only
    // verifies that arithmetic_sub_pure / arithmetic_mul_pure
    // / arithmetic_div_pure RETURN — it doesn't decode the
    // result for the float case.
    (void)d;
    return 1;
}

// `aura_float_ref`: dereferences a float-pool index. Not
// exercised by the test; return 0.0 as a safe default.
double aura_float_ref(std::int64_t idx) {
    (void)idx;
    return 0.0;
}

} // extern "C"
