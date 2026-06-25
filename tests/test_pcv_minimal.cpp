// Minimal PCV repro — Issue #300 follow-up #1
// Pattern that ASAN trace says crashes:
//   set-code with 1 define, then (arena:defrag), then (arena:defrag-stats)
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
    std::cout << "step 5: end (dtors will run)\n"; std::cout.flush();
    return 0;
}
