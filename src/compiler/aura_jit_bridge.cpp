// aura_jit_bridge.cpp — C-linkage bridge for JIT from module context
#include "aura_jit.h"

#include <cstdio>

extern "C" int64_t aura_jit_test() {
#if AURA_HAVE_LLVM
    // D1-P2: bridge uses aura_jit_test just for smoke test
    // Full compilation happens in CompilerService::exec_jit
    return 42;
#else
    fprintf(stderr, "JIT: LLVM not available\n");
    return -1;
#endif
}
