#include <iostream>
import aura.compiler.service;
int main() {
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define a 1)\")");
    cs.eval("(arena:defrag)");
    return 0;
}
