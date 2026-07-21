// Regression test for PCV heap-use-after-free.
// Issue #300 follow-up #1: ArenaStats foundation + (stats:get \"arena:defrag-stats\").
// This test reproduces the ASAN UAF that surfaces under (stats:get \"arena:defrag-stats\")
// after a previous (arena:defrag) call.
//
// Pattern that triggers the UAF (under ASan):
//   1. set-code with 1 define
//   2. (arena:defrag)
//   3. (stats:get \"arena:defrag-stats\")
//
// Root cause (fixed in #300 follow-up #1): pmr::vector<PersistentChildVector>
// realloc during parse left aliased PCV slots sharing one heap control
// block with a corrupted use_count; ~FlatAST then double-freed the block.
// Fix: children_ is std::vector + release_children_for_teardown() dedupe.
//
// Run under ASan (from build_asan):
//   ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./build_asan/test_pcv_heap_use_after_free

import std;
import aura.compiler.service;
int main() {
    std::println("step 1: create CS");
    aura::compiler::CompilerService cs;
    std::println("step 2: set-code");
    cs.eval("(set-code \"(define a 1)\")");
    std::println("step 3: defrag");
    cs.eval("(arena:defrag)");
    std::println("step 4: defrag-stats");
    auto r = cs.eval("(stats:get \"arena:defrag-stats\")");
    (void)r;
    std::println("step 5: about to destruct CS (UAF expected under ASan)");
    return 0;
}
