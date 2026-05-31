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
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h>
#include <llvm/ExecutionEngine/Orc/ExecutorProcessControl.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/LegacyPassManager.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>

namespace aura::jit {

// PrimId values (must match ir.ixx PrimId enum order)
// Used for PrimCall fast-path dispatch — skipping aura_prim_call runtime.
enum : uint32_t {
    PrimDisplay = 8,
    PrimWrite = 9,
    PrimNewline = 10,
    PrimQuotient = 30,
    PrimRemainder = 31,
    PrimRaise = 35,
    PrimErrorP = 36,
    PrimPairP = 37,
    PrimNullP = 38,
};

// Opcode enum values (must match ir.ixx IROpcode)
enum Op : uint32_t {
    OpConstI64 = 1,
    OpConstF64 = 2,
    OpLocal = 3,
    OpArg = 4,
    OpAdd = 5,
    OpSub = 6,
    OpMul = 7,
    OpDiv = 8,
    OpEq = 9,
    OpLt = 10,
    OpGt = 11,
    OpLe = 12,
    OpGe = 13,
    OpAnd = 14,
    OpOr = 15,
    OpNot = 16,
    OpBranch = 17,
    OpJump = 18,
    OpCall = 19,
    OpReturn = 20,
    OpMakeClosure = 21,
    OpCapture = 22,
    OpCaptureRef = 23,
    OpApply = 24,
    OpNewCell = 25,
    OpCellSet = 26,
    OpCellGet = 27,
    OpCastOp = 28,
    OpConstString = 29,
    OpPrimCall = 30,
    OpPrimitive = 31,
    OpConstBool = 32,
    OpConstVoid = 33,
    OpMakePair = 34,
    OpCar = 35,
    OpCdr = 36,
    OpRaise = 37,
    OpIsError = 38,
    OpDrop = 39,
    // M4 Linear ownership opcodes (must match ir.ixx IROpcode values)
    // DropOp is 41 in ir.ixx — handle alongside OpDrop
};

// Map IROpcode::DropOp (41) to the OpDrop handler.
// In the lowering pass, DropOp is emitted for explicit destructors.
// The LLVM builder handles both opcode values the same way.

// LLVM IR Builder
struct LLVMBuilder {
    llvm::LLVMContext& ctx;
    llvm::Module* mod = nullptr;
    llvm::Function* func = nullptr;
    llvm::IRBuilder<>* irb = nullptr;
    std::vector<llvm::Value*> llvm_locals{};
    std::unordered_map<uint32_t, llvm::BasicBlock*> block_map{};

    // AOT mode: OpPrimitive stores negative sentinel for primitive dispatch
    bool aot_mode = false;
    // Shape map from FlatFunction: null = dynamic, non-null = known shapes
    // Values: 0=Dynamic, 1=Int, 2=Float, 3=Bool, 4=String, 5=Void
    const uint8_t* shape_map = nullptr;
    uint32_t shape_map_size = 0;

    // Pointer tagging constants (must match lib/runtime.c)
    static constexpr int64_t KWD_TRUE_VAL = 7;
    static constexpr int64_t KWD_FALSE_VAL = 3;
    static constexpr int64_t FLOAT_BIAS_VAL = -10000000000000000LL;
    llvm::Type* double_ty = llvm::Type::getDoubleTy(ctx);
    // String pool for OpConstString (IR module's string pool content)
    const std::vector<std::string>* string_pool = nullptr;

    // Runtime function declarations
    llvm::Function* fn_alloc_closure = nullptr;
    llvm::Function* fn_closure_capture = nullptr;
    llvm::Function* fn_closure_call = nullptr;
    llvm::Function* fn_new_cell = nullptr;
    llvm::Function* fn_cell_get = nullptr;
    llvm::Function* fn_cell_set = nullptr;
    llvm::Function* fn_alloc_pair = nullptr;
    llvm::Function* fn_alloc_pair_arena = nullptr;
    llvm::Function* fn_pair_car = nullptr;
    llvm::Function* fn_pair_cdr = nullptr;
    llvm::Function* fn_prim_call = nullptr;
    llvm::Function* fn_display_int = nullptr;
    llvm::Function* fn_display_char = nullptr;
    llvm::Function* fn_newline = nullptr;
    llvm::Function* fn_cast_op = nullptr;
    llvm::Function* fn_bump_init = nullptr;
    llvm::Function* fn_bump_reset = nullptr;
    llvm::Function* fn_register_closure = nullptr;
    llvm::Function* fn_drop_pair = nullptr;
    llvm::Function* fn_drop_cell = nullptr;
    llvm::Function* fn_drop_closure = nullptr;
    llvm::Function* fn_drop_value = nullptr;
    llvm::Function* fn_alloc_float = nullptr;
    llvm::Function* fn_float_ref = nullptr;
    llvm::Function* fn_alloc_string = nullptr;
    llvm::Function* fn_string_ref = nullptr;
    // L2 specialization: unchecked pair access (skips tag check)
    llvm::Function* fn_pair_car_unchecked = nullptr;
    llvm::Function* fn_pair_cdr_unchecked = nullptr;

