// Regression test for PCV heap-use-after-free.
// Issue #300 follow-up #1: ArenaStats foundation + (arena:defrag-stats).
// This test reproduces the ASAN UAF that surfaces under (arena:defrag-stats)
// after a previous (arena:defrag) call.
//
// Pattern that triggers the UAF (under ASan):
//   1. set-code with 1 define
//   2. (arena:defrag)
//   3. (arena:defrag-stats)
//
// Root cause (fixed in #300 follow-up #1): pmr::vector<PersistentChildVector>
// realloc during parse left aliased PCV slots sharing one heap control
// block with a corrupted use_count; ~FlatAST then double-freed the block.
// Fix: children_ is std::vector + release_children_for_teardown() dedupe.
//
// Run under ASan:
//   cd build_asan && cmake --build . --target test_pcv_heap_use_after_free
//   ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_stacktrace=1:halt_on_error=1 \
//     ./build_asan/test_pcv_heap_use_after_free
#include <iostream>
import aura.compiler.service;
int main() {
    std::cout << "step 1: create CS\n"; std::cout.flush();
    aura::compiler::CompilerService cs;
    std::cout << "step 2: set-code\n"; std::cout.flush();
    cs.eval("(set-code \"(define a 1)\")");
    std::cout << "step 3: defrag\n"; std::cout.flush();
    cs.eval("(arena:defrag)");
    std::cout << "step 4: defrag-stats\n"; std::cout.flush();
    auto r = cs.eval("(arena:defrag-stats)"); (void)r;
    std::cout << "step 5: about to destruct CS (UAF expected under ASan)\n"; std::cout.flush();
    return 0;
}
