// Minimal PCV repro — Issue #300 follow-up #1
// Pattern that ASAN trace says crashes: set-code with 2 defines
// + defrag-stats + defrag + defrag-stats
#include <iostream>
import aura.compiler.service;
int main() {
    std::cout << "step 1: create CS\n"; std::cout.flush();
    aura::compiler::CompilerService cs;
    std::cout << "step 2: set-code with 2 defines\n"; std::cout.flush();
    cs.eval("(set-code \"(define a 1) (define b 2)\")");
    std::cout << "step 3: defrag-stats\n"; std::cout.flush();
    auto r0 = cs.eval("(arena:defrag-stats)");
    (void)r0;
    std::cout << "step 4: defrag\n"; std::cout.flush();
    cs.eval("(arena:defrag)");
    std::cout << "step 5: defrag-stats again\n"; std::cout.flush();
    auto r1 = cs.eval("(arena:defrag-stats)");
    (void)r1;
    std::cout << "step 6: end (dtors will run)\n"; std::cout.flush();
    return 0;
}
