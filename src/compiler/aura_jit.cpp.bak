// aura_jit.cpp — LLVM ORC JIT backend for Aura IR
#include "aura_jit.h"

#if AURA_HAVE_LLVM

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdio>
#include <vector>
#include <unordered_map>
#include <memory>

namespace aura::jit {

// Opcode enum values (must match ir.ixx IROpcode)
enum Op : uint32_t {
    // Data
    OpConstI64 = 1, OpConstF64 = 2, OpLocal = 3, OpArg = 4,
    // Arithmetic
    OpAdd = 5, OpSub = 6, OpMul = 7, OpDiv = 8,
    // Comparison
    OpEq = 9, OpLt = 10, OpGt = 11, OpLe = 12, OpGe = 13,
    // Logic
    OpAnd = 14, OpOr = 15, OpNot = 16,
    // Control flow
    OpBranch = 17, OpJump = 18, OpCall = 19, OpReturn = 20,
};

// ── LLVM IR Builder ───────────────────────────────────────────
// Converts Aura IR flat instructions into LLVM IR.

struct LLVMBuilder {
    llvm::LLVMContext& ctx;
    llvm::Module* mod = nullptr;
    llvm::Function* func = nullptr;
    llvm::IRBuilder<>* irb = nullptr;
    std::vector<llvm::Value*> llvm_locals;  // alloca'd slots

    // Block map: Aura block id → LLVM BasicBlock*
    std::unordered_map<uint32_t, llvm::BasicBlock*> block_map;

    bool build_function(const FlatFunction& fn) {
        // Create LLVM function type: i64 (i64*, i32)
        auto ptr_i64 = llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(ctx));
        auto i32_ty = llvm::Type::getInt32Ty(ctx);
        auto ret_ty = llvm::Type::getInt64Ty(ctx);
        auto fn_type = llvm::FunctionType::get(ret_ty, {ptr_i64, i32_ty}, false);
        func = llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage,
                                      fn.name, mod);

        // Name arguments
        auto arg_it = func->arg_begin();
        arg_it->setName("locals_ptr"); ++arg_it;
        arg_it->setName("argc"); ++arg_it;

        // Create all basic blocks first
        for (uint32_t i = 0; i < fn.num_blocks; ++i) {
            auto& fb = fn.blocks[i];
            auto bb = llvm::BasicBlock::Create(ctx, "", func);
            block_map[fb.id] = bb;
        }

        // Create alloca instructions in entry block for local slots
        auto entry_bb = block_map[fn.entry_block];
        irb = new llvm::IRBuilder<>(ctx);
        irb->SetInsertPoint(entry_bb);

        llvm::Value* locals_ptr = func->arg_begin();
        llvm::Value* argc_val = func->arg_begin() + 1;

