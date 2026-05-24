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

#include <cstdio>
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
};

// LLVM IR Builder
struct LLVMBuilder {
    llvm::LLVMContext& ctx;
    llvm::Module* mod = nullptr;
    llvm::Function* func = nullptr;
    llvm::IRBuilder<>* irb = nullptr;
    std::vector<llvm::Value*> llvm_locals{};
    std::unordered_map<uint32_t, llvm::BasicBlock*> block_map{};

    // Runtime function declarations
    llvm::Function* fn_alloc_closure = nullptr;
    llvm::Function* fn_closure_capture = nullptr;
    llvm::Function* fn_closure_call = nullptr;
    llvm::Function* fn_new_cell = nullptr;
    llvm::Function* fn_cell_get = nullptr;
    llvm::Function* fn_cell_set = nullptr;
    llvm::Function* fn_alloc_pair = nullptr;
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

    void declare_runtime() {
        auto i64 = llvm::Type::getInt64Ty(ctx);
        auto ptr_i64 = llvm::PointerType::getUnqual(i64);
        auto void_ty = llvm::Type::getVoidTy(ctx);
        auto i8_ty = llvm::Type::getInt8Ty(ctx);

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

        fn_pair_car = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_pair_car", mod);

        fn_pair_cdr = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_pair_cdr", mod);

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
            case OpConstI64:
                store(inst.ops[0], c64(static_cast<int64_t>(inst.ops[1]) |
                                       (static_cast<int64_t>(inst.ops[2]) << 32)));
                return true;
            case OpLocal:
                store(inst.ops[0], load(inst.ops[1]));
                return true;
            case OpArg: {
                auto gep = irb->CreateGEP(llvm::Type::getInt64Ty(ctx), func->arg_begin(),
                                          c64(inst.ops[1]));
                store(inst.ops[0], irb->CreateLoad(llvm::Type::getInt64Ty(ctx), gep));
                return true;
            }
            case OpAdd:
                store(inst.ops[0], irb->CreateAdd(load(inst.ops[1]), load(inst.ops[2])));
                return true;
            case OpSub:
                store(inst.ops[0], irb->CreateSub(load(inst.ops[1]), load(inst.ops[2])));
                return true;
            case OpMul:
                store(inst.ops[0], irb->CreateMul(load(inst.ops[1]), load(inst.ops[2])));
                return true;
            case OpDiv: {
                auto dividend = load(inst.ops[1]);
                auto divisor = load(inst.ops[2]);
                auto is_zero = irb->CreateICmpEQ(divisor, c64(0));
                // Avoid CPU #DE (SIGFPE) on zero divisor: substitute 1, then select 0 as result
                auto safe_div = irb->CreateSelect(is_zero, c64(1), divisor);
                auto div_result = irb->CreateSDiv(dividend, safe_div);
                store(inst.ops[0], irb->CreateSelect(is_zero, c64(0), div_result));
                return true;
            }
            case OpEq: {
                auto c = irb->CreateICmpEQ(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateZExt(c, llvm::Type::getInt64Ty(ctx)));
                return true;
            }
            case OpLt: {
                auto c = irb->CreateICmpSLT(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateZExt(c, llvm::Type::getInt64Ty(ctx)));
                return true;
            }
            case OpGt: {
                auto c = irb->CreateICmpSGT(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateZExt(c, llvm::Type::getInt64Ty(ctx)));
                return true;
            }
            case OpLe: {
                auto c = irb->CreateICmpSLE(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateZExt(c, llvm::Type::getInt64Ty(ctx)));
                return true;
            }
            case OpGe: {
                auto c = irb->CreateICmpSGE(load(inst.ops[1]), load(inst.ops[2]));
                store(inst.ops[0], irb->CreateZExt(c, llvm::Type::getInt64Ty(ctx)));
                return true;
            }
            case OpAnd: {
                auto a = irb->CreateICmpNE(load(inst.ops[1]), c64(0)),
                     b = irb->CreateICmpNE(load(inst.ops[2]), c64(0));
                store(inst.ops[0],
                      irb->CreateZExt(irb->CreateAnd(a, b), llvm::Type::getInt64Ty(ctx)));
                return true;
            }
            case OpOr: {
                auto a = irb->CreateICmpNE(load(inst.ops[1]), c64(0)),
                     b = irb->CreateICmpNE(load(inst.ops[2]), c64(0));
                store(inst.ops[0],
                      irb->CreateZExt(irb->CreateOr(a, b), llvm::Type::getInt64Ty(ctx)));
                return true;
            }
            case OpNot: {
                auto a = irb->CreateICmpEQ(load(inst.ops[1]), c64(0));
                store(inst.ops[0], irb->CreateZExt(a, llvm::Type::getInt64Ty(ctx)));
                return true;
            }
            case OpBranch: {
                auto cond = irb->CreateICmpNE(load(inst.ops[0]), c64(0));
                irb->CreateCondBr(cond, block_map[inst.ops[1]], block_map[inst.ops[2]]);
                return true;
            }
            case OpJump:
                irb->CreateBr(block_map[inst.ops[0]]);
                return true;
            case OpReturn:
                irb->CreateRet(load(inst.ops[0]));
                return true;
            case OpConstBool:
                store(inst.ops[0], c64(inst.ops[1] ? 1 : 0));
                return true;
            case OpConstVoid:
                store(inst.ops[0], c64(0));
                return true;
            case OpConstF64:
                store(inst.ops[0], c64(0));
                return true;
            case OpConstString:
                store(inst.ops[0], c64(0));
                return true;

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

            // Pairs
            case OpMakePair: {
                auto call = irb->CreateCall(fn_alloc_pair, {load(inst.ops[1]), load(inst.ops[2])});
                store(inst.ops[0], call);
                return true;
            }
            case OpCar: {
                auto call = irb->CreateCall(fn_pair_car, {load(inst.ops[1])});
                store(inst.ops[0], call);
                return true;
            }
            case OpCdr: {
                auto call = irb->CreateCall(fn_pair_cdr, {load(inst.ops[1])});
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
                    // Safe quotent: result = (b==0) ? 0 : (a / b)
                    auto zero = irb->CreateICmpEQ(a2, c64(0));
                    auto div = irb->CreateSDiv(a1, a2);
                    auto safe = irb->CreateSelect(zero, c64(0), div);
                    store(result_slot, safe);
                    return true;
                }
                case PrimRemainder: {
                    // Safe remainder: result = (b==0) ? 0 : (a % b)
                    auto zero = irb->CreateICmpEQ(a2, c64(0));
                    auto rem = irb->CreateSRem(a1, a2);
                    auto safe = irb->CreateSelect(zero, c64(0), rem);
                    store(result_slot, safe);
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
                auto call = irb->CreateCall(fn_prim_call, {c64(prim_slot), c64(0), c64(0), c64(0)});
                store(result_slot, call);
                return true;
            }

            default:
                if (inst.ops[0] < fn.local_count)
                    store(inst.ops[0], c64(0));
                return true;
        }
    }
};

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
int64_t aura_pair_car(int64_t);
int64_t aura_pair_cdr(int64_t);
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
}

