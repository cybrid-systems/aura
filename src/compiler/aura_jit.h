// aura_jit.h — LLVM ORC JIT backend for Aura IR
#ifndef AURA_JIT_H
#define AURA_JIT_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>

namespace aura::jit {

// Flat instruction format for JIT compilation (C-compatible)
// Avoids dependency on Aura's C++ module IR types.
struct FlatInstruction {
    uint32_t opcode;  // IROpcode as uint32_t
    uint32_t ops[4];  // operands
};

struct FlatBlock {
    uint32_t id;
    const FlatInstruction* instructions;
    uint32_t num_instructions;
};

struct FlatFunction {
    const char* name;
    uint32_t entry_block;
    uint32_t local_count;
    uint32_t arg_count;
    const FlatBlock* blocks;
    uint32_t num_blocks;
};

// Function pointer type returned by JIT compilation.
using ScalarFn = int64_t(*)(int64_t*, uint32_t);

// AuraJIT manages an LLVM ORC JIT session.
class AuraJIT {
public:
    AuraJIT();
    ~AuraJIT();

    bool available() const;

    // Compile a flat IR function → native function pointer.
    // Takes (locals array, arg_count) and returns int64_t.
    ScalarFn compile(const FlatFunction& fn);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aura::jit

#endif