        // Allocate local slots
        for (uint32_t i = 0; i < fn.local_count; ++i) {
            auto gep = irb->CreateGEP(
                llvm::Type::getInt64Ty(ctx), locals_ptr,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), i));
            llvm_locals.push_back(gep);
        }

        // Fill in each block
        for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
            auto& fb = fn.blocks[bi];
            auto bb = block_map[fb.id];
            irb->SetInsertPoint(bb);

            for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
                auto& inst = fb.instructions[ii];
                if (!lower_instruction(inst, fb.id, fn)) {
                    delete irb;
                    return false;
                }
            }
        }

        delete irb;
        return true;
    }

    bool lower_instruction(const FlatInstruction& inst,
                           uint32_t block_id,
                           const FlatFunction& fn) {
        auto i64 = llvm::Type::getInt64Ty(ctx);
        auto i1 = llvm::Type::getInt1Ty(ctx);
        auto i32 = llvm::Type::getInt32Ty(ctx);

        auto load = [&](uint32_t slot) -> llvm::Value* {
            return irb->CreateLoad(i64, llvm_locals[slot]);
        };
        auto store = [&](uint32_t slot, llvm::Value* val) {
            irb->CreateStore(val, llvm_locals[slot]);
        };
        auto const_i64 = [&](int64_t v) -> llvm::Value* {
            return llvm::ConstantInt::get(i64, v);
        };

        switch (inst.opcode) {
        case OpConstI64:
            store(inst.ops[0], const_i64(
                static_cast<int64_t>(inst.ops[1]) |
                (static_cast<int64_t>(inst.ops[2]) << 32)));
            return true;

        case OpConstF64:
            store(inst.ops[0], const_i64(0));  // placeholder
            return true;

        case OpLocal:
            store(inst.ops[0], load(inst.ops[1]));
            return true;

        case OpArg: {
            // Arg: result_slot, arg_index
            // Load from locals_ptr (which points to function args array)
            auto gep = irb->CreateGEP(i64, func->arg_begin(),
                         llvm::ConstantInt::get(i32, inst.ops[1]));
            auto val = irb->CreateLoad(i64, gep);
            store(inst.ops[0], val);
            return true;
        }

        case OpAdd: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            store(inst.ops[0], irb->CreateAdd(a, b));
            return true;
        }
        case OpSub: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            store(inst.ops[0], irb->CreateSub(a, b));
            return true;
        }
        case OpMul: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            store(inst.ops[0], irb->CreateMul(a, b));
            return true;
        }
        case OpDiv: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            store(inst.ops[0], irb->CreateSDiv(a, b));
            return true;
        }

        // Comparisons: result = (cmp ? 1 : 0)
        case OpEq: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            auto cmp = irb->CreateICmpEQ(a, b);
            store(inst.ops[0], irb->CreateZExt(cmp, i64));
            return true;
        }
        case OpLt: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            auto cmp = irb->CreateICmpSLT(a, b);
            store(inst.ops[0], irb->CreateZExt(cmp, i64));
            return true;
        }
        case OpGt: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            auto cmp = irb->CreateICmpSGT(a, b);
            store(inst.ops[0], irb->CreateZExt(cmp, i64));
            return true;
        }
        case OpLe: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            auto cmp = irb->CreateICmpSLE(a, b);
            store(inst.ops[0], irb->CreateZExt(cmp, i64));
            return true;
        }
        case OpGe: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            auto cmp = irb->CreateICmpSGE(a, b);
            store(inst.ops[0], irb->CreateZExt(cmp, i64));
            return true;
        }

        // Logic
        case OpAnd: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            auto az = irb->CreateICmpNE(a, zero64());
            auto bz = irb->CreateICmpNE(b, zero64());
            store(inst.ops[0], irb->CreateZExt(irb->CreateAnd(az, bz), i64));
            return true;
        }
        case OpOr: {
            auto a = load(inst.ops[1]), b = load(inst.ops[2]);
            auto az = irb->CreateICmpNE(a, zero64());
            auto bz = irb->CreateICmpNE(b, zero64());
            store(inst.ops[0], irb->CreateZExt(irb->CreateOr(az, bz), i64));
            return true;
        }
        case OpNot: {
            auto a = load(inst.ops[1]);
            auto az = irb->CreateICmpEQ(a, zero64());
            store(inst.ops[0], irb->CreateZExt(az, i64));
            return true;
        }

        case OpBranch: {
            // Branch: cond, true_block, false_block
            auto cond = load(inst.ops[0]);
            auto cond_bool = irb->CreateICmpNE(cond, zero64());
            auto true_bb = block_map[inst.ops[1]];
            auto false_bb = block_map[inst.ops[2]];
            irb->CreateCondBr(cond_bool, true_bb, false_bb);
            return true;
        }

        case OpJump: {
            auto target_bb = block_map[inst.ops[0]];
            irb->CreateBr(target_bb);
            return true;
        }

        case OpReturn: {
            auto val = load(inst.ops[0]);
            irb->CreateRet(val);
            return true;
        }

        default:
            // Unsupported opcodes: store 0 and continue
            if (inst.ops[0] < fn.local_count) {
                store(inst.ops[0], zero64());
            }
            return true;  // skip for now
        }
    }

    llvm::Value* zero64() {
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 0);
    }
};

// ── AuraJIT implementation ────────────────────────────────────

struct AuraJIT::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    llvm::orc::JITDylib* main_dylib = nullptr;
    llvm::LLVMContext ctx;
    uint64_t module_counter = 0;
    bool initialized = false;

    bool init() {
        if (initialized) return true;
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        auto jit_or_err = llvm::orc::LLJITBuilder().create();
        if (!jit_or_err) {
            fprintf(stderr, "JIT: failed to create LLJIT\n");
            return false;
        }
        jit = std::move(*jit_or_err);
        main_dylib = &jit->getMainJITDylib();
        initialized = true;
        return true;
    }

    ScalarFn compile(const FlatFunction& fn) {
        if (!init()) return nullptr;

        auto mod = std::make_unique<llvm::Module>(
            std::string("mod_") + std::to_string(module_counter++), ctx);

        LLVMBuilder builder{ctx};
        builder.mod = mod.get();
        if (!builder.build_function(fn)) return nullptr;

        if (llvm::verifyModule(*mod, &llvm::errs())) {
            fprintf(stderr, "JIT: module verification failed\n");
            return nullptr;
        }

        auto tsm = llvm::orc::ThreadSafeModule(
            std::move(mod), std::make_unique<llvm::LLVMContext>());
        if (auto err = jit->addIRModule(std::move(tsm))) {
            fprintf(stderr, "JIT: failed to add module\n");
            return nullptr;
        }

        auto sym = jit->lookup(std::string(fn.name));
        if (!sym) {
            fprintf(stderr, "JIT: symbol lookup failed\n");
            return nullptr;
        }
        return sym->toPtr<ScalarFn>();
    }
};

// ── Public API ────────────────────────────────────────────────

AuraJIT::AuraJIT() : impl_(std::make_unique<Impl>()) {}
AuraJIT::~AuraJIT() = default;
bool AuraJIT::available() const { return impl_->initialized; }
ScalarFn AuraJIT::compile(const FlatFunction& fn) { return impl_->compile(fn); }

} // namespace aura::jit

#else // !AURA_HAVE_LLVM

namespace aura::jit {

AuraJIT::AuraJIT() : impl_(nullptr) {}
AuraJIT::~AuraJIT() = default;
bool AuraJIT::available() const { return false; }
ScalarFn AuraJIT::compile(const FlatFunction&) { return nullptr; }

} // namespace aura::jit

#endif
