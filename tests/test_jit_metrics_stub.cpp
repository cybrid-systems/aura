// test_jit_metrics_stub.cpp — Stub for the JIT test.
//
// `aura_jit_prim_dispatch` is defined in service.ixx (a C++20
// module), which is hard to link from a non-module test binary.
// We provide a stub here that's never actually called (the
// test doesn't trigger init()).


import std;
extern "C" std::int64_t aura_jit_prim_dispatch(std::int64_t prim_id,
                                                std::int64_t* args,
                                                std::int32_t argc) {
    (void)prim_id; (void)args; (void)argc;
    return 0;
}