    void declare_runtime() {
        auto i64 = llvm::Type::getInt64Ty(ctx);
        auto ptr_i64 = llvm::PointerType::getUnqual(i64);
        auto void_ty = llvm::Type::getVoidTy(ctx);
        auto i8_ty = llvm::Type::getInt8Ty(ctx);
        auto ptr_i8 = llvm::PointerType::getUnqual(i8_ty);

        fn_alloc_closure =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_alloc_closure", mod);

        fn_closure_capture =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64, i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_closure_capture", mod);

        fn_closure_call =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64, ptr_i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_closure_call", mod);

        fn_new_cell = llvm::Function::Create(llvm::FunctionType::get(i64, false),
                                             llvm::Function::ExternalLinkage, "aura_new_cell", mod);

        fn_cell_get = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_cell_get", mod);

        fn_cell_set = llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64, i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_cell_set", mod);

        fn_alloc_pair =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_alloc_pair", mod);

        fn_alloc_pair_arena =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_alloc_pair_arena", mod);

        fn_pair_car = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_pair_car", mod);

        fn_pair_cdr = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_pair_cdr", mod);

        fn_pair_car_unchecked = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_pair_car_unchecked", mod);

        fn_pair_cdr_unchecked = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_pair_cdr_unchecked", mod);

        fn_prim_call =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64, i64, i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_prim_call", mod);

        fn_display_int =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_display_int", mod);

        fn_display_char =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i8_ty}, false),
                                   llvm::Function::ExternalLinkage, "aura_display_char", mod);

        fn_newline = llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                            llvm::Function::ExternalLinkage, "aura_newline", mod);

        fn_cast_op = llvm::Function::Create(llvm::FunctionType::get(i64, {i64, i64}, false),
                                            llvm::Function::ExternalLinkage, "aura_cast_op", mod);

        // Bump Allocator lifecycle
        fn_bump_init = llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                             llvm::Function::ExternalLinkage, "aura_bump_init", mod);
        fn_bump_reset = llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                               llvm::Function::ExternalLinkage, "aura_bump_reset", mod);

        // Drop functions
        fn_drop_pair = llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64}, false),
                                               llvm::Function::ExternalLinkage, "aura_drop_pair", mod);
        fn_drop_cell = llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64}, false),
                                               llvm::Function::ExternalLinkage, "aura_drop_cell", mod);
        fn_drop_closure = llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64}, false),
                                                  llvm::Function::ExternalLinkage, "aura_drop_closure", mod);

        // String functions
        fn_alloc_string =
            llvm::Function::Create(llvm::FunctionType::get(i64, {ptr_i8}, false),
                                   llvm::Function::ExternalLinkage, "aura_alloc_string", mod);
        fn_alloc_float = llvm::Function::Create(
                                   llvm::FunctionType::get(i64, {llvm::Type::getDoubleTy(ctx)}, false),
                                   llvm::Function::ExternalLinkage, "aura_alloc_float", mod);
        fn_float_ref = llvm::Function::Create(
                                   llvm::FunctionType::get(llvm::Type::getDoubleTy(ctx), {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_float_ref", mod);
        fn_string_ref =
            llvm::Function::Create(llvm::FunctionType::get(ptr_i8, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_string_ref", mod);

        // Closure registration (JIT linking)
        fn_register_closure =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64, i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_register_fn", mod);
    }

    llvm::Value* load(uint32_t slot) {
        return irb->CreateLoad(llvm::Type::getInt64Ty(ctx), llvm_locals[slot]);
    }
    void store(uint32_t slot, llvm::Value* val) { irb->CreateStore(val, llvm_locals[slot]); }
    llvm::Value* c64(int64_t v) { return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), v); }

    bool lower(const FlatInstruction& inst, uint32_t block_id, const FlatFunction& fn) {
        (void)block_id;
        (void)fn;
        switch (inst.opcode) {
            case OpConstI64: {
                // Fixnum-encode: value << 1 (matching EvalValue pointer tagging)
                auto raw = c64(static_cast<int64_t>(inst.ops[1]) |
                               (static_cast<int64_t>(inst.ops[2]) << 32));
                store(inst.ops[0], irb->CreateShl(raw, c64(1)));
                return true;
            }
            case OpLocal:
                store(inst.ops[0], load(inst.ops[1]));
                return true;
            case OpArg: {
                auto gep = irb->CreateGEP(llvm::Type::getInt64Ty(ctx), func->arg_begin(),
                                          c64(inst.ops[1]));
                store(inst.ops[0], irb->CreateLoad(llvm::Type::getInt64Ty(ctx), gep));
                return true;
            }
            case OpAdd: {
                auto a = load(inst.ops[1]), b = load(inst.ops[2]);
                // L1 specialization: if both args known Int, skip float check
                bool spec_int = (shape_map &&
                    inst.ops[1] < shape_map_size && shape_map[inst.ops[1]] == 1 &&
                    inst.ops[2] < shape_map_size && shape_map[inst.ops[2]] == 1);
                if (spec_int) {
                    store(inst.ops[0], irb->CreateAdd(a, b));
                    return true;
                }
                auto a_is_float = irb->CreateICmpSLE(a, c64(FLOAT_BIAS_VAL));
                auto b_is_float = irb->CreateICmpSLE(b, c64(FLOAT_BIAS_VAL));
                auto any_float = irb->CreateOr(a_is_float, b_is_float);
                auto a_dbl = irb->CreateSelect(a_is_float,
                    irb->CreateCall(fn_float_ref, {a}),
                    irb->CreateSIToFP(irb->CreateAShr(a, c64(1)), double_ty));
                auto b_dbl = irb->CreateSelect(b_is_float,
                    irb->CreateCall(fn_float_ref, {b}),
                    irb->CreateSIToFP(irb->CreateAShr(b, c64(1)), double_ty));
                auto fsum = irb->CreateFAdd(a_dbl, b_dbl);
                auto float_res = irb->CreateCall(fn_alloc_float, {fsum});
                auto fixnum_res = irb->CreateAdd(a, b);
                store(inst.ops[0], irb->CreateSelect(any_float, float_res, fixnum_res));
                return true;
            }
            case OpSub: {
                auto a = load(inst.ops[1]), b = load(inst.ops[2]);
                bool spec_int = (shape_map &&
                    inst.ops[1] < shape_map_size && shape_map[inst.ops[1]] == 1 &&
                    inst.ops[2] < shape_map_size && shape_map[inst.ops[2]] == 1);
                if (spec_int) {
                    store(inst.ops[0], irb->CreateSub(a, b));
                    return true;
                }
                auto a_is_float = irb->CreateICmpSLE(a, c64(FLOAT_BIAS_VAL));
                auto b_is_float = irb->CreateICmpSLE(b, c64(FLOAT_BIAS_VAL));
                auto any_float = irb->CreateOr(a_is_float, b_is_float);
                auto a_dbl = irb->CreateSelect(a_is_float,
                    irb->CreateCall(fn_float_ref, {a}),
                    irb->CreateSIToFP(irb->CreateAShr(a, c64(1)), double_ty));
                auto b_dbl = irb->CreateSelect(b_is_float,
                    irb->CreateCall(fn_float_ref, {b}),
                    irb->CreateSIToFP(irb->CreateAShr(b, c64(1)), double_ty));
                auto fsub = irb->CreateFSub(a_dbl, b_dbl);
                auto float_res = irb->CreateCall(fn_alloc_float, {fsub});
                auto fixnum_res = irb->CreateSub(a, b);
                store(inst.ops[0], irb->CreateSelect(any_float, float_res, fixnum_res));
                return true;
            }
            case OpMul: {
                auto a = load(inst.ops[1]), b = load(inst.ops[2]);
                bool spec_int = (shape_map &&
                    inst.ops[1] < shape_map_size && shape_map[inst.ops[1]] == 1 &&
                    inst.ops[2] < shape_map_size && shape_map[inst.ops[2]] == 1);
                if (spec_int) {
                    // Tagged fixnums: (a*b) >> 1
                    store(inst.ops[0], irb->CreateAShr(irb->CreateMul(a, b), c64(1)));
                    return true;
                }
                auto a_is_float = irb->CreateICmpSLE(a, c64(FLOAT_BIAS_VAL));
                auto b_is_float = irb->CreateICmpSLE(b, c64(FLOAT_BIAS_VAL));
                auto any_float = irb->CreateOr(a_is_float, b_is_float);
                auto a_dbl = irb->CreateSelect(a_is_float,
                    irb->CreateCall(fn_float_ref, {a}),
                    irb->CreateSIToFP(irb->CreateAShr(a, c64(1)), double_ty));
                auto b_dbl = irb->CreateSelect(b_is_float,
                    irb->CreateCall(fn_float_ref, {b}),
                    irb->CreateSIToFP(irb->CreateAShr(b, c64(1)), double_ty));
                auto fmul = irb->CreateFMul(a_dbl, b_dbl);
                auto float_res = irb->CreateCall(fn_alloc_float, {fmul});
                // Fixnum path: tagged fixnums (val<<1), multiply → (a*b)>>1
                auto fixnum_res = irb->CreateAShr(irb->CreateMul(a, b), c64(1));
                store(inst.ops[0], irb->CreateSelect(any_float, float_res, fixnum_res));
                return true;
            }
            case OpDiv: {
                auto dividend = load(inst.ops[1]);
                auto divisor = load(inst.ops[2]);
                bool spec_int = (shape_map &&
                    inst.ops[1] < shape_map_size && shape_map[inst.ops[1]] == 1 &&
                    inst.ops[2] < shape_map_size && shape_map[inst.ops[2]] == 1);
                if (spec_int) {
                    auto is_zero = irb->CreateICmpEQ(divisor, c64(0));
                    auto safe_div = irb->CreateSelect(is_zero, c64(2), divisor);  // fixnum 1
                    auto div_result = irb->CreateSDiv(dividend, safe_div);
                    auto safe = irb->CreateSelect(is_zero, c64(0), div_result);
                    store(inst.ops[0], irb->CreateShl(safe, c64(1)));
                    return true;
                }
                auto a_is_float = irb->CreateICmpSLE(dividend, c64(FLOAT_BIAS_VAL));
                auto b_is_float = irb->CreateICmpSLE(divisor, c64(FLOAT_BIAS_VAL));
                auto any_float = irb->CreateOr(a_is_float, b_is_float);
                auto a_dbl = irb->CreateSelect(a_is_float,
                    irb->CreateCall(fn_float_ref, {dividend}),
                    irb->CreateSIToFP(irb->CreateAShr(dividend, c64(1)), double_ty));
                auto b_dbl = irb->CreateSelect(b_is_float,
                    irb->CreateCall(fn_float_ref, {divisor}),
                    irb->CreateSIToFP(irb->CreateAShr(divisor, c64(1)), double_ty));
                auto is_zero_f = irb->CreateFCmpOEQ(b_dbl, llvm::ConstantFP::get(double_ty, 0.0));
                auto safe_b = irb->CreateSelect(is_zero_f, llvm::ConstantFP::get(double_ty, 1.0), b_dbl);
                auto fdiv = irb->CreateFDiv(a_dbl, safe_b);
                auto float_res = irb->CreateCall(fn_alloc_float, {fdiv});
                auto is_zero = irb->CreateICmpEQ(divisor, c64(0));
                auto safe_div = irb->CreateSelect(is_zero, c64(1), divisor);
                auto div_result = irb->CreateSDiv(dividend, safe_div);
                auto safe = irb->CreateSelect(is_zero, c64(0), div_result);
                auto fixnum_res = irb->CreateShl(safe, c64(1));
                store(inst.ops[0], irb->CreateSelect(any_float, float_res, fixnum_res));
                return true;
            }
            case OpEq: {
                auto c = irb->CreateICmpEQ(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateSelect(c, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpLt: {
                auto c = irb->CreateICmpSLT(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateSelect(c, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpGt: {
                auto c = irb->CreateICmpSGT(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateSelect(c, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpLe: {
                auto c = irb->CreateICmpSLE(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateSelect(c, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpGe: {
                auto c = irb->CreateICmpSGE(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateSelect(c, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpAnd: {
                // Truthiness: #f (val=3) and integer 0 (val=0) are both falsy.
                auto is_truthy_fn = [&](llvm::Value* v) -> llvm::Value* {
                    auto not_false = irb->CreateICmpNE(v, c64(KWD_FALSE_VAL));
                    auto not_zero = irb->CreateICmpNE(v, c64(0));
                    return irb->CreateAnd(not_false, not_zero);
                };
                auto a = is_truthy_fn(load(inst.ops[1]));
                auto b = is_truthy_fn(load(inst.ops[2]));
                store(inst.ops[0],
                      irb->CreateSelect(irb->CreateAnd(a, b), c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpOr: {
                auto is_truthy_fn = [&](llvm::Value* v) -> llvm::Value* {
                    auto not_false = irb->CreateICmpNE(v, c64(KWD_FALSE_VAL));
                    auto not_zero = irb->CreateICmpNE(v, c64(0));
                    return irb->CreateOr(not_false, not_zero);
                };
                auto a = is_truthy_fn(load(inst.ops[1]));
                auto b = is_truthy_fn(load(inst.ops[2]));
                store(inst.ops[0],
                      irb->CreateSelect(irb->CreateOr(a, b), c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpNot: {
                // #f and int 0 are both falsy → not is true only for those
                auto v = load(inst.ops[1]);
                auto eq_false = irb->CreateICmpEQ(v, c64(KWD_FALSE_VAL));
                auto eq_zero = irb->CreateICmpEQ(v, c64(0));
                auto falsy = irb->CreateOr(eq_false, eq_zero);
                store(inst.ops[0],
                      irb->CreateSelect(falsy, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpBranch: {
                // Truthiness: #f (val=3) and integer 0 (val=0) are both falsy.
                auto cond_val = load(inst.ops[0]);
                auto not_false = irb->CreateICmpNE(cond_val, c64(KWD_FALSE_VAL));
                auto not_int0 = irb->CreateICmpNE(cond_val, c64(0));
                auto truthy = irb->CreateAnd(not_false, not_int0);
                irb->CreateCondBr(truthy, block_map[inst.ops[1]], block_map[inst.ops[2]]);
                return true;
            }
            case OpJump:
                irb->CreateBr(block_map[inst.ops[0]]);
                return true;
            case OpReturn:
                irb->CreateRet(load(inst.ops[0]));
                return true;
            case OpConstBool: {
                // Pointer tagging: #t = 7, #f = 3 (matching EvalValue)
                store(inst.ops[0], c64(inst.ops[1] ? KWD_TRUE_VAL : KWD_FALSE_VAL));
                return true;
            }
            case OpConstVoid: {
                // Pointer tagging: void = 11 (matching EvalValue)
                store(inst.ops[0], c64(11));
                return true;
            }
            case OpConstF64: {
                // Reconstruct double from operand bits (low, high).
                // Store in float pool via aura_alloc_float for proper tagged encoding.
                std::uint64_t bits = static_cast<std::uint64_t>(inst.ops[1]) |
                                     (static_cast<std::uint64_t>(inst.ops[2]) << 32);
                double d;
                std::memcpy(&d, &bits, sizeof(d));
                auto fp = llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx), d);
                auto call = irb->CreateCall(fn_alloc_float, {fp});
                store(inst.ops[0], call);
                return true;
            }
            case OpConstString: {
                // Load string content from IR module's string pool
                std::string_view str_content;
                if (string_pool && inst.ops[1] < string_pool->size())
                    str_content = (*string_pool)[inst.ops[1]];
                auto str_ptr = irb->CreateGlobalStringPtr(str_content);
                auto call = irb->CreateCall(fn_alloc_string, {str_ptr});
                store(inst.ops[0], call);
                return true;
            }

            // Closures
            case OpMakeClosure: {
                auto call = irb->CreateCall(fn_alloc_closure, {c64(inst.ops[1])});
                store(inst.ops[0], call);
                return true;
            }
            case OpCapture: {
                // ops[0] = closure_slot, ops[1] = env_idx, ops[2] = var_slot
                auto closure_val = load(inst.ops[0]);
                auto env_val = load(inst.ops[2]);
                irb->CreateCall(fn_closure_capture, {closure_val, c64(inst.ops[1]), env_val});
                return true;
            }
            case OpCall: {
                // ops[0] = callee_slot, ops[1] = arg_base, ops[2] = arg_count, ops[3] = result_slot
                auto callee = load(inst.ops[0]);
                auto arg_base = inst.ops[1];
                auto arg_count = inst.ops[2];
                auto alloca_ty = llvm::ArrayType::get(llvm::Type::getInt64Ty(ctx), arg_count);
                auto args_arr = irb->CreateAlloca(alloca_ty);
                for (uint32_t i = 0; i < arg_count; ++i) {
                    auto gep = irb->CreateGEP(llvm::Type::getInt64Ty(ctx), args_arr, c64(i));
                    irb->CreateStore(load(arg_base + i), gep);
                }
                auto call = irb->CreateCall(
                    fn_closure_call,
                    {callee,
                     irb->CreateBitCast(args_arr,
                                        llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(ctx))),
                     c64(arg_count)});
                store(inst.ops[3], call);
                return true;
            }

            // Cells
            case OpNewCell: {
                auto call = irb->CreateCall(fn_new_cell);
                store(inst.ops[0], call);
                return true;
            }
            case OpCellGet: {
                auto call = irb->CreateCall(fn_cell_get, {load(inst.ops[1])});
                store(inst.ops[0], call);
                return true;
            }
            case OpCellSet: {
                irb->CreateCall(fn_cell_set, {load(inst.ops[0]), load(inst.ops[1])});
                return true;
            }

            // Drop: call all safe drop functions (runtime's live-flag is idempotent)
            // ── M4 Linear ownership ops (IROpcode values from ir.ixx) ─
            // LinearWrap=39, MoveOp=40, BorrowOp=41, MutBorrowOp=42,
            // DropOp=43, RefCountOp=44
            // For untagged AOT runtime:
            //   LinearWrap/MoveOp/BorrowOp/MutBorrowOp/RefCountOp =
            //     no-ops (compile-time concepts, pass through)
            //   DropOp (43) = actually calls drop functions

            case 39: /* IROpcode::LinearWrap */
            case 40: /* IROpcode::MoveOp */
            case 41: /* IROpcode::BorrowOp */
            case 42: /* IROpcode::MutBorrowOp */
            case 44: /* IROpcode::RefCountOp */ {
                store(inst.ops[0], load(inst.ops[1]));
                return true;
            }
            case 43: /* IROpcode::DropOp */ {
                auto val = load(inst.ops[0]);
                irb->CreateCall(fn_drop_pair, {val});
                irb->CreateCall(fn_drop_cell, {val});
                irb->CreateCall(fn_drop_closure, {val});
                return true;
            }

            // Pairs
            case OpMakePair: {
                auto car = load(inst.ops[1]);
                auto cdr = load(inst.ops[2]);
                auto result_slot = inst.ops[0];
                // Check escape analysis: non-escaping pairs use arena allocation
                if (fn.escape_map && result_slot < fn.local_count && !fn.escape_map[result_slot]) {
                    // NON_ESCAPING: allocate from TL arena
                    auto call = irb->CreateCall(fn_alloc_pair_arena, {car, cdr});
                    store(result_slot, call);
                } else {
                    // ESCAPED or unknown: allocate from global heap
                    auto call = irb->CreateCall(fn_alloc_pair, {car, cdr});
                    store(result_slot, call);
                }
                return true;
            }
            case OpCar: {
                auto pair_val = load(inst.ops[1]);
                // L2 specialization: if pair arg known to be Pair, call unchecked variant
                bool spec_pair = (shape_map &&
                    inst.ops[1] < shape_map_size && shape_map[inst.ops[1]] == 10);
                auto car_fn = spec_pair ? fn_pair_car_unchecked : fn_pair_car;
                auto call = irb->CreateCall(car_fn, {pair_val});
                store(inst.ops[0], call);
                return true;
            }
            case OpCdr: {
                auto pair_val = load(inst.ops[1]);
                // L2 specialization: if pair arg known to be Pair, call unchecked variant
                bool spec_pair = (shape_map &&
                    inst.ops[1] < shape_map_size && shape_map[inst.ops[1]] == 10);
                auto cdr_fn = spec_pair ? fn_pair_cdr_unchecked : fn_pair_cdr;
                auto call = irb->CreateCall(cdr_fn, {pair_val});
                store(inst.ops[0], call);
                return true;
            }

            // CastOp: runtime type check
            case OpCastOp: {
                // ops[0] = result_slot, ops[1] = value_slot, ops[2] = type_tag
                auto val = load(inst.ops[1]);
                auto call = irb->CreateCall(fn_cast_op, {val, c64(inst.ops[2])});
                store(inst.ops[0], call);
                return true;
            }

            // Primitive calls
            case OpPrimCall: {
                // IR: operands[0]=prim_id, operands[1]=arg_base, operands[2]=arg_count,
                //     operands[3]=result_slot
                auto prim_id = inst.ops[0];
                auto arg_base = inst.ops[1];
                auto arg_count = inst.ops[2];
                auto result_slot = inst.ops[3];
                auto a1 = (arg_count > 0) ? load(arg_base) : c64(0);
                auto a2 = (arg_count > 1) ? load(arg_base + 1) : c64(0);

                // Fast-path: inline known primitives to skip aura_prim_call dispatch
                switch (prim_id) {
                case PrimNewline:
                    irb->CreateCall(fn_newline);
                    store(result_slot, c64(0));
                    return true;
                case PrimDisplay:
                case PrimWrite:
                    irb->CreateCall(fn_display_int, {a1});
                    store(result_slot, a1);
                    return true;
                case PrimQuotient: {
                    // Both args are fixnum-encoded (value << 1). SDiv cancels the shift,
                    // so we get the correct integer result. Shift back for fixnum encoding.
                    auto zero = irb->CreateICmpEQ(a2, c64(0));
                    auto div = irb->CreateSDiv(a1, a2);
                    auto safe = irb->CreateSelect(zero, c64(0), div);
                    store(result_slot, irb->CreateShl(safe, c64(1)));
                    return true;
                }
                case PrimRemainder: {
                    auto zero = irb->CreateICmpEQ(a2, c64(0));
                    auto rem = irb->CreateSRem(a1, a2);
                    auto safe = irb->CreateSelect(zero, c64(0), rem);
                    // a1/a2 fixnum-encoded, SRem preserves fixnum encoding
                    store(result_slot, safe);
                    return true;
                }
                case PrimPairP: {
                    // Pointer tagging: low 2 bits = 01 means pair
                    auto masked = irb->CreateAnd(a1, c64(3));
                    auto is_pair = irb->CreateICmpEQ(masked, c64(1));
                    store(result_slot, irb->CreateSelect(is_pair, c64(7), c64(3)));
                    return true;
                }
                case PrimNullP: {
                    // Void sentinel (11) or fixnum 0 (from () → LiteralInt 0) are null
                    auto is_void = irb->CreateICmpEQ(a1, c64(11));
                    auto is_zero = irb->CreateICmpEQ(a1, c64(0));
                    auto is_null = irb->CreateOr(is_void, is_zero);
                    store(result_slot, irb->CreateSelect(is_null, c64(7), c64(3)));
                    return true;
                }
                default:
                    break;
                }

                // Slow-path: generic aura_prim_call dispatch
                auto call = irb->CreateCall(fn_prim_call, {c64(prim_id), a1, a2, c64(arg_count)});
                store(result_slot, call);
                return true;
            }
            case OpPrimitive: {
                // IR: operands[0]=result_slot, operands[1]=prim_slot_index
                auto result_slot = inst.ops[0];
                auto prim_slot = inst.ops[1];
                if (aot_mode) {
                    // AOT: store negative sentinel for primitive dispatch.
                    // aura_closure_call will recognize negative values as primitive
                    // references and dispatch to the runtime's primitive table.
                    // Encoding: -(prim_slot + 1)
                    store(result_slot, c64(-((int64_t)prim_slot + 1)));
                } else {
                    // JIT: call through evaluator's primitive dispatcher
                    auto call = irb->CreateCall(fn_prim_call, {c64(prim_slot), c64(0), c64(0), c64(0)});
                    store(result_slot, call);
                }
                return true;
            }

            default:
                if (inst.ops[0] < fn.local_count)
                    store(inst.ops[0], c64(0));
                return true;
        }
    }
};

// ── Escape Analysis (backward dataflow) ───────────────────────

// Run backward escape analysis on flat IR. Fills escape_map (size = local_count).
// Each entry: 0 = NON_ESCAPING, 1 = ESCAPED.
// Escape points: Return(value), Call(callee, args...), Capture(env, val),
// Store(hash, key, val), CellSet(cell, val).
void run_escape_analysis(
    const std::vector<std::vector<FlatInstruction>>& flat_instrs,
    uint32_t local_count,
    std::vector<uint8_t>& escape_map)
{
    enum Op : uint32_t {
        OpConstI64 = 1, OpConstF64 = 2, OpLocal = 3, OpArg = 4,
        OpAdd = 5, OpSub = 6, OpMul = 7, OpDiv = 8,
        OpEq = 9, OpLt = 10, OpGt = 11, OpLe = 12, OpGe = 13,
        OpAnd = 14, OpOr = 15, OpNot = 16,
        OpBranch = 17, OpJump = 18, OpCall = 19, OpReturn = 20,
        OpMakeClosure = 21, OpCapture = 22, OpCaptureRef = 23, OpApply = 24,
        OpNewCell = 25, OpCellSet = 26, OpCellGet = 27,
        OpCastOp = 28, OpConstString = 29,
        OpPrimitive = 30, OpPrimCall = 31,
        OpCellAlloc = 32, OpMakePair = 34, OpCar = 35, OpCdr = 36,
        OpMakeVector = 37, OpVectorRef = 38, OpVectorSet = 39,
        OpDrop = 40, OpDropOp = 41, OpMakeClosureOp = 42, OpCaptureOp = 43,
        OpMakeRef = 44, OpRefGet = 45, OpRefSet = 46,
    };

    escape_map.assign(local_count, 0);

    // First pass: mark escape points
    for (auto& block : flat_instrs) {
        for (auto& inst : block) {
            uint32_t result = inst.ops[0];
            switch (inst.opcode) {
            case OpReturn:
                // Return(value) — value escapes
                if (inst.ops[0] < local_count)
                    escape_map[inst.ops[0]] = 1;
                break;
            case OpCall:
                // Call(callee, arg_base, arg_count, result) — callee + all args escape
                {
                    uint32_t callee = inst.ops[0];
                    uint32_t arg_base = inst.ops[1];
                    uint32_t arg_count = inst.ops[2];
                    if (callee < local_count)
                        escape_map[callee] = 1;
                    for (uint32_t i = 0; i < arg_count && (arg_base + i) < local_count; ++i)
                        escape_map[arg_base + i] = 1;
                }
                break;
            case OpCapture:
            case OpCaptureRef:
            case OpCaptureOp:
                // Capture(result, env_ptr_or_val) — captured value escapes
                if (inst.ops[1] < local_count)
                    escape_map[inst.ops[1]] = 1;
                break;
            case OpCellSet:
                // CellSet(cell, val) — val escapes
                if (inst.ops[1] < local_count)
                    escape_map[inst.ops[1]] = 1;
                break;
            case OpPrimCall:
                // PrimCall(slot, a, b, count) — call args escape
                {
                    uint32_t a_op = inst.ops[1];
                    uint32_t b_op = inst.ops[2];
                    if (a_op < local_count) escape_map[a_op] = 1;
                    if (b_op < local_count) escape_map[b_op] = 1;
                }
                break;
            default:
                break;
            }
        }
    }

    // Backward propagation: if a result escapes, its operands escape
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& block : flat_instrs) {
            for (auto& inst : block) {
                uint32_t result = inst.ops[0];
                if (result >= local_count) continue;
                if (!escape_map[result]) continue; // result doesn't escape

                switch (inst.opcode) {
                case OpLocal:
                    // Local(result, src) — src escapes
                    if (inst.ops[1] < local_count && !escape_map[inst.ops[1]]) {
                        escape_map[inst.ops[1]] = 1;
                        changed = true;
                    }
                    break;
                case OpMakePair:
                    // MakePair(result, car, cdr) — car + cdr escape
                    if (inst.ops[1] < local_count && !escape_map[inst.ops[1]]) {
                        escape_map[inst.ops[1]] = 1;
                        changed = true;
                    }
                    if (inst.ops[2] < local_count && !escape_map[inst.ops[2]]) {
                        escape_map[inst.ops[2]] = 1;
                        changed = true;
                    }
                    break;
                case OpCall:
                    // Call(callee, arg_base, count, result) — propagate back to callee
                    // (already marked in first pass)
                    break;
                default:
                    break;
                }
            }
        }
    }
}

// ── AuraJIT implementation ────────────────────────────────────

// Runtime function declarations (C linkage, defined in aura_jit_runtime.cpp)
extern "C" {
int64_t aura_alloc_closure(int64_t);
void aura_closure_capture(int64_t, int64_t, int64_t);
int64_t aura_closure_call(int64_t, int64_t*, int64_t);
int64_t aura_new_cell();
int64_t aura_cell_get(int64_t);
void aura_cell_set(int64_t, int64_t);
int64_t aura_alloc_pair(int64_t, int64_t);
int64_t aura_alloc_pair_arena(int64_t, int64_t);
int64_t aura_pair_car(int64_t);
int64_t aura_pair_cdr(int64_t);
int64_t aura_pair_car_unchecked(int64_t);
int64_t aura_pair_cdr_unchecked(int64_t);
int64_t aura_prim_call(int64_t, int64_t, int64_t, int64_t);
void aura_display_int(int64_t);
void aura_display_char(char);
void aura_newline();
int64_t aura_jit_prim_dispatch(int64_t, int64_t*, int32_t);
int64_t aura_cast_op(int64_t, int64_t);
void aura_register_fn(int64_t func_id, int64_t (*fn)(int64_t*, uint32_t), int32_t local_count,
                      int32_t arg_count, int32_t env_count);
void aura_reset_runtime();
void aura_set_prim_dispatcher(int64_t (*fn)(int64_t, int64_t*, int32_t));
// Float/string pool functions (defined in aura_jit_runtime.cpp)
int64_t aura_alloc_float(double);
double aura_float_ref(int64_t);
int64_t aura_alloc_string(const char*);
const char* aura_string_ref(int64_t);
const char* aura_jit_string_content(int64_t);
// Drop stubs: Aura uses bump allocator, no per-value GC needed.
// JIT-compiled DropOp calls these; they exist for completeness.
void aura_drop_pair(int64_t) {}
void aura_drop_cell(int64_t) {}
void aura_drop_closure(int64_t) {}
}

// C standard library functions (declared in <cstdio>, registered as JIT symbols)

// ── AOT native compilation ──────────────────────────────────────
// Compile a FlatFunction to LLVM IR → .ll → llc → .o
static int aot_func_counter = 0;
static bool emit_native_object_llvm(const FlatFunction& fn, const std::string& out_obj_path,
                                     const std::vector<std::string>* string_pool) {
    llvm::LLVMContext local_ctx;
    int my_id = __sync_fetch_and_add(&aot_func_counter, 1);
    auto mod = std::make_unique<llvm::Module>(std::string(fn.name) + "_" + std::to_string(my_id) + "_aot", local_ctx);

    LLVMBuilder builder{local_ctx};
    if (string_pool)
        builder.string_pool = string_pool;
    builder.aot_mode = true;
    builder.mod = mod.get();
    builder.declare_runtime();

    // Build function with unique name (counter disambiguates __lambda__ duplicates).
    // __top__ is the entry point called by runtime.c's main() — keep exact name.
    std::string fn_name = fn.name;
    auto unique_name = (fn_name == "__top__")
        ? fn_name
        : fn_name + "_" + std::to_string(my_id);
    auto ptr_i64 = llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(local_ctx));
    auto i32_ty = llvm::Type::getInt32Ty(local_ctx);
    auto ret_ty = llvm::Type::getInt64Ty(local_ctx);
    auto fn_type = llvm::FunctionType::get(ret_ty, {ptr_i64, i32_ty}, false);
    builder.func =
        llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage, unique_name, mod.get());
    auto arg_it = builder.func->arg_begin();
    arg_it->setName("locals_ptr");
    ++arg_it;
    arg_it->setName("argc");

    // Create blocks
    for (uint32_t i = 0; i < fn.num_blocks; ++i)
        builder.block_map[fn.blocks[i].id] = llvm::BasicBlock::Create(local_ctx, "", builder.func);

    // Alloc locals in entry
    builder.irb = new llvm::IRBuilder<>(local_ctx);
    builder.irb->SetInsertPoint(builder.block_map[fn.entry_block]);
    for (uint32_t i = 0; i < fn.local_count; ++i)
        builder.llvm_locals.push_back(builder.irb->CreateAlloca(llvm::Type::getInt64Ty(local_ctx)));

    // Lower each block
    for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
        auto& fb = fn.blocks[bi];
        builder.irb->SetInsertPoint(builder.block_map[fb.id]);
        for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
            if (!builder.lower(fb.instructions[ii], fb.id, fn)) {
                delete builder.irb;
                return false;
            }
        }
    }
    delete builder.irb;

    // Run LLVM optimization passes
    {
        llvm::PassBuilder pb;
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;

        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);

        auto mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        mpm.run(*mod, mam);
    }

    if (llvm::verifyModule(*mod, &llvm::errs())) {
        fprintf(stderr, "AOT: verification failed for '%s'\n", fn.name);
        return false;
    }

    // ── Emit .o directly via LLVM TargetMachine (no llc dependency) ──
    static std::once_flag aot_target_init;
    std::call_once(aot_target_init, []() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });

    auto triple_str = llvm::sys::getDefaultTargetTriple();
    llvm::Triple triple(triple_str);
    std::string err;
    const auto* target = llvm::TargetRegistry::lookupTarget(triple_str, err);
    if (!target) {
        fprintf(stderr, "AOT: cannot find target %s: %s\n", triple_str.c_str(), err.c_str());
        return false;
    }

    auto tm = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, "generic", {}, {}, {}));
    if (!tm) {
        fprintf(stderr, "AOT: cannot create TargetMachine for %s\n", std::string(triple.str()).c_str());
        return false;
    }

    mod->setDataLayout(tm->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(out_obj_path, ec, llvm::sys::fs::OF_None);
    if (ec) {
        fprintf(stderr, "AOT: cannot open %s: %s\n", out_obj_path.c_str(), ec.message().c_str());
        return false;
    }

    llvm::legacy::PassManager cgpm;
    if (tm->addPassesToEmitFile(cgpm, dest, nullptr,
                                 llvm::CodeGenFileType::ObjectFile)) {
        fprintf(stderr, "AOT: TargetMachine cannot emit object file for '%s'\n", fn.name);
        return false;
    }
    cgpm.run(*mod);
    dest.flush();
    return true;
}

struct AuraJIT::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    llvm::orc::JITDylib* main_dylib = nullptr;
    llvm::LLVMContext ctx;
    uint64_t module_counter = 0;
    bool initialized = false;
    bool optimize = true;
    std::vector<FunctionMeta> compiled_fns_{};
    const std::vector<std::string>* string_pool_{nullptr};
    // Per-function resource trackers for hot-swap (remove old module, add new one)
    llvm::orc::ResourceTrackerSP get_or_create_tracker(const std::string& name) {
        // Remove old tracker/module for this name before creating a new one.
        // This fixes duplicate symbol errors when the same function name
        // is compiled from different eval() calls (e.g., inlined lambdas).
        auto it = fn_trackers_.find(name);
        if (it != fn_trackers_.end()) {
            // Remove old module from JITDylib before adding the new one
            if (auto err = it->second->remove())
                llvm::consumeError(std::move(err));
        }
        auto rt = main_dylib->createResourceTracker();
        fn_trackers_[name] = rt;
        return rt;
    }
    std::unordered_map<std::string, llvm::orc::ResourceTrackerSP> fn_trackers_;

    bool init() {
        if (initialized)
            return true;
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        auto jit_or_err = llvm::orc::LLJITBuilder().create();
        if (!jit_or_err) {
            fprintf(stderr, "JIT: LLJIT create failed\n");
            return false;
        }
        jit = std::move(*jit_or_err);
        main_dylib = &jit->getMainJITDylib();
        initialized = true;

        // Register runtime function symbols
        auto reg = [&](const char* name, void* ptr) {
            auto sym = llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(ptr),
                                                    llvm::JITSymbolFlags::Callable);
            auto pool = jit->getExecutionSession().getSymbolStringPool();
            auto k = pool->intern(name);
            auto map = llvm::orc::absoluteSymbols({{k, sym}});
            if (auto err = main_dylib->define(std::move(map)))
                fprintf(stderr, "JIT: failed to define symbol '%s'\n", name);
        };

        // Runtime functions
        reg("aura_alloc_closure", (void*)aura_alloc_closure);
        reg("aura_closure_capture", (void*)aura_closure_capture);
        reg("aura_closure_call", (void*)aura_closure_call);
        reg("aura_new_cell", (void*)aura_new_cell);
        reg("aura_cell_get", (void*)aura_cell_get);
        reg("aura_cell_set", (void*)aura_cell_set);
        reg("aura_alloc_pair", (void*)aura_alloc_pair);
        reg("aura_alloc_pair_arena", (void*)aura_alloc_pair_arena);
        reg("aura_pair_car", (void*)aura_pair_car);
        reg("aura_pair_cdr", (void*)aura_pair_cdr);
        reg("aura_pair_car_unchecked", (void*)aura_pair_car_unchecked);
        reg("aura_pair_cdr_unchecked", (void*)aura_pair_cdr_unchecked);
        reg("aura_prim_call", (void*)aura_prim_call);
        reg("aura_set_prim_dispatcher", (void*)aura_set_prim_dispatcher);
        reg("aura_display_int", (void*)aura_display_int);
        reg("aura_display_char", (void*)aura_display_char);
        reg("aura_newline", (void*)aura_newline);
        reg("aura_jit_prim_dispatch", (void*)aura_jit_prim_dispatch);
        reg("aura_register_fn", (void*)aura_register_fn);
        reg("aura_cast_op", (void*)aura_cast_op);
        reg("aura_reset_runtime", (void*)aura_reset_runtime);
        reg("aura_alloc_float", (void*)aura_alloc_float);
        reg("aura_float_ref", (void*)aura_float_ref);
        reg("aura_alloc_string", (void*)aura_alloc_string);
        reg("aura_string_ref", (void*)aura_string_ref);
        reg("aura_jit_string_content", (void*)aura_jit_string_content);
        reg("aura_drop_pair", (void*)aura_drop_pair);
        reg("aura_drop_cell", (void*)aura_drop_cell);
        reg("aura_drop_closure", (void*)aura_drop_closure);

        // C standard library functions
        reg("printf", (void*)printf);
        reg("fprintf", (void*)fprintf);
        reg("fflush", (void*)fflush);
        reg("fputc", (void*)fputc);

        // Reset runtime state for fresh session
        aura_reset_runtime();

        return true;
    }

    ScalarFn compile(const FlatFunction& fn) {
        if (!init())
            return nullptr;

        auto mod = std::make_unique<llvm::Module>(
            std::string("mod_") + std::to_string(module_counter++), ctx);

        LLVMBuilder builder{ctx};
        builder.mod = mod.get();
        builder.declare_runtime();
        builder.shape_map = fn.shape_map;
        builder.shape_map_size = fn.local_count;
        if (string_pool_)
            builder.string_pool = string_pool_;

        // Build function
        auto ptr_i64 = llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(ctx));
        auto i32_ty = llvm::Type::getInt32Ty(ctx);
        auto ret_ty = llvm::Type::getInt64Ty(ctx);
        auto fn_type = llvm::FunctionType::get(ret_ty, {ptr_i64, i32_ty}, false);
        builder.func =
            llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage, fn.name, mod.get());
        auto arg_it = builder.func->arg_begin();
        arg_it->setName("locals_ptr");
        ++arg_it;
        arg_it->setName("argc");

        // Create blocks
        for (uint32_t i = 0; i < fn.num_blocks; ++i)
            builder.block_map[fn.blocks[i].id] = llvm::BasicBlock::Create(ctx, "", builder.func);

        // Alloc locals in entry
        builder.irb = new llvm::IRBuilder<>(ctx);
        builder.irb->SetInsertPoint(builder.block_map[fn.entry_block]);
        for (uint32_t i = 0; i < fn.local_count; ++i)
            builder.llvm_locals.push_back(builder.irb->CreateAlloca(llvm::Type::getInt64Ty(ctx)));

        // Lower each block
        for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
            auto& fb = fn.blocks[bi];
            builder.irb->SetInsertPoint(builder.block_map[fb.id]);
            for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
                if (!builder.lower(fb.instructions[ii], fb.id, fn)) {
                    delete builder.irb;
                    return nullptr;
                }
            }
        }
        delete builder.irb;

        // Run LLVM optimization passes
        if (optimize) {
            llvm::PassBuilder pb;
            llvm::LoopAnalysisManager lam;
            llvm::FunctionAnalysisManager fam;
            llvm::CGSCCAnalysisManager cgam;
            llvm::ModuleAnalysisManager mam;

            pb.registerModuleAnalyses(mam);
            pb.registerCGSCCAnalyses(cgam);
            pb.registerFunctionAnalyses(fam);
            pb.registerLoopAnalyses(lam);
            pb.crossRegisterProxies(lam, fam, cgam, mam);

            auto mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
            mpm.run(*mod, mam);
        }

        if (llvm::verifyModule(*mod, &llvm::errs())) {
            fprintf(stderr, "JIT: verification failed\n");
            return nullptr;
        }

        auto tsm =
            llvm::orc::ThreadSafeModule(std::move(mod), std::make_unique<llvm::LLVMContext>());
        auto rt = get_or_create_tracker(std::string(fn.name));
        if (auto err = jit->addIRModule(rt, std::move(tsm))) {
            fprintf(stderr, "JIT: addIRModule failed\n");
            return nullptr;
        }

        auto sym = jit->lookup(std::string(fn.name));
        if (!sym) {
            fprintf(stderr, "JIT: lookup failed\n");
            return nullptr;
        }
        return sym->toPtr<ScalarFn>();
    }

    void* get_function_ptr(const char* name) {
        if (!init())
            return nullptr;
        auto sym = jit->lookup(name);
        if (!sym)
            return nullptr;
        return sym->toPtr<void*>();
    }

    void register_symbol_func(const char* name, void* ptr) {
        if (!init())
            return;
        auto sym = llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(ptr),
                                                llvm::JITSymbolFlags::Callable);
        auto pool = jit->getExecutionSession().getSymbolStringPool();
        auto k = pool->intern(name);
        auto map = llvm::orc::absoluteSymbols({{k, sym}});
        if (auto err = main_dylib->define(std::move(map)))
            fprintf(stderr, "JIT: failed to define symbol '%s'\n", name);
    }

    void register_fn_func(int64_t func_id, ScalarFn fn_ptr, uint32_t local_count,
                          uint32_t arg_count, uint32_t env_count) {
        aura_register_fn(func_id, fn_ptr, static_cast<int32_t>(local_count),
                         static_cast<int32_t>(arg_count), static_cast<int32_t>(env_count));
        compiled_fns_.push_back({std::string(), fn_ptr, local_count, arg_count, env_count});
    }

};

AuraJIT::AuraJIT()
    : impl_(std::make_unique<Impl>()) {}
AuraJIT::~AuraJIT() = default;
bool AuraJIT::available() const {
    return impl_->initialized;
}
ScalarFn AuraJIT::compile(const FlatFunction& fn) {
    return impl_->compile(fn);
}
void* AuraJIT::get_function_ptr(const char* name) {
    return impl_->get_function_ptr(name);
}

void AuraJIT::register_symbol(const char* name, void* ptr) {
    impl_->register_symbol_func(name, ptr);
}

void AuraJIT::set_string_pool(const std::vector<std::string>* pool) {
    impl_->string_pool_ = pool;
}

void AuraJIT::register_function(int64_t func_id, ScalarFn fn_ptr, uint32_t local_count,
                                uint32_t arg_count, uint32_t env_count) {
    impl_->register_fn_func(func_id, fn_ptr, local_count, arg_count, env_count);
}

const std::vector<FunctionMeta>& AuraJIT::compiled_functions() const {
    return impl_->compiled_fns_;
}

// ── Public AOT API ──────────────────────────────────────────────

bool emit_native_object(const FlatFunction& fn, const std::string& out_obj_path,
                         const std::vector<std::string>* string_pool) {
    return emit_native_object_llvm(fn, out_obj_path, string_pool);
}

bool emit_object(const std::string& ir_dump, const std::string& out_path) {
    // Deprecated: use emit_native_object instead.
    // Parse IR dump and rebuild the module, then compile
    // For now: dump only — full implementation needs IR deserialization
    if (auto* f = std::fopen((out_path + ".ir").c_str(), "w")) {
        std::fprintf(f, "%s", ir_dump.c_str());
        std::fclose(f);
        return true;
    }
    return false;
}

bool emit_object_module(void* /*ir_module*/, const std::string& out_path) {
    // Deprecated: use emit_native_object instead.
    if (auto* f = std::fopen((out_path + ".ir").c_str(), "w")) {
        std::fprintf(f, "emit_object_module: deprecated placeholder\n");
        std::fclose(f);
        return true;
    }
    return false;
}

} // namespace aura::jit

#else

namespace aura::jit {

AuraJIT::AuraJIT()
    : impl_(nullptr) {}
AuraJIT::~AuraJIT() = default;
bool AuraJIT::available() const {
    return false;
}
ScalarFn AuraJIT::compile(const FlatFunction&) {
    return nullptr;
}
void* AuraJIT::get_function_ptr(const char*) {
    return nullptr;
}
void AuraJIT::register_symbol(const char*, void*) {}
void AuraJIT::set_string_pool(const std::vector<std::string>*) {}
void AuraJIT::register_function(int64_t, ScalarFn, uint32_t, uint32_t, uint32_t) {}
const std::vector<FunctionMeta>& AuraJIT::compiled_functions() const {
    static std::vector<FunctionMeta> empty;
    return empty;
}


// ── AOT native compilation (stubs, LLVM unavailable) ────────────

bool emit_native_object(const FlatFunction&, const std::string&,
                         const std::vector<std::string>*) {
    return false;
}

bool emit_object(const std::string& ir_dump, const std::string& out_path) {
    // Parse IR dump and rebuild the module, then compile
    // For now: dump only — full implementation needs IR deserialization
    if (auto* f = std::fopen((out_path + ".ir").c_str(), "w")) {
        std::fprintf(f, "%s", ir_dump.c_str());
        std::fclose(f);
        return true;
    }
    return false;
}

bool emit_object_module(void* /*ir_module*/, const std::string& out_path) {
    // Deprecated: use emit_native_object instead.
    if (auto* f = std::fopen((out_path + ".ir").c_str(), "w")) {
        std::fprintf(f, "emit_object_module: deprecated placeholder (no LLVM)\n");
        std::fclose(f);
        return true;
    }
    return false;
}

} // namespace aura::jit

#endif
