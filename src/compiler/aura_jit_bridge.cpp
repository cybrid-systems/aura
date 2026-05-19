// aura_jit_bridge.cpp — C-linkage bridge for JIT from module context
#include "aura_jit.h"

#include <cstdio>

extern "C" int64_t aura_jit_test() {
#if AURA_HAVE_LLVM
    aura::jit::AuraJIT jit;
    auto fn = jit.compile_empty();
    if (!fn) {
        fprintf(stderr, "JIT: compilation failed\n");
        return -1;
    }
    auto result = fn();
    return result;
#else
    fprintf(stderr, "JIT: LLVM not available\n");
    return -1;
#endif
}
