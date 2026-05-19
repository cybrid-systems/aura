// aura_jit.h — LLVM ORC JIT backend for Aura IR
#ifndef AURA_JIT_H
#define AURA_JIT_H

#include <cstdint>
#include <memory>
#include <functional>

namespace aura::jit {

// Function pointer type returned by JIT compilation.
// Takes (argc, argv) and returns int64_t.
using JitFunction = int64_t(*)();

// AuraJIT manages an LLVM ORC JIT session.
// Handles IR module compilation, symbol resolution, and caching.
class AuraJIT {
public:
    AuraJIT();
    ~AuraJIT();

    // Check if JIT is available (LLVM initialized, ORC ready).
    bool available() const;

    // Compile an empty test function that returns 42.
    JitFunction compile_empty();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aura::jit

#endif