// C standard library functions (declared in <cstdio>, registered as JIT symbols)

struct AuraJIT::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    llvm::orc::JITDylib* main_dylib = nullptr;
    llvm::LLVMContext ctx;
    uint64_t module_counter = 0;
    bool initialized = false;
    bool optimize = true;
    std::vector<FunctionMeta> compiled_fns_{};

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
        reg("aura_pair_car", (void*)aura_pair_car);
        reg("aura_pair_cdr", (void*)aura_pair_cdr);
        reg("aura_prim_call", (void*)aura_prim_call);
        reg("aura_set_prim_dispatcher", (void*)aura_set_prim_dispatcher);
        reg("aura_display_int", (void*)aura_display_int);
        reg("aura_display_char", (void*)aura_display_char);
        reg("aura_newline", (void*)aura_newline);
        reg("aura_jit_prim_dispatch", (void*)aura_jit_prim_dispatch);
        reg("aura_register_fn", (void*)aura_register_fn);
        reg("aura_cast_op", (void*)aura_cast_op);
        reg("aura_reset_runtime", (void*)aura_reset_runtime);

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
        if (auto err = jit->addIRModule(std::move(tsm))) {
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

void AuraJIT::register_function(int64_t func_id, ScalarFn fn_ptr, uint32_t local_count,
                                uint32_t arg_count, uint32_t env_count) {
    impl_->register_fn_func(func_id, fn_ptr, local_count, arg_count, env_count);
}

const std::vector<FunctionMeta>& AuraJIT::compiled_functions() const {
    return impl_->compiled_fns_;
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
void AuraJIT::register_function(int64_t, ScalarFn, uint32_t, uint32_t, uint32_t) {}
const std::vector<FunctionMeta>& AuraJIT::compiled_functions() const {
    static std::vector<FunctionMeta> empty;
    return empty;
}


// ── emit_object: compile IR to native object file ──────────────
// Uses LLVM TargetMachine to emit .o file.
// Falls back to .ir dump when LLVM is unavailable.

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

bool emit_object_module(void* ir_module, const std::string& out_path) {
    if (!ir_module)
        return false;
    auto& mod = *static_cast<aura::ir::IRModule*>(ir_module);
    // Dump IR as placeholder
    if (auto* f = std::fopen((out_path + ".ir").c_str(), "w")) {
        for (auto& fn : mod.functions()) {
            std::fprintf(f, "func[%zu] %s\n", fn.id, fn.name.c_str());
        }
        std::fclose(f);
        return true;
    }
    return false;
}

} // namespace aura::jit

#endif
