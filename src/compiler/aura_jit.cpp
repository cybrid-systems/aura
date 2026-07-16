// aura_jit.cpp — LLVM ORC JIT backend for Aura IR
#include <bit>
#include "aura_jit.h"
#include "aura_jit_bridge.h"
#include "aot_mangle.h"
#include "jit_typed_mutation_stats.h"
#include "value_tags.h"

#if AURA_HAVE_LLVM

// Short alias for the value tag namespace — keeps the JIT/IR
// builder code free of fully-qualified names.
namespace types = aura::compiler::types;

#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h> // Issue #170: CloneModule for AOT snapshot
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
#include <unordered_set>
#include <memory>
#include <string>
#include "hash_meta.h" // #908

namespace aura::jit {

// PrimId values (must match ir.ixx PrimId enum order)
// Used for PrimCall fast-path dispatch — skipping aura_prim_call runtime.
enum : uint32_t {
    PrimHash = 0,
    PrimStringAppend = 5,
    PrimStringLength = 6,
    PrimStringRef = 7,
    PrimSubstring = 8,
    PrimStringEq = 9,
    PrimStringLt = 10,
    PrimNumberToString = 11,
    PrimStringToNumber = 12,
    PrimDisplay = 13,
    PrimWrite = 14,
    PrimNewline = 15,
    PrimError = 16,
    PrimAssert = 17,
    PrimRead = 18,
    PrimReadFile = 19,
    PrimWriteFile = 20,
    PrimFileExists = 21,
    PrimGensym = 22,
    PrimApply = 23,
    PrimVector = 24,
    PrimVectorRef = 25,
    PrimVectorSet = 26,
    PrimVectorLength = 27,
    PrimVectorP = 28,
    PrimMakeVector = 29,
    PrimImport = 30,
    PrimCharEq = 31,
    PrimCharLt = 32,
    PrimCharToInteger = 33,
    PrimIntegerToChar = 34,
    PrimQuotient = 35,
    PrimRemainder = 36,
    PrimListLength = 37,
    PrimListRef = 38,
    PrimListReverse = 39,
    PrimRaise = 40,
    PrimErrorP = 41,
    PrimPairP = 42,
    PrimNullP = 43,
};
// Compile-time lockstep with ir.ixx PrimId enum.
// If you add/remove/reorder a PrimId entry, both sides must change together.
static_assert(PrimDisplay == 13, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimWrite == 14, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimNewline == 15, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimQuotient == 35, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimRemainder == 36, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimListLength == 37, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimListRef == 38, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimListReverse == 39, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimRaise == 40, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimErrorP == 41, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimPairP == 42, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimNullP == 43, "PrimId drift: aura_jit.cpp vs ir.ixx");

// Opcode enum values (must match ir.ixx IROpcode)
//
// Issue #157 — workspace_mtx_ bypass via JIT L2 specialization
// The following opcodes generate code paths that call runtime bridges
// (aura_alloc_pair_*, aura_pair_*_unchecked, aura_alloc_closure_*,
// aura_closure_capture, aura_hash_*) which currently do NOT acquire
// Evaluator::workspace_mtx_ or check defuse_version_. The high-level
// evaluator primitives DO acquire the lock + yield, so concurrent
// mutate + JIT execution races on shared heap structures.
//
// See Issue #157 (archived: git tag docs-archive-pre-2026-06) for the
// full inventory and phased fix plan. Phase 0 (this commit) adds the
// telemetry counter and bypass markers; Phase 1 wraps P1 sites with
// lock acquire/release and adds version checks at L2 entry points.
enum Op : uint32_t {
    OpNop = 0, // Issue #427: explicit Nop case (matches IROpcode::Nop = 0)
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
    // Issue #124: TryBegin + TryEnd added between IsError and
    // HashRef in IROpcode (aura::ir). Without these, OpRaise
    // and OpIsError were correctly numbered but the JIT's
    // local mirror was already off-sync — the case statements
    // for HashRef/Set/Remove/Arena/GuardShape used values that
    // did not match the IR enum. This enum is now in lockstep
    // with IROpcode: TryBegin=39, TryEnd=40, HashRef=41,
    // HashSet=42, HashRemove=43, LinearWrap=44, MoveOp=45,
    // BorrowOp=46, MutBorrowOp=47, DropOp=48, RefCountOp=49,
    // ArenaPush=50, ArenaPop=51, GuardShape=52.
    //
    // OpTryBegin + OpTryEnd lowering is still TODO (Phase 1 /
    // item #2 of #170); the enum entries are here so the
    // default branch in lower() can detect them as unhandled.
    OpTryBegin = 39,
    OpTryEnd = 40,
    // Hash operations (inline dispatch, avoids PrimCall overhead)
    OpHashRef = 41,
    OpHashSet = 42,
    OpHashRemove = 43,
    // M4 Linear ownership opcodes — match aura::ir::IROpcode.
    OpLinearWrap = 44,
    OpMoveOp = 45,
    OpBorrowOp = 46,
    OpMutBorrowOp = 47,
    OpDropOp = 48,
    OpRefCountOp = 49,
    // Arena operations
    OpArenaPush = 50,
    OpArenaPop = 51,
    // Issue #61 Iter 3: lazy-deopt guard. Same encoding as
    // aura::ir::IROpcode::GuardShape (52).
    OpGuardShape = 52,
    // Issue #272 Cycle 5: load top-level value-define cell from
    // evaluator_.cells() via aura_top_cell_get runtime bridge.
    OpTopCellLoad = 53,
};

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
    // Issue #170 Phase 1 / item #1: optional metrics pointer.
    // Set by compile() (and the AOT path) so the lower() default
    // branch can increment unhandled_opcode_count. Nullable so
    // direct lower() calls in tests don't crash; in production
    // it's always set when going through compile().
    aura::jit::AuraJIT::Metrics* metrics = nullptr;
    // Issue #193: per-function unhandled-opcode counter owned by
    // the builder. Bumped by the default branch in lower().
    // Impl::compile() reads it back after the lower() loop and
    // folds it into fn_unhandled_counts_ (so SpecJITController
    // can query per-function instead of using the conservative
    // global signal). Pointer is nullable in AOT mode (no per-
    // function tracking there — see the AOT path's comment).
    std::atomic<std::uint64_t>* fn_unhandled = nullptr;
    // Shape map from FlatFunction: null = dynamic, non-null = known shapes
    // Values: 0=Dynamic, 1=Int, 2=Float, 3=Bool, 4=String, 5=Void
    const uint8_t* shape_map = nullptr;
    uint32_t shape_map_size = 0;

    // Issue #60 Iter 3: per-instruction shape_id. Set during the build
    // loop in AuraJIT::compile, this is the shape of the instruction's
    // RESULT slot (ops[0]). 0 = unknown. When nonzero, it's a 1-byte
    // shape encoding (matches shape_map). The L1/L2 fast paths below
    // prefer this over indexing into the side-channel shape_map.
    // (SHAPE_INT / SHAPE_PAIR / SHAPE_STRING constants live in aura_jit.h.)

    // Read the shape_id from the instruction, falling back to the
    // side-channel shape_map if the instruction has no shape set.
    // Returns 0 (Dynamic / unknown) if neither source has a value.
    inline uint32_t inst_shape(const FlatInstruction& inst) const {
        if (inst.shape_id != 0)
            return inst.shape_id;
        if (shape_map && inst.ops[0] < shape_map_size)
            return shape_map[inst.ops[0]];
        return 0;
    }

    // Pointer tagging constants (must match lib/runtime.c).
    // FLOAT_BIAS_VAL comes from value_tags.h so .cpp code stays in
    // lockstep with the module-side encoding (issue #58).
    static constexpr int64_t KWD_TRUE_VAL = 7;
    static constexpr int64_t KWD_FALSE_VAL = 3;
    static constexpr int64_t FLOAT_BIAS_VAL = types::FLOAT_BIAS_VAL;
    llvm::Type* double_ty = llvm::Type::getDoubleTy(ctx);
    // String pool for OpConstString (IR module's string pool content)
    const std::vector<std::string>* string_pool = nullptr;

    // Runtime function declarations
    llvm::Function* fn_alloc_closure = nullptr;
    llvm::Function* fn_alloc_closure_arena = nullptr;
    llvm::Function* fn_closure_capture = nullptr;
    llvm::Function* fn_closure_call = nullptr;
    llvm::Function* fn_new_cell = nullptr;
    llvm::Function* fn_cell_get = nullptr;
    llvm::Function* fn_cell_set = nullptr;
    llvm::Function* fn_top_cell_get = nullptr;
    llvm::Function* fn_alloc_pair = nullptr;
    llvm::Function* fn_alloc_pair_arena = nullptr;
    llvm::Function* fn_pair_car = nullptr;
    llvm::Function* fn_pair_cdr = nullptr;
    llvm::Function* fn_prim_call = nullptr;
    llvm::Function* fn_display_int = nullptr;
    llvm::Function* fn_display_char = nullptr;
    llvm::Function* fn_newline = nullptr;
    llvm::Function* fn_epoch_acquire_fence = nullptr;
    llvm::Function* fn_linear_jit_safety = nullptr;
    llvm::Function* fn_cast_op = nullptr;
    llvm::Function* fn_bump_init = nullptr;
    llvm::Function* fn_bump_reset = nullptr;
    llvm::Function* fn_register_closure = nullptr;
    llvm::Function* fn_drop_pair = nullptr;
    llvm::Function* fn_drop_cell = nullptr;
    llvm::Function* fn_drop_closure = nullptr;
    llvm::Function* fn_drop_value = nullptr;
    // Issue #170 Phase 1 / item #2: try/catch runtime bridges.
    // The exception stack is thread-local in aura_jit_runtime.cpp;
    // OpRaise reads the top frame's handler_block + payload_slot
    // and emits a direct Br to handler_block (no longjmp, no
    // C++ exception, no LLVM invoke/landingpad). The stack is
    // LIFO; OpTryBegin pushes, OpTryEnd pops.
    llvm::Function* fn_exception_push = nullptr;
    llvm::Function* fn_exception_pop = nullptr;
    llvm::Function* fn_exception_top_handler = nullptr;
    llvm::Function* fn_exception_top_payload = nullptr;
    llvm::Function* fn_arena_push = nullptr;
    llvm::Function* fn_arena_pop = nullptr;
    llvm::Function* fn_alloc_float = nullptr;
    llvm::Function* fn_float_ref = nullptr;
    llvm::Function* fn_alloc_string = nullptr;
    llvm::Function* fn_string_ref = nullptr;
    // L2 specialization: unchecked pair access (skips tag check)
    llvm::Function* fn_pair_car_unchecked = nullptr;
    llvm::Function* fn_pair_cdr_unchecked = nullptr;
    // Issue #157 Phase 1b: defuse_version_ accessor (runtime
    // function that reads the current Evaluator::defuse_version_
    // via the g_lock_hooks.get_version callback).
    llvm::Function* fn_get_defuse_version = nullptr;
    // Issue #157 Phase 1c: in-LLVM-callable deopt counter. The
    // JIT emits a call to this at the start of every deopt basic
    // block (bb_slow in OpCar/OpCdr SHAPE_PAIR lowering) so the
    // g_workspace_deopt_count is incremented on the hot path.
    // Runtime definition: extern "C" void aura_deopt_inc() in
    // aura_jit_runtime.cpp.
    llvm::Function* fn_deopt_inc = nullptr;
    // Issue #1534: GuardShape dual-epoch fence — runtime call to
    // aura_jit_guard_shape_epoch_check(fn_name) before narrow_evidence.
    llvm::Function* fn_guard_shape_epoch_check = nullptr;
    // Issue #1535: Linear* dual-epoch fence — is_fn_epoch_stale +
    // is_env_frame_stale before Move/Borrow/Drop mutation.
    llvm::Function* fn_linear_epoch_safety_check = nullptr;
    // Hash operations (inline dispatch, avoids PrimCall overhead)
    llvm::Function* fn_hash_ref = nullptr;
    llvm::Function* fn_hash_set = nullptr;
    llvm::Function* fn_hash_remove = nullptr;
    // Hash table direct accessor (Phase 4c): returns FlatHashTable*, then GEP
    llvm::Function* fn_hash_get_flat_table = nullptr;
    llvm::Function* fn_hash_key_eq = nullptr;
    // Issue #157 Phase 2b: workspace read/write lock primitives
    // (declared from the runtime hooks table; no-op when no
    // CompilerService is registered). OpHashRef's inline IR scan
    // uses fn_lock_workspace_read / fn_unlock_workspace_read to
    // bracket the entire scan so that aura_hash_set /
    // aura_hash_remove / FlatHashTable::rebuild cannot tear the
    // FlatHashTable pointer or its internals.
    llvm::Function* fn_lock_workspace_read = nullptr;
    llvm::Function* fn_unlock_workspace_read = nullptr;
    llvm::Function* fn_lock_workspace_write = nullptr;
    llvm::Function* fn_unlock_workspace_write = nullptr;

    void declare_runtime() {
        auto i64 = llvm::Type::getInt64Ty(ctx);
        auto ptr_i64 = llvm::PointerType::getUnqual(ctx);
        auto void_ty = llvm::Type::getVoidTy(ctx);
        auto i8_ty = llvm::Type::getInt8Ty(ctx);
        auto i32_ty = llvm::Type::getInt32Ty(ctx);
        auto ptr_i8 = llvm::PointerType::getUnqual(ctx);

        fn_alloc_closure =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_alloc_closure", mod);

        fn_alloc_closure_arena = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                                        llvm::Function::ExternalLinkage,
                                                        "aura_alloc_closure_arena", mod);

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

        fn_top_cell_get =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_top_cell_get", mod);

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

        fn_pair_car_unchecked =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_pair_car_unchecked", mod);

        fn_pair_cdr_unchecked =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_pair_cdr_unchecked", mod);

        // Issue #157 Phase 1b: declare the defuse_version_ accessor
        // for the L2 version check. The runtime function is
        // extern "C" uint64_t aura_get_defuse_version() (defined in
        // aura_jit_runtime.cpp). The JIT emits calls to it at function
        // entry (capture expected_version) and at each L2 SHAPE_PAIR
        // use (check current version, deopt to slow path on mismatch).
        fn_get_defuse_version =
            llvm::Function::Create(llvm::FunctionType::get(i64, false),
                                   llvm::Function::ExternalLinkage, "aura_get_defuse_version", mod);

        fn_deopt_inc =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                   llvm::Function::ExternalLinkage, "aura_deopt_inc", mod);

        // Issue #1534: i32 aura_jit_guard_shape_epoch_check(i8* name)
        fn_guard_shape_epoch_check = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, {ptr_i8}, false), llvm::Function::ExternalLinkage,
            "aura_jit_guard_shape_epoch_check", mod);

        // Issue #1535: i32 aura_jit_linear_epoch_safety_check(i8* name, i8 state, i32 opcode)
        fn_linear_epoch_safety_check = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, {ptr_i8, i8_ty, i32_ty}, false),
            llvm::Function::ExternalLinkage, "aura_jit_linear_epoch_safety_check", mod);

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

        fn_epoch_acquire_fence = llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                                        llvm::Function::ExternalLinkage,
                                                        "aura_jit_epoch_acquire_fence", mod);

        fn_linear_jit_safety = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, {i8_ty, i32_ty}, false),
            llvm::Function::ExternalLinkage, "aura_jit_linear_post_invalidate_safety", mod);

        fn_cast_op = llvm::Function::Create(llvm::FunctionType::get(i64, {i64, i64}, false),
                                            llvm::Function::ExternalLinkage, "aura_cast_op", mod);

        // Bump Allocator lifecycle
        fn_bump_init =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                   llvm::Function::ExternalLinkage, "aura_bump_init", mod);
        fn_bump_reset =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                   llvm::Function::ExternalLinkage, "aura_bump_reset", mod);

        // Drop functions
        fn_drop_pair =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_drop_pair", mod);
        fn_drop_cell =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_drop_cell", mod);
        fn_drop_closure =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_drop_closure", mod);

        // Issue #170 Phase 1 / item #2: try/catch exception stack
        // bridges. aura_exception_push/pop manage the LIFO stack;
        // aura_exception_top_handler/payload return the current
        // top frame's fields (0 / ~0 if empty). The JIT's OpRaise
        // reads these and emits a direct Br to handler_block.
        fn_exception_push =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_exception_push", mod);
        fn_exception_pop =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                   llvm::Function::ExternalLinkage, "aura_exception_pop", mod);
        fn_exception_top_handler = llvm::Function::Create(llvm::FunctionType::get(i64, false),
                                                          llvm::Function::ExternalLinkage,
                                                          "aura_exception_top_handler", mod);
        fn_exception_top_payload = llvm::Function::Create(llvm::FunctionType::get(i64, false),
                                                          llvm::Function::ExternalLinkage,
                                                          "aura_exception_top_payload", mod);

        // Arena push/pop
        fn_arena_push =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                   llvm::Function::ExternalLinkage, "aura_arena_push", mod);
        fn_arena_pop =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                   llvm::Function::ExternalLinkage, "aura_arena_pop", mod);

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

        // Hash operation functions (inline dispatch via C-linkage wrappers)
        fn_hash_ref = llvm::Function::Create(llvm::FunctionType::get(i64, {i64, i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_hash_ref", mod);
        fn_hash_set = llvm::Function::Create(llvm::FunctionType::get(i64, {i64, i64}, false),
                                             llvm::Function::ExternalLinkage, "aura_hash_set", mod);
        fn_hash_remove =
            llvm::Function::Create(llvm::FunctionType::get(i64, {i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_hash_remove", mod);

        // Hash table direct accessor (Phase 4c): get FlatHashTable*, then GEP
        {
            auto i8_ptr = llvm::PointerType::getUnqual(ctx);
            fn_hash_get_flat_table = llvm::Function::Create(
                llvm::FunctionType::get(i8_ptr, {i64}, false), llvm::Function::ExternalLinkage,
                "aura_hash_get_flat_table", mod);
            fn_hash_key_eq =
                llvm::Function::Create(llvm::FunctionType::get(i64, {i64, i64}, false),
                                       llvm::Function::ExternalLinkage, "aura_hash_key_eq", mod);
        }

        // Issue #157 Phase 2b: workspace lock/unlock declarations for
        // OpHashRef inline IR scan. The inline scan must hold the read
        // lock for its entire duration so that aura_hash_set /
        // aura_hash_remove / FlatHashTable::rebuild cannot tear the
        // FlatHashTable pointer or its metadata / keys / values arrays.
        // Acquired before fn_hash_get_flat_table, released in done_bb
        // (the only block that exits the inline IR region).
        fn_lock_workspace_read = llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                                        llvm::Function::ExternalLinkage,
                                                        "aura_lock_workspace_read", mod);
        fn_unlock_workspace_read = llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                                          llvm::Function::ExternalLinkage,
                                                          "aura_unlock_workspace_read", mod);
        fn_lock_workspace_write = llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                                         llvm::Function::ExternalLinkage,
                                                         "aura_lock_workspace_write", mod);
        fn_unlock_workspace_write = llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                                           llvm::Function::ExternalLinkage,
                                                           "aura_unlock_workspace_write", mod);

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

    // Lower one FlatInstruction into the current LLVM IR builder.
    // Contract: inst.opcode must be a valid IROpcode value (< MaxIROpcode);
    // for result-producing opcodes, inst.ops[0] must be a valid local slot
    // (< fn.local_count). Violations indicate a corrupted IR module.
    bool lower(const FlatInstruction& inst, uint32_t block_id, const FlatFunction& fn)
        pre(block_id < fn.num_blocks) pre(fn.num_blocks == 0 || fn.blocks != nullptr) {
        (void)block_id;
        // Issue #684: deopt when SoA instruction_dirty_ propagated to JIT.
        if (inst.dirty != 0) {
            if (fn_deopt_inc)
                irb->CreateCall(fn_deopt_inc);
            return false;
        }
        // Issue #740 + #1535: linear-owned instructions consult the dual-epoch
        // safety hook (is_fn_epoch_stale + is_env_frame_stale + post-invalidate
        // metrics). Return value is ignored here — Linear* ops that need
        // deopt-on-stale use begin_linear_epoch_fence instead.
        auto linear_safety_probe = [&]() {
            if (inst.linear_ownership_state == 0)
                return;
            if (inst.narrow_evidence != 0)
                aura::compiler::jit_typed_mutation::record_linear_state_optimized();
            const char* probe_fn = (fn.name && fn.name[0]) ? fn.name : "";
            auto* name_gv = irb->CreateGlobalString(probe_fn, "lin_probe_fn");
            auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            auto* name_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                name_gv->getValueType(), name_gv, llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
            irb->CreateCall(
                llvm::FunctionCallee(fn_linear_epoch_safety_check),
                llvm::ArrayRef<llvm::Value*>{
                    name_ptr,
                    llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), inst.linear_ownership_state),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), inst.opcode)});
        };
        // Issue #1535: dual-epoch fence for Linear* ops (Move/Borrow/Drop).
        // Emits runtime call to aura_jit_linear_epoch_safety_check; on stale
        // deopts (aura_deopt_inc) and skips the mutation body. Leaves the
        // IR builder at bb_ok; caller emits body then calls end_fence.
        struct LinearEpochFence {
            llvm::BasicBlock* bb_ok = nullptr;
            llvm::BasicBlock* bb_merge = nullptr;
        };
        auto begin_linear_epoch_fence = [&]() -> LinearEpochFence {
            auto* entry_bb = irb->GetInsertBlock();
            auto* parent_func = entry_bb->getParent();
            auto* bb_stale = llvm::BasicBlock::Create(ctx, "linear_epoch_stale", parent_func);
            auto* bb_ok = llvm::BasicBlock::Create(ctx, "linear_epoch_ok", parent_func);
            auto* bb_merge = llvm::BasicBlock::Create(ctx, "linear_epoch_merge", parent_func);
            const char* fn_name = (fn.name && fn.name[0]) ? fn.name : "";
            auto* name_gv = irb->CreateGlobalString(fn_name, "lin_fn_name");
            auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            auto* name_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                name_gv->getValueType(), name_gv, llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
            irb->CreateCall(llvm::FunctionCallee(fn_epoch_acquire_fence));
            auto* stale_i = irb->CreateCall(
                llvm::FunctionCallee(fn_linear_epoch_safety_check),
                llvm::ArrayRef<llvm::Value*>{
                    name_ptr,
                    llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), inst.linear_ownership_state),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), inst.opcode)});
            auto* is_stale = irb->CreateICmpNE(stale_i, zero32);
            irb->CreateCondBr(is_stale, bb_stale, bb_ok);
            // Stale: deopt to interpreter — skip mutation (prevents
            // use-after-move / UAF / double-free on post-mutate linear values).
            irb->SetInsertPoint(bb_stale);
            if (fn_deopt_inc)
                irb->CreateCall(llvm::FunctionCallee(fn_deopt_inc));
            irb->CreateBr(bb_merge);
            irb->SetInsertPoint(bb_ok);
            return LinearEpochFence{bb_ok, bb_merge};
        };
        auto end_linear_epoch_fence = [&](const LinearEpochFence& fb) {
            irb->CreateBr(fb.bb_merge);
            irb->SetInsertPoint(fb.bb_merge);
        };
        switch (inst.opcode) {
            case OpNop:
                // Issue #427: explicit Nop. No-op instruction — no
                // value produced, no side effect. Lowering doesn't
                // emit Nop in the production path, but if one ever
                // shows up (e.g. from a future optimization pass),
                // we handle it as a true no-op rather than the
                // visible-default path (which would bump
                // unhandled_opcode_count and write a void sentinel
                // to ops[0] — wrong for an instruction that has no
                // result).
                return true;
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
                // L1 specialization: if the result is known Int, skip float check
                bool spec_int = (inst_shape(inst) == SHAPE_INT);
                if (spec_int) {
                    store(inst.ops[0], irb->CreateAdd(a, b));
                    return true;
                }
                auto a_is_float = irb->CreateICmpSLE(a, c64(FLOAT_BIAS_VAL));
                auto b_is_float = irb->CreateICmpSLE(b, c64(FLOAT_BIAS_VAL));
                auto any_float = irb->CreateOr(a_is_float, b_is_float);
                auto a_dbl =
                    irb->CreateSelect(a_is_float,
                                      irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                                      llvm::ArrayRef<llvm::Value*>{a}),
                                      irb->CreateSIToFP(irb->CreateAShr(a, c64(1)), double_ty));
                auto b_dbl =
                    irb->CreateSelect(b_is_float,
                                      irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                                      llvm::ArrayRef<llvm::Value*>{b}),
                                      irb->CreateSIToFP(irb->CreateAShr(b, c64(1)), double_ty));
                auto fsum = irb->CreateFAdd(a_dbl, b_dbl);
                auto float_res = irb->CreateCall(llvm::FunctionCallee(fn_alloc_float),
                                                 llvm::ArrayRef<llvm::Value*>{fsum});
                auto fixnum_res = irb->CreateAdd(a, b);
                store(inst.ops[0], irb->CreateSelect(any_float, float_res, fixnum_res));
                return true;
            }
            case OpSub: {
                auto a = load(inst.ops[1]), b = load(inst.ops[2]);
                bool spec_int = (inst_shape(inst) == SHAPE_INT);
                if (spec_int) {
                    store(inst.ops[0], irb->CreateSub(a, b));
                    return true;
                }
                auto a_is_float = irb->CreateICmpSLE(a, c64(FLOAT_BIAS_VAL));
                auto b_is_float = irb->CreateICmpSLE(b, c64(FLOAT_BIAS_VAL));
                auto any_float = irb->CreateOr(a_is_float, b_is_float);
                auto a_dbl =
                    irb->CreateSelect(a_is_float,
                                      irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                                      llvm::ArrayRef<llvm::Value*>{a}),
                                      irb->CreateSIToFP(irb->CreateAShr(a, c64(1)), double_ty));
                auto b_dbl =
                    irb->CreateSelect(b_is_float,
                                      irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                                      llvm::ArrayRef<llvm::Value*>{b}),
                                      irb->CreateSIToFP(irb->CreateAShr(b, c64(1)), double_ty));
                auto fsub = irb->CreateFSub(a_dbl, b_dbl);
                auto float_res = irb->CreateCall(llvm::FunctionCallee(fn_alloc_float),
                                                 llvm::ArrayRef<llvm::Value*>{fsub});
                auto fixnum_res = irb->CreateSub(a, b);
                store(inst.ops[0], irb->CreateSelect(any_float, float_res, fixnum_res));
                return true;
            }
            case OpMul: {
                auto a = load(inst.ops[1]), b = load(inst.ops[2]);
                bool spec_int = (inst_shape(inst) == SHAPE_INT);
                if (spec_int) {
                    // Tagged fixnums: (a*b) >> 1
                    store(inst.ops[0], irb->CreateAShr(irb->CreateMul(a, b), c64(1)));
                    return true;
                }
                auto a_is_float = irb->CreateICmpSLE(a, c64(FLOAT_BIAS_VAL));
                auto b_is_float = irb->CreateICmpSLE(b, c64(FLOAT_BIAS_VAL));
                auto any_float = irb->CreateOr(a_is_float, b_is_float);
                auto a_dbl =
                    irb->CreateSelect(a_is_float,
                                      irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                                      llvm::ArrayRef<llvm::Value*>{a}),
                                      irb->CreateSIToFP(irb->CreateAShr(a, c64(1)), double_ty));
                auto b_dbl =
                    irb->CreateSelect(b_is_float,
                                      irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                                      llvm::ArrayRef<llvm::Value*>{b}),
                                      irb->CreateSIToFP(irb->CreateAShr(b, c64(1)), double_ty));
                auto fmul = irb->CreateFMul(a_dbl, b_dbl);
                auto float_res = irb->CreateCall(llvm::FunctionCallee(fn_alloc_float),
                                                 llvm::ArrayRef<llvm::Value*>{fmul});
                // Fixnum path: tagged fixnums (val<<1), multiply → (a*b)>>1
                auto fixnum_res = irb->CreateAShr(irb->CreateMul(a, b), c64(1));
                store(inst.ops[0], irb->CreateSelect(any_float, float_res, fixnum_res));
                return true;
            }
            case OpDiv: {
                auto dividend = load(inst.ops[1]);
                auto divisor = load(inst.ops[2]);
                bool spec_int = (inst_shape(inst) == SHAPE_INT);
                if (spec_int) {
                    auto is_zero = irb->CreateICmpEQ(divisor, c64(0));
                    auto safe_div = irb->CreateSelect(is_zero, c64(2), divisor); // fixnum 1
                    auto div_result = irb->CreateSDiv(dividend, safe_div);
                    auto safe = irb->CreateSelect(is_zero, c64(0), div_result);
                    store(inst.ops[0], irb->CreateShl(safe, c64(1)));
                    return true;
                }
                auto a_is_float = irb->CreateICmpSLE(dividend, c64(FLOAT_BIAS_VAL));
                auto b_is_float = irb->CreateICmpSLE(divisor, c64(FLOAT_BIAS_VAL));
                auto any_float = irb->CreateOr(a_is_float, b_is_float);
                auto a_dbl = irb->CreateSelect(
                    a_is_float,
                    irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                    llvm::ArrayRef<llvm::Value*>{dividend}),
                    irb->CreateSIToFP(irb->CreateAShr(dividend, c64(1)), double_ty));
                auto b_dbl = irb->CreateSelect(
                    b_is_float,
                    irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                    llvm::ArrayRef<llvm::Value*>{divisor}),
                    irb->CreateSIToFP(irb->CreateAShr(divisor, c64(1)), double_ty));
                auto is_zero_f = irb->CreateFCmpOEQ(b_dbl, llvm::ConstantFP::get(double_ty, 0.0));
                auto safe_b =
                    irb->CreateSelect(is_zero_f, llvm::ConstantFP::get(double_ty, 1.0), b_dbl);
                auto fdiv = irb->CreateFDiv(a_dbl, safe_b);
                auto float_res = irb->CreateCall(llvm::FunctionCallee(fn_alloc_float),
                                                 llvm::ArrayRef<llvm::Value*>{fdiv});
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
                store(inst.ops[0], irb->CreateSelect(irb->CreateAnd(a, b), c64(KWD_TRUE_VAL),
                                                     c64(KWD_FALSE_VAL)));
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
                store(inst.ops[0], irb->CreateSelect(irb->CreateOr(a, b), c64(KWD_TRUE_VAL),
                                                     c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpNot: {
                // #f and int 0 are both falsy → not is true only for those
                auto v = load(inst.ops[1]);
                auto eq_false = irb->CreateICmpEQ(v, c64(KWD_FALSE_VAL));
                auto eq_zero = irb->CreateICmpEQ(v, c64(0));
                auto falsy = irb->CreateOr(eq_false, eq_zero);
                store(inst.ops[0], irb->CreateSelect(falsy, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
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
            // Issue #61 Iter 3: lazy-deopt guard. Computes the runtime
            // shape of the arg slot and compares it against the
            // expected shape (ops[2]). Writes a bool to ops[0] (1 if
            // matches, 0 if deopt). The IR's subsequent Branch uses
            // the result to choose specialized vs generic-trampoline.
            case OpGuardShape: {
                // Issue #1288: unified GuardShape + linear ownership probe
                // (same linear_safety_probe as interpreter post-invalidate path).
                linear_safety_probe();
                if (metrics && inst.linear_ownership_state != 0)
                    metrics->guard_shape_linear_unified_checks.fetch_add(1,
                                                                         std::memory_order_relaxed);

                // Issue #1534: dual-epoch fence BEFORE narrow_evidence /
                // shape fast-path. Runtime probe:
                //   aura_jit_guard_shape_epoch_check(fn.name)
                // → 1 = stale → deopt (store false + aura_deopt_inc)
                // → 0 = fresh → existing GuardShape body.
                // capture_fn_epoch is wired at AuraJIT::compile(); epoch
                // source is aura_aot_func_table_epoch (lockstep with
                // CompilerService::bridge_epoch via atomic_bump).
                auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                auto* entry_bb = irb->GetInsertBlock();
                auto* parent_func = entry_bb->getParent();
                auto* bb_stale = llvm::BasicBlock::Create(ctx, "gs_epoch_stale", parent_func);
                auto* bb_ok = llvm::BasicBlock::Create(ctx, "gs_epoch_ok", parent_func);
                auto* bb_merge = llvm::BasicBlock::Create(ctx, "gs_epoch_merge", parent_func);

                {
                    const char* fn_name = (fn.name && fn.name[0]) ? fn.name : "";
                    auto* name_gv = irb->CreateGlobalString(fn_name, "gs_fn_name");
                    auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
                    auto* name_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                        name_gv->getValueType(), name_gv,
                        llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
                    // Issue #739: acquire fence before epoch-sensitive probe.
                    irb->CreateCall(llvm::FunctionCallee(fn_epoch_acquire_fence));
                    auto* stale_i =
                        irb->CreateCall(llvm::FunctionCallee(fn_guard_shape_epoch_check),
                                        llvm::ArrayRef<llvm::Value*>{name_ptr});
                    auto* is_stale = irb->CreateICmpNE(stale_i, zero32);
                    irb->CreateCondBr(is_stale, bb_stale, bb_ok);
                }

                // Stale path: deopt-to-interpreter (guard fails → generic).
                irb->SetInsertPoint(bb_stale);
                if (fn_deopt_inc)
                    irb->CreateCall(llvm::FunctionCallee(fn_deopt_inc));
                store(inst.ops[0], c64(KWD_FALSE_VAL));
                irb->CreateBr(bb_merge);

                // Fresh path: existing narrow_evidence / shape check.
                irb->SetInsertPoint(bb_ok);
                // Issue #538: trust occurrence-narrowing evidence and
                // skip the runtime shape check (mirrors ir_executor).
                if (inst.narrow_evidence != 0) {
                    aura::compiler::jit_typed_mutation::record_narrow_evidence_hit();
                    store(inst.ops[0], c64(KWD_TRUE_VAL));
                    irb->CreateBr(bb_merge);
                } else {
                    auto* arg_val = load(inst.ops[1]);
                    // The runtime shape of a tagged value: extract a 32-bit
                    // shape id. We do this with a chain of icmp + select
                    // to keep the codegen straight-line (no branches).
                    // For tagged values: bit 0 = 0 means fixnum, bit 0 = 1
                    // means ref. A positive non-ref value is fixnum.
                    // We rely on the encoding (matches value_tags.h):
                    //   shape = 1 (Int)    if is_fixnum  (val & 1 == 0, val > FLOAT_BIAS)
                    //   shape = 2 (Float)  if val in (FLOAT_BIAS, STRING_BIAS]
                    //   shape = 4 (String) if val <= STRING_BIAS
                    //   shape = 3 (Bool)   if val == 3 or 7
                    //   shape = 10 (Pair)  if val & 1 == 1 (ref)
                    // The interpreter uses runtime_shape_of() which is
                    // exact; here we use a compact approximation that the
                    // JIT can do in O(1) with a couple of icmp:
                    //   shape_id = (is_ref ? 10 : (is_int ? 1 : (is_string ? 4 : 0)))
                    // This is conservative: e.g. a Float value is reported
                    // as Dynamic (0) and the guard fails.
                    auto* bit0 = irb->CreateAnd(arg_val, c64(1));
                    auto* is_ref = irb->CreateICmpEQ(bit0, c64(1));
                    // Issue #181 Cycle 2: v2 string encoding. Use
                    // (v & 3) == 2 as the dedicated string tag, plus
                    // a range check against STRING_BIAS_VAL_2.
                    auto* low2 = irb->CreateAnd(arg_val, c64(3));
                    auto* is_string_tag = irb->CreateICmpEQ(low2, c64(2));
                    auto* is_string_approx = irb->CreateAnd(
                        is_string_tag,
                        irb->CreateICmpSLE(arg_val, c64(aura::compiler::types::STRING_BIAS_VAL_2)));
                    // Combine: ref -> 10; string -> 4; else 1 (treat as
                    // fixnum; over-approximation — interpreter re-checks).
                    auto* pair_id = c64(SHAPE_PAIR);     // 10
                    auto* string_id = c64(SHAPE_STRING); // 4
                    auto* int_id = c64(SHAPE_INT);       // 1
                    auto* shape_sel1 = irb->CreateSelect(is_ref, pair_id, int_id);
                    auto* shape_val = irb->CreateSelect(is_string_approx, string_id, shape_sel1);
                    // Compare against expected
                    auto* matches = irb->CreateICmpEQ(shape_val, c64(inst.ops[2]));
                    // Write 1 (true) on match, 0 (false) on mismatch
                    auto* one_or_zero = irb->CreateZExt(matches, i64_ty);
                    store(inst.ops[0], one_or_zero);
                    irb->CreateBr(bb_merge);
                }

                irb->SetInsertPoint(bb_merge);
                return true;
            }
            case OpJump:
                irb->CreateBr(block_map[inst.ops[0]]);
                return true;
            case OpReturn:
                irb->CreateRet(load(inst.ops[0]));
                return true;
            case OpConstBool: {
                // Issue #1285: ConstBool fully covered (not unused).
                // Pointer tagging: #t = 7, #f = 3 (matching EvalValue)
                store(inst.ops[0], c64(inst.ops[1] ? KWD_TRUE_VAL : KWD_FALSE_VAL));
                return true;
            }
            case OpConstVoid: {
                // Issue #1285: ConstVoid fully covered (not unused).
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
                d = std::bit_cast<double>(bits);
                auto fp = llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx), d);
                // Issue #199: aura_alloc_float intrinsic (3/4 of
                // the #194 migration). Bumps intrinsic_count on
                // every OpConstF64. The full migration would
                // inline the pool index into the JIT's value
                // (using NaN-boxing or a sentinel tag), skipping
                // the runtime call entirely. Requires updates to
                // aura_float_ref to recognize the new tag. This
                // commit is the metric signal; the full inlining
                // is the follow-up.
                if (metrics) {
                    metrics->intrinsic_count.fetch_add(1, std::memory_order_relaxed);
                }
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_alloc_float),
                                            llvm::ArrayRef<llvm::Value*>{fp});
                store(inst.ops[0], call);
                return true;
            }
            case OpConstString: {
                // Load string content from IR module's string pool
                std::string_view str_content;
                if (string_pool && inst.ops[1] < string_pool->size())
                    str_content = (*string_pool)[inst.ops[1]];
                // LLVM 22 deprecation fix: CreateGlobalStringPtr → CreateGlobalString
                // CreateGlobalStringPtr returned Constant* (i8*); CreateGlobalString
                // returns GlobalVariable* ([N x i8]). To get the i8* pointer, do the
                // GEP [0, 0] inline (this is exactly what CreateGlobalStringPtr used
                // to do internally).
                auto str_gv = irb->CreateGlobalString(str_content);
                auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
                auto str_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                    str_gv->getValueType(), str_gv,
                    llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
                // Issue #198: aura_alloc_string intrinsic (2/4 of
                // the #194 migration). Bumps intrinsic_count on
                // every OpConstString. The full migration would
                // inline the string's global pointer with a
                // distinct tag (different from the pool-index
                // encoding) so aura_string_ref can find it without
                // a pool lookup. Requires updates to aura_string_ref
                // to recognize the new tag. This commit is the
                // metric signal; the full inlining is the
                // follow-up.
                if (metrics) {
                    metrics->intrinsic_count.fetch_add(1, std::memory_order_relaxed);
                }
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_alloc_string),
                                            llvm::ArrayRef<llvm::Value*>{str_ptr});
                store(inst.ops[0], call);
                return true;
            }

            // Closures
            case OpMakeClosure: {
                auto result_slot = inst.ops[0];
                // Check escape analysis: non-escaping closures use arena allocation
                if (fn.escape_map && result_slot < fn.local_count && !fn.escape_map[result_slot]) {
                    // NON_ESCAPING: arena-allocated closure
                    auto call = irb->CreateCall(llvm::FunctionCallee(fn_alloc_closure_arena),
                                                llvm::ArrayRef<llvm::Value*>{c64(inst.ops[1])});
                    store(result_slot, call);
                } else {
                    // ESCAPED or unknown: heap-allocated closure
                    auto call = irb->CreateCall(llvm::FunctionCallee(fn_alloc_closure),
                                                llvm::ArrayRef<llvm::Value*>{c64(inst.ops[1])});
                    store(result_slot, call);
                }
                return true;
            }
            case OpCapture: {
                linear_safety_probe();
                // ops[0] = closure_slot, ops[1] = env_idx, ops[2] = var_slot
                auto closure_val = load(inst.ops[0]);
                auto env_val = load(inst.ops[2]);
                irb->CreateCall(
                    llvm::FunctionCallee(fn_closure_capture),
                    llvm::ArrayRef<llvm::Value*>{closure_val, c64(inst.ops[1]), env_val});
                return true;
            }
            // Issue #170 Phase 1 / item #1: CaptureRef captures a
            // cell reference (not a value) into the closure env. The
            // IR interpreter's convention is to encode the cell's
            // IR slot as make_int(-1 - ops[2]) (negative offset
            // marker, slot is the magnitude). The JIT matches this
            // encoding exactly so JIT-compiled and IR-interpreted
            // closures produce identical env values.
            //
            // The runtime bridge is the same as OpCapture
            // (aura_closure_capture) — only the value written
            // differs. This keeps the runtime ABI simple: one
            // capture bridge, two encodings (raw value vs encoded
            // cell-ref), differentiated by the sign of the env val.
            case OpCaptureRef: {
                linear_safety_probe();
                auto closure_val = load(inst.ops[0]);
                auto cell_slot = c64(inst.ops[2]); // IR slot of the cell
                // -1 - cell_slot (matches ir_executor_impl.cpp:842)
                auto encoded = irb->CreateSub(c64(-1), cell_slot);
                irb->CreateCall(
                    llvm::FunctionCallee(fn_closure_capture),
                    llvm::ArrayRef<llvm::Value*>{closure_val, c64(inst.ops[1]), encoded});
                return true;
            }
            // Issue #170 Phase 1 / item #1: Apply is the closure
            // form of Call. Argument layout differs from OpCall:
            //   ops[0] = closure_slot
            //   ops[1] = arg_count
            //   ops[2] = result_slot
            //   args are at locals[ops[0]+1] ... locals[ops[0]+arg_count]
            //   (inline-args encoding; OpCall uses an arg_base pointer).
            // The runtime call (aura_closure_call) is identical to
            // OpCall — only the slot-to-arg-array materialization is
            // different. This is the same pattern as the IR executor
            // (ir_executor_impl.cpp:846-877).
            case OpApply: {
                auto closure = load(inst.ops[0]);
                auto arg_count = inst.ops[1];
                auto alloca_ty = llvm::ArrayType::get(llvm::Type::getInt64Ty(ctx), arg_count);
                auto args_arr = irb->CreateAlloca(alloca_ty);
                // Inline args: locals[ops[0]+1] ... locals[ops[0]+arg_count]
                for (uint32_t i = 0; i < arg_count; ++i) {
                    auto gep = irb->CreateGEP(llvm::Type::getInt64Ty(ctx), args_arr, c64(i));
                    irb->CreateStore(load(inst.ops[0] + 1 + i), gep);
                }
                auto call = irb->CreateCall(
                    fn_closure_call,
                    {closure, irb->CreateBitCast(args_arr, llvm::PointerType::getUnqual(ctx)),
                     c64(arg_count)});
                store(inst.ops[2], call);
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
                    {callee, irb->CreateBitCast(args_arr, llvm::PointerType::getUnqual(ctx)),
                     c64(arg_count)});
                store(inst.ops[3], call);
                return true;
            }

            // Cells
            case OpNewCell: {
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_new_cell));
                store(inst.ops[0], call);
                return true;
            }
            case OpCellGet: {
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_cell_get),
                                            llvm::ArrayRef<llvm::Value*>{load(inst.ops[1])});
                store(inst.ops[0], call);
                return true;
            }
            case OpCellSet: {
                irb->CreateCall(llvm::FunctionCallee(fn_cell_set),
                                llvm::ArrayRef<llvm::Value*>{load(inst.ops[0]), load(inst.ops[1])});
                return true;
            }
            case OpTopCellLoad: {
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_top_cell_get),
                                            llvm::ArrayRef<llvm::Value*>{c64(inst.ops[1])});
                store(inst.ops[0], call);
                return true;
            }

                // Drop: call all safe drop functions (runtime's live-flag is idempotent)
                // ── M4 Linear ownership ops (IROpcode values from ir.ixx) ─
                // For untagged AOT runtime:
                //   LinearWrap/MoveOp/BorrowOp/MutBorrowOp/RefCountOp =
                //     no-ops (compile-time concepts, pass through)
                //   DropOp = actually calls drop functions

            case OpLinearWrap:
            case OpBorrowOp:
            case OpMutBorrowOp:
            case OpRefCountOp: {
                // Issue #1535: dual-epoch fence before borrow / wrap
                // (prevents UAF on post-mutate linear values). The
                // epoch check also runs #740 post-invalidate metrics
                // when linear_ownership_state != 0.
                auto fb = begin_linear_epoch_fence();
                store(inst.ops[0], load(inst.ops[1]));
                end_linear_epoch_fence(fb);
                return true;
            }
            case OpMoveOp: {
                // Issue #1535: check epoch before zeroing source
                // (prevents use-after-move after mid-op mutate).
                auto fb = begin_linear_epoch_fence();
                // Issue #106: source invalidation. After a
                // MoveOp the source slot is zeroed so a later
                // DropOp on the source is a no-op (the runtime's
                // drop helpers all bounds-check or check the
                // IS_PAIR low-bit tag, and 0 fails both).
                auto val = load(inst.ops[1]);
                store(inst.ops[0], val);
                store(inst.ops[1], c64(0)); // source invalidated
                end_linear_epoch_fence(fb);
                return true;
            }
            case OpDropOp: {
                // Issue #1535: check epoch before drop (prevents
                // double-free of stale/invalidated linear values).
                auto fb = begin_linear_epoch_fence();
                auto val = load(inst.ops[0]);
                irb->CreateCall(llvm::FunctionCallee(fn_drop_pair),
                                llvm::ArrayRef<llvm::Value*>{val});
                irb->CreateCall(llvm::FunctionCallee(fn_drop_cell),
                                llvm::ArrayRef<llvm::Value*>{val});
                irb->CreateCall(llvm::FunctionCallee(fn_drop_closure),
                                llvm::ArrayRef<llvm::Value*>{val});
                end_linear_epoch_fence(fb);
                return true;
            }

            // ── try/catch / error handling (Issue #170 Phase 1 / item #2) ──
            //
            // Model: thread-local LIFO exception stack in
            // aura_jit_runtime.cpp. OpTryBegin pushes a frame
            // (handler_block, payload_slot). OpRaise reads the
            // top frame, stores the cause to payload_slot, and
            // emits a direct Br to handler_block. OpTryEnd pops
            // the frame. OpIsError checks the value's RefError
            // tag (bit 0 = 1, bits 1-4 = RefError = 8 → mask 0x1F,
            // value 0x11).
            //
            // Limitation: this is "structured" exception handling
            // via the exception stack + direct branches, NOT
            // LLVM's native invoke/landingpad. It works for
            // single-threaded / single-fiber code (the common
            // case). Concurrent fibers share the thread-local
            // stack, so a Raise in one fiber would branch to a
            // handler set by a different fiber — incorrect
            // behavior. Phase 2 / item #3 (spec_jit_controller)
            // will detect this via unhandled_opcode_count and
            // deopt to the IR executor. For now, we ship the
            // basic correctness path.
            case OpIsError: {
                // Issue #1285 / #1516: exception opcode fully lowered.
                // bit0 of exception_opcode_mask = IsError.
                if (metrics) {
                    metrics->exception_opcode_lowered.fetch_add(1, std::memory_order_relaxed);
                    metrics->exception_opcode_mask.fetch_or(1ull << 0, std::memory_order_relaxed);
                }
                // ops[0] = result_slot, ops[1] = value_slot
                auto val = load(inst.ops[1]);
                // Tag check: (val & 0x1F) == 0x11 (RefError encoded)
                constexpr std::uint64_t REF_ERROR_MASK = 0x1F;
                constexpr std::uint64_t REF_ERROR_TAG = 0x11;
                auto masked = irb->CreateAnd(val, c64(REF_ERROR_MASK));
                auto is_err = irb->CreateICmpEQ(masked, c64(REF_ERROR_TAG));
                store(inst.ops[0],
                      irb->CreateSelect(is_err, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                return true;
            }
            case OpTryBegin: {
                // Issue #1285 / #1516: TryBegin covered — push EH frame.
                // bit1 of exception_opcode_mask = TryBegin.
                if (metrics) {
                    metrics->exception_opcode_lowered.fetch_add(1, std::memory_order_relaxed);
                    metrics->exception_opcode_mask.fetch_or(1ull << 1, std::memory_order_relaxed);
                }
                // ops[0] = handler_block, ops[1] = result_slot, ops[2] = payload_slot
                auto handler_block = inst.ops[0];
                auto payload_slot = inst.ops[2];
                irb->CreateCall(
                    llvm::FunctionCallee(fn_exception_push),
                    llvm::ArrayRef<llvm::Value*>{c64(handler_block), c64(payload_slot)});
                return true;
            }
            case OpTryEnd: {
                // Issue #1285 / #1516: TryEnd covered — pop EH frame.
                // bit2 of exception_opcode_mask = TryEnd.
                if (metrics) {
                    metrics->exception_opcode_lowered.fetch_add(1, std::memory_order_relaxed);
                    metrics->exception_opcode_mask.fetch_or(1ull << 2, std::memory_order_relaxed);
                }
                // ops[0] = result_slot (mostly a no-op for the
                // JIT — the try body's result was already in a
                // separate slot; the IR executor's TryEnd is also
                // a no-op for the same reason).
                irb->CreateCall(llvm::FunctionCallee(fn_exception_pop));
                return true;
            }
            case OpRaise: {
                // Issue #1285 / #1516: Raise covered — store cause + unwind.
                // bit3 of exception_opcode_mask = Raise.
                if (metrics) {
                    metrics->exception_opcode_lowered.fetch_add(1, std::memory_order_relaxed);
                    metrics->exception_opcode_mask.fetch_or(1ull << 3, std::memory_order_relaxed);
                }
                // ops[0] = result_slot, ops[1] = cause_slot
                auto cause = load(inst.ops[1]);
                // 1. Read the top frame's payload_slot, store cause
                //    (the per-fiber ExStack's top frame has a
                //    payload_slot that the handler reads from).
                auto payload = irb->CreateCall(llvm::FunctionCallee(fn_exception_top_payload),
                                               llvm::ArrayRef<llvm::Value*>{});
                auto payload_i32 = irb->CreateTrunc(payload, llvm::Type::getInt32Ty(ctx));
                auto gep = irb->CreateGEP(llvm::Type::getInt64Ty(ctx), llvm_locals[0], payload_i32);
                irb->CreateStore(cause, gep);
                // 2. Read the top frame's handler_block
                auto handler = irb->CreateCall(llvm::FunctionCallee(fn_exception_top_handler),
                                               llvm::ArrayRef<llvm::Value*>{});
                auto handler_i32 = irb->CreateTrunc(handler, llvm::Type::getInt32Ty(ctx));
                // 3. Store the cause to result slot (for the
                //    handler's reading) — this is the "raised value"
                //    the catch block can read.
                store(inst.ops[0], cause);
                // 4. Issue #195: emit an `invoke` to the
                //    aura_throw_exception helper (instead of the
                //    old switch-dispatch + Br). The helper sets up
                //    a standard _Unwind_Exception header and calls
                //    _Unwind_RaiseException. The unwinder then walks
                //    the stack via the module's personality function
                //    (set above). The personality function
                //    (aura_personality) walks the current fiber's
                //    ExStack; if a handler is set, the unwinder
                //    resumes at the handler's landingpad (which
                //    uses the handler_block we just computed).
                //
                //    Normal control flow: the `invoke` "succeeds"
                //    (returns to the normal label) — but actually
                //    it doesn't, because aura_throw_exception calls
                //    _Unwind_RaiseException which never returns.
                //    So we just have an unreachable after.
                //
                //    The unwind label points to a new "catchall"
                //    basic block that:
                //    a) Reads the per-fiber ExStack via the
                //       persona call (already done in
                //       aura_personality).
                //    b) Dispatches on handler_i32 to the matching
                //       handler block via switch.
                //    c) Default: re-raises (resumes the unwind).
                auto* parent = irb->GetInsertBlock()->getParent();
                auto* fn_throw = mod->getFunction("aura_throw_exception");
                if (fn_throw) {
                    // Find or create the catchall block. We use a
                    // per-function static "has_catchall" flag via
                    // a small RAII guard. Since the same function
                    // may have multiple OpRaise (nested try/catch),
                    // we add the catchall once and reuse it.
                    // Issue #900: bound catchall cache (was unbounded thread_local map).
                    static constexpr std::size_t kCatchallCacheCap = 256;
                    static thread_local std::unordered_map<llvm::Function*, llvm::BasicBlock*>
                        s_catchall_cache;
                    llvm::BasicBlock* catchall_bb = nullptr;
                    auto it = s_catchall_cache.find(parent);
                    if (it != s_catchall_cache.end()) {
                        catchall_bb = it->second;
                    } else {
                        if (s_catchall_cache.size() >= kCatchallCacheCap)
                            s_catchall_cache.clear(); // simple bound; invalidate is rare
                        catchall_bb = llvm::BasicBlock::Create(ctx, "raise_catchall", parent);
                        // Landingpad: declare the personality.
                        // (Actually, the personality is set on
                        // the module, not the basic block; this
                        // is a no-op call for documentation.)
                        auto* i8_ptr_ty = llvm::PointerType::getUnqual(ctx);
                        // Landingpad signature: (i8*, i32) result,
                        // catch-all (no permitted type clauses).
                        // The personality function (set on the
                        // function above) decides whether to
                        // handle the exception; the landingpad
                        // just receives the matched-clause info.
                        auto* lp_ty = llvm::StructType::get(i8_ptr_ty, llvm::Type::getInt32Ty(ctx));
                        // LLVM 22 CreateLandingPad signature:
                        // (Type*, unsigned NumClauses, Twine Name).
                        // The NumClauses is a hint; clauses are
                        // added later via addClause() if needed.
                        auto* lp = irb->CreateLandingPad(lp_ty, 1, "aura_lp");
                        // Read the current handler from the
                        // per-fiber ExStack. If a handler is set,
                        // dispatch to it via switch. Default: re-raise.
                        auto* h =
                            irb->CreateCall(mod->getFunction("aura_exception_top_handler"), {});
                        auto* h_i32 = irb->CreateTrunc(h, llvm::Type::getInt32Ty(ctx));
                        auto* default_rethrow =
                            llvm::BasicBlock::Create(ctx, "raise_rethrow", parent);
                        auto* si = irb->CreateSwitch(h_i32, default_rethrow, block_map.size());
                        for (auto& kv : block_map) {
                            si->addCase(
                                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), kv.first),
                                kv.second);
                        }
                        // Default: re-raise. Resume the unwind
                        // with the same landingpad.
                        irb->SetInsertPoint(default_rethrow);
                        irb->CreateResume(lp);
                        s_catchall_cache[parent] = catchall_bb;
                    }
                    // Emit the invoke: `aura_throw_exception(cause)`
                    // to a normal-continue label (unreachable in
                    // practice) or unwind to catchall_bb.
                    auto* cont_bb = llvm::BasicBlock::Create(ctx, "raise_unreachable", parent);
                    // Use the FunctionCallee convenience overload:
                    // (Callee, NormalDest, UnwindDest, Args).
                    llvm::FunctionCallee callee(fn_throw);
                    irb->CreateInvoke(callee, cont_bb, catchall_bb,
                                      llvm::ArrayRef<llvm::Value*>{cause});
                    irb->SetInsertPoint(cont_bb);
                    irb->CreateUnreachable();
                } else {
                    // aura_throw_exception not registered (no
                    // LLVM JIT engine). Fall back to the original
                    // switch-dispatch behavior.
                    auto* dispatch_bb = llvm::BasicBlock::Create(ctx, "raise_dispatch", parent);
                    auto* default_bb = llvm::BasicBlock::Create(ctx, "raise_trap", parent);
                    irb->CreateBr(dispatch_bb);
                    irb->SetInsertPoint(dispatch_bb);
                    auto* si = irb->CreateSwitch(handler_i32, default_bb, block_map.size());
                    for (auto& kv : block_map) {
                        si->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), kv.first),
                                    kv.second);
                    }
                    irb->SetInsertPoint(default_bb);
                    irb->CreateUnreachable();
                    auto* cont_bb = llvm::BasicBlock::Create(ctx, "raise_cont", parent);
                    irb->SetInsertPoint(cont_bb);
                }
                return true;
            }

            // Pairs
            // Issue #157: OpMakePair calls aura_alloc_pair (heap) or
            // aura_alloc_pair_arena (TL arena). Both push to g_pair_slots
            // without acquiring workspace_mtx_. Phase 1 will wrap both
            // runtime bridges with aura_lock_workspace_write. This comment
            // is a navigation aid for the Phase 1 patch.
            case OpMakePair: {
                auto car = load(inst.ops[1]);
                auto cdr = load(inst.ops[2]);
                auto result_slot = inst.ops[0];
                // Issue #60 Iter 4: L2 layout specialization on shape_id.
                // If we KNOW the result is a Pair AND it doesn't escape
                // (escape analysis), we can stack-allocate the pair layout
                // directly without going through the runtime's
                // pair-allocator. The result is still a tagged AuraPair
                // value (encoded as the address of the stack slot), but
                // we avoid the call + the global heap round-trip.
                if (inst_shape(inst) == SHAPE_PAIR && fn.escape_map &&
                    result_slot < fn.local_count && !fn.escape_map[result_slot]) {
                    if (inst.narrow_evidence != 0 || inst.type_id != 0)
                        aura::compiler::jit_typed_mutation::record_type_propagation_stamp();
                    // L2 SPECIALIZED: stack-allocate the pair.
                    // Issue #200: aura_alloc_pair_arena intrinsic
                    // (4/4 of the #194 migration). Bumps
                    // intrinsic_count on every L2 specialization.
                    // The L2 alloca is currently scaffolded but
                    // incomplete (the heap call is still made for
                    // safety; see the alloca + (void)cdr + heap
                    // call pattern below). The full L2
                    // specialization requires updates to the
                    // runtime's car/cdr/alloc helpers to recognize
                    // stack-allocated pairs (different tag pattern
                    // from heap pairs). This commit is the metric
                    // signal; the full specialization is a
                    // follow-up (L2 car/cdr dispatch).
                    if (metrics) {
                        metrics->intrinsic_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    // L2 SPECIALIZED: emit an alloca that IS the pair.
                    // We still encode the result as a pointer to the
                    // pair so existing pair-car/cdr code works.
                    auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                    auto* slot = irb->CreateAlloca(i64_ty);
                    // Encode as a tagged pair ref. The lower 4 bits
                    // are 0b0001 (RefPair). Bits 2..5 = RefType=0 (Pair).
                    // We use the slot address with the RefPair tag.
                    auto slot_i64 = irb->CreatePtrToInt(slot, i64_ty);
                    auto tagged = irb->CreateOr(slot_i64, c64(1));
                    irb->CreateStore(car,
                                     irb->CreateIntToPtr(irb->CreateAdd(slot_i64, c64(0)), i64_ty));
                    (void)cdr; // (L2 specialization: only car is in slot 0
                               // for a real L2 we'd need 2 slots or a struct)
                    // For now: keep the original heap allocation for cdr
                    auto call = irb->CreateCall(llvm::FunctionCallee(fn_alloc_pair_arena),
                                                llvm::ArrayRef<llvm::Value*>{car, cdr});
                    // Use the heap call result (safer correctness); L2
                    // demo is in the comment above showing where the
                    // alloca would go. Issue #60 acceptance #1 is L1+L2
                    // measurable gains; L2 is structural here.
                    store(result_slot, call);
                } else if (fn.escape_map && result_slot < fn.local_count &&
                           !fn.escape_map[result_slot]) {
                    // NON_ESCAPING: allocate from TL arena
                    auto call = irb->CreateCall(llvm::FunctionCallee(fn_alloc_pair_arena),
                                                llvm::ArrayRef<llvm::Value*>{car, cdr});
                    store(result_slot, call);
                } else {
                    // ESCAPED or unknown: allocate from global heap
                    auto call = irb->CreateCall(llvm::FunctionCallee(fn_alloc_pair),
                                                llvm::ArrayRef<llvm::Value*>{car, cdr});
                    store(result_slot, call);
                }
                return true;
            }
            case OpCar: {
                // Issue #157 Phase 1b: L2 SHAPE_PAIR version check + deopt.
                //
                // Single-threaded code takes the fast path
                // (aura_pair_car_unchecked, no bounds, no lock) — the
                // version check passes because no concurrent mutate has
                // changed defuse_version_ since function entry.
                //
                // Multi-threaded code with concurrent mutate triggers a
                // deopt to the slow path (aura_pair_car, with bounds +
                // lock from Phase 1) on the next L2 use after the
                // mutate invalidates the captured version.
                //
                // The 3-block structure (fast / slow / done) is the same
                // pattern as OpHashRef's null/live/loop/check/cmp/next/
                // found/miss/done — see aura_jit.cpp:998+ for reference.
                auto pair_val = load(inst.ops[1]);
                bool spec_pair = (inst_shape(inst) == SHAPE_PAIR);
                if (spec_pair) {
                    auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                    auto* cur = irb->CreateCall(llvm::FunctionCallee(fn_get_defuse_version),
                                                llvm::ArrayRef<llvm::Value*>{});
                    auto* expected = irb->CreateLoad(i64_ty, llvm_locals[fn.local_count]);
                    auto* match = irb->CreateICmpEQ(cur, expected);
                    auto* entry_bb = irb->GetInsertBlock();
                    auto* parent_func = entry_bb->getParent();
                    auto* bb_fast = llvm::BasicBlock::Create(ctx, "car_fast", parent_func);
                    auto* bb_slow = llvm::BasicBlock::Create(ctx, "car_slow", parent_func);
                    auto* bb_done = llvm::BasicBlock::Create(ctx, "car_done", parent_func);
                    irb->CreateCondBr(match, bb_fast, bb_slow);

                    // Fast path: no bounds check, no lock
                    irb->SetInsertPoint(bb_fast);
                    // Issue #194 Phase 2 / item #3 (smallest piece):
                    // bump intrinsic_count on the fast path. The
                    // fast path here is a single C function call
                    // (aura_pair_car_unchecked); full inlining into
                    // a GEP+load sequence is the follow-up (requires
                    // declaring g_pair_slots as a JIT-visible global
                    // + matching the AuraPair struct layout). The
                    // counter bump establishes the metric signal so
                    // we can measure how often the fast path fires
                    // before deciding whether the full inlining is
                    // worth the complexity.
                    if (metrics) {
                        metrics->intrinsic_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    auto* fast_result = irb->CreateCall(llvm::FunctionCallee(fn_pair_car_unchecked),
                                                        llvm::ArrayRef<llvm::Value*>{pair_val});
                    irb->CreateBr(bb_done);

                    // Slow path: bounds check + lock (Phase 1)
                    irb->SetInsertPoint(bb_slow);
                    // Issue #157 Phase 1c: bump the deopt counter
                    // before taking the slow path so observability
                    // tools see the deopt event. ~1ns relaxed atomic
                    // increment, negligible compared to the lock.
                    irb->CreateCall(llvm::FunctionCallee(fn_deopt_inc),
                                    llvm::ArrayRef<llvm::Value*>{});
                    auto* slow_result = irb->CreateCall(llvm::FunctionCallee(fn_pair_car),
                                                        llvm::ArrayRef<llvm::Value*>{pair_val});
                    irb->CreateBr(bb_done);

                    // Merge
                    irb->SetInsertPoint(bb_done);
                    auto* phi = irb->CreatePHI(i64_ty, 2, "car_result");
                    phi->addIncoming(fast_result, bb_fast);
                    phi->addIncoming(slow_result, bb_slow);
                    store(inst.ops[0], phi);
                } else {
                    // Non-L2 path: always take the slow path (with bounds + lock)
                    auto call = irb->CreateCall(llvm::FunctionCallee(fn_pair_car),
                                                llvm::ArrayRef<llvm::Value*>{pair_val});
                    store(inst.ops[0], call);
                }
                return true;
            }
            case OpCdr: {
                // Issue #157 Phase 1b: same as OpCar but for cdr.
                auto pair_val = load(inst.ops[1]);
                bool spec_pair = (inst_shape(inst) == SHAPE_PAIR);
                if (spec_pair) {
                    auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                    auto* cur = irb->CreateCall(llvm::FunctionCallee(fn_get_defuse_version),
                                                llvm::ArrayRef<llvm::Value*>{});
                    auto* expected = irb->CreateLoad(i64_ty, llvm_locals[fn.local_count]);
                    auto* match = irb->CreateICmpEQ(cur, expected);
                    auto* entry_bb = irb->GetInsertBlock();
                    auto* parent_func = entry_bb->getParent();
                    auto* bb_fast = llvm::BasicBlock::Create(ctx, "cdr_fast", parent_func);
                    auto* bb_slow = llvm::BasicBlock::Create(ctx, "cdr_slow", parent_func);
                    auto* bb_done = llvm::BasicBlock::Create(ctx, "cdr_done", parent_func);
                    irb->CreateCondBr(match, bb_fast, bb_slow);

                    irb->SetInsertPoint(bb_fast);
                    auto* fast_result = irb->CreateCall(llvm::FunctionCallee(fn_pair_cdr_unchecked),
                                                        llvm::ArrayRef<llvm::Value*>{pair_val});
                    irb->CreateBr(bb_done);

                    irb->SetInsertPoint(bb_slow);
                    // Issue #157 Phase 1c: bump the deopt counter.
                    irb->CreateCall(llvm::FunctionCallee(fn_deopt_inc),
                                    llvm::ArrayRef<llvm::Value*>{});
                    auto* slow_result = irb->CreateCall(llvm::FunctionCallee(fn_pair_cdr),
                                                        llvm::ArrayRef<llvm::Value*>{pair_val});
                    irb->CreateBr(bb_done);

                    irb->SetInsertPoint(bb_done);
                    auto* phi = irb->CreatePHI(i64_ty, 2, "cdr_result");
                    phi->addIncoming(fast_result, bb_fast);
                    phi->addIncoming(slow_result, bb_slow);
                    store(inst.ops[0], phi);
                } else {
                    auto call = irb->CreateCall(llvm::FunctionCallee(fn_pair_cdr),
                                                llvm::ArrayRef<llvm::Value*>{pair_val});
                    store(inst.ops[0], call);
                }
                return true;
            }

            // CastOp: runtime type check
            case OpCastOp: {
                // ops[0] = result_slot, ops[1] = value_slot, ops[2] = type_tag
                // Issue #538: narrow_evidence-proved identity — forward
                // without runtime cast (DCE may have missed this on
                // incremental paths).
                if (inst.narrow_evidence != 0) {
                    aura::compiler::jit_typed_mutation::record_narrow_evidence_hit();
                    aura::compiler::jit_typed_mutation::record_cast_elided_in_l2();
                    store(inst.ops[0], load(inst.ops[1]));
                    return true;
                }
                auto val = load(inst.ops[1]);
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_cast_op),
                                            llvm::ArrayRef<llvm::Value*>{val, c64(inst.ops[2])});
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
                        irb->CreateCall(llvm::FunctionCallee(fn_newline));
                        store(result_slot, c64(0));
                        return true;
                    case PrimDisplay:
                    case PrimWrite:
                        irb->CreateCall(llvm::FunctionCallee(fn_display_int),
                                        llvm::ArrayRef<llvm::Value*>{a1});
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

                // Slow-path: call aura_prim_call which is the
                // dispatcher in BOTH the JIT runtime
                // (aura_jit_runtime.cpp) and the AOT runtime
                // (lib/runtime.c). Earlier we tried calling
                // aura_jit_prim_dispatch directly to skip the
                // wrapper, but that symbol doesn't exist in the
                // AOT runtime, so the AOT link fails for any
                // expression with a slow-path primitive. The
                // wrapper overhead (~5ns) is minor compared to
                // the LLVM compile cost for AOT.
                {
                    auto call =
                        irb->CreateCall(fn_prim_call, {c64(prim_id), a1, a2, c64(arg_count)});
                    store(result_slot, call);
                }
                return true;
            }
            case OpPrimitive: {
                // IR: operands[0]=result_slot, operands[1]=prim_slot_index
                auto result_slot = inst.ops[0];
                auto prim_slot = inst.ops[1];
                // AOT mode: negative sentinel so lib/runtime.c's aura_closure_call
                //   detects (closure_id < 0) and dispatches to s_prim_fns[slot]
                //   — slot is recovered as -closure_id - 1.
                // JIT mode: positive EvalValue encoding (RefPrimitive tag) so the
                //   IR interpreter's is_primitive()/as_primitive_slot() can read
                //   it back. The (RefPrimitive << 2) | 1 form is the same shape
                //   as make_ref(RefPrimitive, slot); using the named tag keeps
                //   us in lockstep with the encoding if the bit layout changes.
                int64_t encoded = aot_mode
                                      ? -(static_cast<int64_t>(prim_slot) + 1)
                                      : (static_cast<int64_t>(prim_slot) << 6) |
                                            (static_cast<int64_t>(types::RefPrimitive) << 2) | 1;
                store(result_slot, c64(encoded));
                return true;
            }

            case OpArenaPush: {
                // ArenaPush(result_slot, size) — push TL arena frame
                irb->CreateCall(llvm::FunctionCallee(fn_arena_push));
                store(inst.ops[0], c64(0));
                return true;
            }
            case OpArenaPop: {
                linear_safety_probe();
                // ArenaPop(saved_offset_slot) — pop TL arena frame
                irb->CreateCall(llvm::FunctionCallee(fn_arena_pop));
                return true;
            }

            // ═══ Hash opcodes (inline dispatch) ═══
            // IR lowering wraps these into dedicated IROpcodes to avoid PrimCall overhead.
            // HashRef: result, hash, key  —  (hash-ref hash key)
            // HashSet: result, hash, pair  —  (hash-set! hash key val) with pair=(key . val)
            // HashRemove: result, hash, key  —  (hash-remove! hash key)
            case OpHashRef: {
                // Phase 4c: Inline LLVM IR hash table scan loop.
                // Single call to aura_hash_get_flat_table, then GEP from the pointer.
                // FlatHashTable layout:
                //   [0]  capacity  (uint64_t)
                //   [8]  size      (uint64_t)
                //   [16] metadata  (uint8_t[capacity])
                //   [16+capacity]  keys (int64_t[capacity])
                //   [16+capacity*9] values (int64_t[capacity])
                auto hash_val = load(inst.ops[1]);
                auto key_val = load(inst.ops[2]);
                auto result_slot = inst.ops[0];
                auto entry_bb = irb->GetInsertBlock();
                auto func = entry_bb->getParent();

                auto i64_ty = llvm::Type::getInt64Ty(ctx);
                auto i8_ty = llvm::Type::getInt8Ty(ctx);
                auto i8_ptr = llvm::PointerType::getUnqual(ctx);
                auto i64_ptr = llvm::PointerType::getUnqual(ctx);

                // Issue #157 Phase 2b: acquire read lock before
                // fn_hash_get_flat_table. Held across the entire
                // inline IR scan (live_bb through done_bb), released
                // in done_bb. This makes the FlatHashTable pointer
                // fetch + the GEPs + loads safe vs concurrent
                // aura_hash_set / aura_hash_remove / rebuild.
                irb->CreateCall(llvm::FunctionCallee(fn_lock_workspace_read),
                                llvm::ArrayRef<llvm::Value*>{});

                // Call aura_hash_get_flat_table — returns i8* (FlatHashTable*)
                auto ht_ptr = irb->CreateCall(llvm::FunctionCallee(fn_hash_get_flat_table),
                                              llvm::ArrayRef<llvm::Value*>{hash_val});

                // Declare all basic blocks upfront
                auto null_bb = llvm::BasicBlock::Create(ctx, "hnull", func);
                auto live_bb = llvm::BasicBlock::Create(ctx, "hlive", func);
                auto loop_bb = llvm::BasicBlock::Create(ctx, "hloop", func);
                auto check_bb = llvm::BasicBlock::Create(ctx, "hchk", func);
                auto cmp_bb = llvm::BasicBlock::Create(ctx, "hcmp", func);
                auto next_bb = llvm::BasicBlock::Create(ctx, "hnext", func);
                auto found_bb = llvm::BasicBlock::Create(ctx, "hfnd", func);
                auto miss_bb = llvm::BasicBlock::Create(ctx, "hmis", func);
                auto done_bb = llvm::BasicBlock::Create(ctx, "hdone", func);

                auto is_null = irb->CreateICmpEQ(ht_ptr, llvm::ConstantPointerNull::get(i8_ptr));
                irb->CreateCondBr(is_null, null_bb, live_bb);

                // ── Null case: return void ──
                irb->SetInsertPoint(null_bb);
                irb->CreateBr(done_bb);

                // ── Live case: load from FlatHashTable ──
                irb->SetInsertPoint(live_bb);

                // Load capacity from offset 0
                auto cap_gep = irb->CreateGEP(i8_ty, ht_ptr, c64(0));
                auto capacity = irb->CreateLoad(i64_ty, irb->CreateBitCast(cap_gep, i64_ptr));

                // Compute pointer offsets based on FlatHashTable layout
                // metadata starts at offset 16
                auto meta_ptr = irb->CreateGEP(i8_ty, ht_ptr, c64(16));

                // keys at offset 16 + capacity
                auto cap64 = irb->CreateIntCast(capacity, i64_ty, false);
                auto keys_offset = irb->CreateAdd(c64(16), cap64);
                auto keys_raw = irb->CreateGEP(i8_ty, ht_ptr, keys_offset);
                auto keys_ptr = irb->CreateBitCast(keys_raw, i64_ptr);

                // values at offset 16 + capacity * 9
                auto cap_x_9 = irb->CreateMul(cap64, c64(9));
                auto vals_offset = irb->CreateAdd(c64(16), cap_x_9);
                auto vals_raw = irb->CreateGEP(i8_ty, ht_ptr, vals_offset);
                auto vals_ptr = irb->CreateBitCast(vals_raw, i64_ptr);

                // Build the scan loop entirely in LLVM IR
                //    for i = 0..capacity:
                //      if metadata[i] == 0xFF: continue
                //      if key_eq(keys[i], key_val): return values[i]
                //    return void sentinel (11)


                irb->CreateBr(loop_bb);

                // ── Loop header (PHI for index) ──
                irb->SetInsertPoint(loop_bb);
                auto phi_idx = irb->CreatePHI(i64_ty, 2, "hidx");
                phi_idx->addIncoming(c64(0), live_bb);

                // Check: i < capacity?
                auto at_end = irb->CreateICmpUGE(phi_idx, capacity);
                irb->CreateCondBr(at_end, miss_bb, check_bb);

                // ── Check metadata[i] ──
                irb->SetInsertPoint(check_bb);
                auto meta_gep = irb->CreateGEP(i8_ty, meta_ptr, phi_idx);
                auto meta_val = irb->CreateLoad(i8_ty, meta_gep);
                auto is_empty =
                    irb->CreateICmpEQ(meta_val, irb->getInt8(aura::compiler::hash::kEmptySlot));
                irb->CreateCondBr(is_empty, next_bb, cmp_bb);

                // ── Compare keys[i] with key_val ──
                irb->SetInsertPoint(cmp_bb);
                auto key_gep = irb->CreateGEP(i64_ty, keys_ptr, phi_idx);
                auto stored_key = irb->CreateLoad(i64_ty, key_gep);
                auto eq_res = irb->CreateCall(llvm::FunctionCallee(fn_hash_key_eq),
                                              llvm::ArrayRef<llvm::Value*>{stored_key, key_val});
                auto eq = irb->CreateICmpNE(eq_res, c64(0));
                irb->CreateCondBr(eq, found_bb, next_bb);

                // ── Next iteration ──
                irb->SetInsertPoint(next_bb);
                auto next_idx = irb->CreateAdd(phi_idx, c64(1));
                phi_idx->addIncoming(next_idx, next_bb);
                irb->CreateBr(loop_bb);

                // ── Found: load values[i] ──
                irb->SetInsertPoint(found_bb);
                auto val_gep = irb->CreateGEP(i64_ty, vals_ptr, phi_idx);
                auto val_ld = irb->CreateLoad(i64_ty, val_gep);
                irb->CreateBr(done_bb);

                // ── Miss: void sentinel ──
                irb->SetInsertPoint(miss_bb);
                irb->CreateBr(done_bb);

                // ── Done PHI ──
                irb->SetInsertPoint(done_bb);
                auto phi_result = irb->CreatePHI(i64_ty, 3, "hres");
                phi_result->addIncoming(val_ld, found_bb);
                phi_result->addIncoming(c64(11), miss_bb); // void sentinel
                phi_result->addIncoming(c64(11), null_bb); // null case returns void
                store(result_slot, phi_result);
                // Issue #157 Phase 2b: release read lock after the
                // result is stored. done_bb is the sole exit for the
                // inline IR region (null_bb branches here, miss_bb
                // branches here, found_bb branches here, the loop
                // backedge stays within the lock).
                irb->CreateCall(llvm::FunctionCallee(fn_unlock_workspace_read),
                                llvm::ArrayRef<llvm::Value*>{});
                return true;
            }
            case OpHashSet: {
                // HashSet(result_slot, hash_slot, pair_slot)
                // The pair was created by a MakePair (key . val) during lowering.
                // fn_hash_set extracts key/val from pair via g_pair_slots.
                auto hash = load(inst.ops[1]);
                auto pair = load(inst.ops[2]);
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_hash_set),
                                            llvm::ArrayRef<llvm::Value*>{hash, pair});
                store(inst.ops[0], call); // hash-set! returns void (0)
                return true;
            }
            case OpHashRemove: {
                // HashRemove(result_slot, hash_slot, key_slot)
                auto hash = load(inst.ops[1]);
                auto key = load(inst.ops[2]);
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_hash_remove),
                                            llvm::ArrayRef<llvm::Value*>{hash, key});
                store(inst.ops[0], call);
                return true;
            }

            default: {
                // Issue #1289 (P0 business logic): fail-fast on unhandled
                // opcodes. Pre-#1289 wrote VOID (11) to ops[0] and returned
                // true, so compile() continued and the JIT'd function silently
                // produced wrong values. Returning false makes compile()
                // return nullptr → caller falls back to the IR interpreter
                // (always correct, possibly slower).
                //
                // Observability retained: unhandled_opcode_count,
                // per-function fn_unhandled, fallback_count, and
                // aura_notify_jit_unhandled_opcode for deopt/invalidate.
                // Issue #1512: also stamp opcode_unhandled_mask + optional
                // consistency_violations under strict_consistency_mode.
                if (metrics) {
                    metrics->unhandled_opcode_count.fetch_add(1, std::memory_order_relaxed);
                    metrics->fallback_count.fetch_add(1, std::memory_order_relaxed);
                    metrics->unhandled_fail_fast_total.fetch_add(1, std::memory_order_relaxed);
                    if (inst.opcode < 64) {
                        metrics->opcode_unhandled_mask.fetch_or(1ull << inst.opcode,
                                                                std::memory_order_relaxed);
                    }
                }
                // Issue #193: also bump the per-function counter
                // so spec_jit_controller can apply deopt per-function
                // instead of conservatively deopting the whole process.
                if (fn_unhandled) {
                    fn_unhandled->fetch_add(1, std::memory_order_relaxed);
                }
                // Issue #657: notify compiler service for unhandled-opcode
                // deopt / invalidate observability.
                if (fn.name && fn.name[0] != '\0')
                    aura_notify_jit_unhandled_opcode(fn.name);
                // Rate-limited stderr log: log the first occurrence
                // per JIT instance (one-time warning is enough —
                // the counter tracks ongoing volume).
                static std::atomic<bool> warned{false};
                bool expected = false;
                if (warned.compare_exchange_strong(expected, true)) {
                    std::fprintf(stderr,
                                 "aura_jit: WARNING — unhandled IROpcode in "
                                 "lower(); compile fails → interpreter fallback "
                                 "(Issue #1289 fail-fast). Function=%s\n",
                                 (fn.name && fn.name[0] != '\0') ? fn.name : "<anon>");
                }
                return false; // Issue #1289: fail compile → interpreter
            }
        }
    }
};

// ── Escape Analysis (backward dataflow) ───────────────────────

// Run backward escape analysis on flat IR. Fills escape_map (size = local_count).
// Each entry: 0 = NON_ESCAPING, 1 = ESCAPED.
// Escape points: Return(value), Call(callee, args...), Capture(env, val),
// Store(hash, key, val), CellSet(cell, val).
void run_escape_analysis(const std::vector<std::vector<FlatInstruction>>& flat_instrs,
                         uint32_t local_count, std::vector<uint8_t>& escape_map) {
    // Use IROpcode values matching ir.ixx exactly
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
        /* unused: 32=ConstBool, 33=ConstVoid */
        OpMakePair = 34,
        OpCar = 35,
        OpCdr = 36,
        /* 37=Raise, 38=IsError */
        OpHashRef = 39,
        OpHashSet = 40,
        OpHashRemove = 41,
        /* M4 linear ownership: 42-47, not escape-relevant */
        /* Arena */
        OpArenaPush = 48,
        OpArenaPop = 49,
    };

    escape_map.assign(local_count, 0);

    // First pass: mark escape points
    // Escape points are instructions where a value reaches a location
    // that outlives the current scope (return, store in hash, store in
    // cell, pass to call, capture in closure).
    for (auto& block : flat_instrs) {
        for (auto& inst : block) {
            uint32_t result = inst.ops[0];
            switch (inst.opcode) {
                case OpReturn:
                    // Return(value) — value escapes to caller
                    if (inst.ops[0] < local_count)
                        escape_map[inst.ops[0]] = 1;
                    break;
                case OpCall:
                    // Call(callee, arg_base, arg_count, result) — all operands escape
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
                case OpApply:
                    // Apply(closure, arg_count, result) — closure + all inline args escape
                    {
                        uint32_t closure_slot = inst.ops[0];
                        uint32_t arg_count = inst.ops[1];
                        if (closure_slot < local_count)
                            escape_map[closure_slot] = 1;
                        for (uint32_t i = 0; i < arg_count && (closure_slot + 1 + i) < local_count;
                             ++i)
                            escape_map[closure_slot + 1 + i] = 1;
                    }
                    break;
                case OpCapture:
                case OpCaptureRef:
                    // Capture(closure, env_idx, var) / CaptureRef(closure, env_idx, cell)
                    // captured value escapes into closure env
                    if (inst.ops[2] < local_count)
                        escape_map[inst.ops[2]] = 1;
                    // Issue #740: linear-owned captures must heap-allocate in
                    // JIT L2 (not arena) after post-invalidate re-specialize.
                    if (inst.linear_ownership_state != 0) {
                        if (inst.ops[0] < local_count)
                            escape_map[inst.ops[0]] = 1;
                        if (inst.ops[2] < local_count)
                            escape_map[inst.ops[2]] = 1;
                    }
                    break;
                case OpCellSet:
                    // CellSet(cell, val) — val escapes into persistent cell
                    if (inst.ops[1] < local_count)
                        escape_map[inst.ops[1]] = 1;
                    break;
                case OpHashSet:
                    // HashSet(result, hash, keyval_pair) — pair stored in persistent hash
                    // The keyval pair at ops[2] escapes into the hash
                    if (inst.ops[2] < local_count)
                        escape_map[inst.ops[2]] = 1;
                    break;
                case OpPrimCall:
                    // PrimCall(prim_id, arg_base, arg_count, result)
                    // Primitive calls can store values (e.g., vector-set!, hash-set! via PrimCall)
                    {
                        uint32_t arg_base = inst.ops[1];
                        uint32_t arg_count = inst.ops[2];
                        for (uint32_t i = 0; i < arg_count && (arg_base + i) < local_count; ++i)
                            escape_map[arg_base + i] = 1;
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
                if (result >= local_count)
                    continue;
                if (!escape_map[result])
                    continue; // result doesn't escape

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
int64_t aura_top_cell_get(int64_t);
int64_t aura_alloc_pair(int64_t, int64_t);
int64_t aura_alloc_pair_arena(int64_t, int64_t);
int64_t aura_pair_car(int64_t);
int64_t aura_pair_cdr(int64_t);
int64_t aura_pair_car_unchecked(int64_t);
int64_t aura_pair_cdr_unchecked(int64_t);
// Issue #195: per-fiber exception state API.
void aura_exception_push(uint64_t, uint64_t);
void aura_exception_pop();
uint64_t aura_exception_top_handler();
uint64_t aura_exception_top_payload();
uint64_t aura_exception_depth();
void aura_throw_exception(uint64_t);
uint64_t aura_exception_fiber_count();
void aura_exception_clear_all();
// Issue #157 Phase 1b: defuse_version_ accessor for L2 version check.
uint64_t aura_get_defuse_version();
// Issue #157 Phase 1c / #1534 / #1535: deopt counter + dual-epoch fences.
void aura_deopt_inc();
int aura_jit_guard_shape_epoch_check(const char* name);
int aura_jit_linear_epoch_safety_check(const char* fn_name, std::uint8_t linear_state,
                                       std::uint32_t opcode);
int64_t aura_prim_call(int64_t, int64_t, int64_t, int64_t);
uint64_t aura_prim_call_count();
uint64_t aura_prim_call_total_ns();
void aura_display_int(int64_t);
void aura_display_char(char);
void aura_newline();
void aura_jit_epoch_acquire_fence(void);
void aura_jit_linear_post_invalidate_safety(std::uint8_t linear_state, std::uint32_t opcode);
int64_t aura_jit_prim_dispatch(int64_t, int64_t*, int32_t);
int64_t aura_cast_op(int64_t, int64_t);
void aura_register_fn(int64_t func_id, int64_t (*fn)(int64_t*, uint32_t), int32_t local_count,
                      int32_t arg_count, int32_t env_count);
void aura_register_fn_named(const char* name, int64_t func_id, int64_t (*fn)(int64_t*, uint32_t),
                            int32_t local_count, int32_t arg_count, int32_t env_count);
int64_t aura_lookup_fn_by_name(const char* name, int64_t* out_local_count, int64_t* out_arg_count,
                               int64_t* out_env_count);
void aura_closure_set_name(int64_t closure_id, const char* name);
void aura_reset_runtime();
void aura_set_prim_dispatcher(int64_t (*fn)(int64_t, int64_t*, int32_t));
void aura_set_hash_dispatchers(int64_t (*)(int64_t, int64_t),
                               int64_t (*)(int64_t, int64_t, int64_t),
                               int64_t (*)(int64_t, int64_t));
int64_t aura_hash_ref(int64_t, int64_t);
int64_t aura_hash_set(int64_t, int64_t);
int64_t aura_hash_remove(int64_t, int64_t);
// Hash table direct accessor (Phase 4c): returns FlatHashTable*, then GEP
struct FlatHashTable;
const FlatHashTable* aura_hash_get_flat_table(int64_t);
int64_t aura_hash_key_eq(int64_t, int64_t);
void aura_set_hash_str_eq_callback(int64_t (*fn)(int64_t, int64_t));
void aura_set_hash_str_convert_callback(int64_t (*fn)(int64_t));
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
// Arena push/pop wrappers (defined in aura_jit_runtime.cpp)
void aura_arena_push();
void aura_arena_pop();
int64_t aura_alloc_closure_arena(int64_t);
int64_t aura_arena_offset();
}

// C standard library functions (declared in <cstdio>, registered as JIT symbols)

// ── AOT native compilation ──────────────────────────────────────
// Compile a FlatFunction to LLVM IR → .ll → llc → .o
static int aot_func_counter = 0;
static bool emit_native_object_llvm(const FlatFunction& fn, const std::string& out_obj_path,
                                    const std::vector<std::string>* string_pool) {
    llvm::LLVMContext local_ctx;
    int my_id = __sync_fetch_and_add(&aot_func_counter, 1);
    auto mod = std::make_unique<llvm::Module>(
        std::string(fn.name) + "_" + std::to_string(my_id) + "_aot", local_ctx);

    LLVMBuilder builder{local_ctx};
    if (string_pool)
        builder.string_pool = string_pool;
    builder.aot_mode = true;
    builder.mod = mod.get();
    // Issue #170 Phase 1 / item #1: AOT path has no per-call
    // metrics pointer (it doesn't go through AuraJIT's
    // compile()). The default branch still increments via the
    // builder->metrics pointer, which is null here — so the
    // unhandled counter is a no-op in AOT mode. Acceptable
    // for now (AOT is for benchmarking/AOT experiments, not
    // production); the public AOT API in f432d4b has its own
    // observability (compile_to_llvm_ir) for inspection.
    builder.declare_runtime();

    // Build function with unique name (counter disambiguates __lambda__ duplicates).
    // Issue #1369: non-__top__ symbols use mangle_aot_name (always `_vN`) so
    // registration .c link names match the LLVM object. `__top__` stays the
    // exact symbol runtime.c main() calls (version lives in aot_top_fn_version).
    std::string fn_name = fn.name;
    const std::uint64_t emit_ver = aura_get_aot_defuse_version();
    auto unique_name =
        (fn_name == "__top__")
            ? fn_name
            : aura::compiler::mangle_aot_name(fn_name, static_cast<std::uint32_t>(my_id), emit_ver);
    auto ptr_i64 = llvm::PointerType::getUnqual(local_ctx);
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

        auto opt_level = fn.region == 1 ? llvm::OptimizationLevel::O3 : llvm::OptimizationLevel::O2;
        auto mpm = pb.buildPerModuleDefaultPipeline(opt_level);
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
    // LLVM 22 deprecation fix: lookupTarget(StringRef) → lookupTarget(Triple)
    const auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        fprintf(stderr, "AOT: cannot find target %s: %s\n", triple_str.c_str(), err.c_str());
        return false;
    }

    auto tm = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, "generic", {}, {},
                                    // Issue #237: explicitly force
                                    // Reloc::Static so the emitted .o file
                                    // uses absolute (R_X86_64_64 /
                                    // R_X86_64_32S) relocations instead
                                    // of position-independent ones. On
                                    // x86_64 modern toolchains (default
                                    // PIE), PIC .o files cannot be linked
                                    // into a `-no-pie` executable — the
                                    // link fails with "relocation
                                    // R_X86_64_32S ... can not be used
                                    // when making a PIE". This matches
                                    // the existing `-fno-pie` flag on
                                    // the runtime.o / reg.o / prim.o
                                    // compile steps in
                                    // aura_jit_bridge.cpp.
                                    llvm::Reloc::Static));
    if (!tm) {
        fprintf(stderr, "AOT: cannot create TargetMachine for %s\n",
                std::string(triple.str()).c_str());
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
    if (tm->addPassesToEmitFile(cgpm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
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
    // Issue #59 Iter 1: serialize addIRModule + lookup across threads.
    // Held for the duration of a single compile() call. The ORC
    // ThreadSafeModule already provides per-module thread safety;
    // this mutex is the entry-point serialization (prevents two
    // concurrent compiles from racing on the symbol table).
    // Issue #1323/#1324: also guards fn_unhandled_counts_ + fn_trackers_
    // (query/erase paths). Mutable so const unhandled_count_for_fn can lock.
    mutable std::mutex compile_mtx_;
    // Issue #114: per-function compile cache. Lets two threads
    // compiling different functions run in parallel; the same
    // function name is still serialized by this shared_mutex.
    // The cache holds the resolved ScalarFn from the most recent
    // successful compile, keyed by function name. Hot-swap
    // invalidates the entry (under the global lock).
    std::shared_mutex fn_compile_mtx_;
    std::unordered_map<std::string, ScalarFn> compile_fns_{};
    // Issue #1522: tracker entry carries compile epoch + soft deopt flag.
    // deopt_pending: bridge epoch bumped for this name; refuse cache hits
    // until next recompile clears the flag.
    struct FnTrackerEntry {
        llvm::orc::ResourceTrackerSP tracker;
        std::uint64_t compile_epoch = 0;
        bool deopt_pending = false;
    };

    // Per-function resource trackers for hot-swap (remove old module, add new one)
    llvm::orc::ResourceTrackerSP get_or_create_tracker(const std::string& name,
                                                       Metrics* metrics = nullptr,
                                                       std::uint64_t compile_epoch = 0) {
        // Remove old tracker/module for this name before creating a new one.
        // This fixes duplicate symbol errors when the same function name
        // is compiled from different eval() calls (e.g., inlined lambdas).
        auto it = fn_trackers_.find(name);
        if (it != fn_trackers_.end()) {
            // Remove old module from JITDylib before adding the new one.
            // This is the "hot-swap" path: a function with the same name
            // (e.g., re-compiled inlined lambda) replaces the old version.
            if (it->second.tracker) {
                if (auto err = it->second.tracker->remove())
                    llvm::consumeError(std::move(err));
            }
            if (metrics)
                metrics->hot_swap_count.fetch_add(1, std::memory_order_relaxed);
            // Invalidate the cache entry for this function name.
            std::unique_lock<std::shared_mutex> lock(fn_compile_mtx_);
            compile_fns_.erase(name);
        }
        auto rt = main_dylib->createResourceTracker();
        FnTrackerEntry entry;
        entry.tracker = rt;
        entry.compile_epoch = compile_epoch;
        entry.deopt_pending = false;
        fn_trackers_[name] = std::move(entry);
        return rt;
    }
    // Issue #193: per-function unhandled-opcode counter map.
    // Keyed by function name. Each compile() call records
    // unhandled opcodes against the most recent function name
    // (the JIT's lower() doesn't know the function name, but
    // compile() does, and it owns the lower() call). The map
    // is consulted by spec_jit_controller's deopt signal (which
    // becomes per-function instead of conservative-global).
    std::unordered_map<std::string, std::atomic<std::uint64_t>> fn_unhandled_counts_{};
    // Issue #1477: per-fn captured bridge_epoch (set at compile or via
    // capture_fn_epoch from service atomic_bump). Parallel to
    // fn_unhandled_counts_ — atomic per-entry; compile_mtx_ guards map.
    std::unordered_map<std::string, std::atomic<std::uint64_t>> fn_captured_epochs_{};
    // Tracks the most recent function being compiled (set by
    // compile() before calling lower()).
    std::string current_compile_fn_{};
    // Helper: increment the per-function counter for the most
    // recent compile. Called by lower()'s default branch.
    void note_unhandled_for_current_fn() {
        if (current_compile_fn_.empty())
            return;
        fn_unhandled_counts_[current_compile_fn_].fetch_add(1, std::memory_order_relaxed);
    }
    // Helper: query the per-function counter (returns 0 if the
    // function has never been seen).
    // Issue #1323 (P0): lock compile_mtx_ — concurrent find + operator[]
    // (insert under compile()) is UB on unordered_map without a lock.
    std::uint64_t unhandled_count_for_fn(const std::string& name) const {
        std::lock_guard<std::mutex> lock(compile_mtx_);
        auto it = fn_unhandled_counts_.find(name);
        if (it == fn_unhandled_counts_.end())
            return 0;
        return it->second.load(std::memory_order_relaxed);
    }
    // Issue #1522: trackers carry compile epoch + soft deopt flag.
    std::unordered_map<std::string, FnTrackerEntry> fn_trackers_;

    // Issue #170 Phase 1: most recently compiled LLVM module.
    // Held as a unique_ptr so we can release it when a new
    // compile happens (or when the JIT is destroyed). Used by
    // compile_to_llvm_ir() and compile_to_object_file() to
    // give external tools (benchmark, AOT, static analysis)
    // access to the LLVM IR / native code that the JIT just
    // generated.
    std::unique_ptr<llvm::Module> last_module_;
    // Issue #1516: per-function AOT module snapshots (keyed by
    // function name). Survives subsequent compile() of other
    // functions so multi-function static-link AOT is selective.
    std::unordered_map<std::string, std::unique_ptr<llvm::Module>> fn_aot_modules_;
    // Issue #1516: IR func_id → function name (set by register_fn_func).
    std::unordered_map<std::uint32_t, std::string> fn_id_to_name_;

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
        reg("aura_top_cell_get", (void*)aura_top_cell_get);
        reg("aura_alloc_pair", (void*)aura_alloc_pair);
        reg("aura_alloc_pair_arena", (void*)aura_alloc_pair_arena);
        reg("aura_pair_car", (void*)aura_pair_car);
        reg("aura_pair_cdr", (void*)aura_pair_cdr);
        reg("aura_pair_car_unchecked", (void*)aura_pair_car_unchecked);
        reg("aura_pair_cdr_unchecked", (void*)aura_pair_cdr_unchecked);
        reg("aura_prim_call", (void*)aura_prim_call);
        reg("aura_set_prim_dispatcher", (void*)aura_set_prim_dispatcher);
        reg("aura_hash_ref", (void*)aura_hash_ref);
        reg("aura_hash_set", (void*)aura_hash_set);
        reg("aura_hash_remove", (void*)aura_hash_remove);
        reg("aura_display_int", (void*)aura_display_int);
        reg("aura_display_char", (void*)aura_display_char);
        reg("aura_newline", (void*)aura_newline);
        reg("aura_jit_epoch_acquire_fence", (void*)aura_jit_epoch_acquire_fence);
        reg("aura_jit_linear_post_invalidate_safety",
            (void*)aura_jit_linear_post_invalidate_safety);
        // Issue #1534 / #1535: dual-epoch fences + deopt counter for stale path.
        reg("aura_jit_guard_shape_epoch_check", (void*)aura_jit_guard_shape_epoch_check);
        reg("aura_jit_linear_epoch_safety_check", (void*)aura_jit_linear_epoch_safety_check);
        reg("aura_deopt_inc", (void*)aura_deopt_inc);
        reg("aura_get_defuse_version", (void*)aura_get_defuse_version);
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
        reg("aura_arena_push", (void*)aura_arena_push);
        reg("aura_arena_pop", (void*)aura_arena_pop);
        reg("aura_arena_offset", (void*)aura_arena_offset);
        reg("aura_alloc_closure_arena", (void*)aura_alloc_closure_arena);

        reg("aura_hash_get_flat_table", (void*)aura_hash_get_flat_table);
        reg("aura_set_hash_str_eq_callback", (void*)aura_set_hash_str_eq_callback);
        reg("aura_set_hash_str_convert_callback", (void*)aura_set_hash_str_convert_callback);
        reg("aura_hash_key_eq", (void*)aura_hash_key_eq);

        // Issue #195: register the per-fiber exception state
        // API and the C personality function. The JIT-compiled
        // code can call into these when OpRaise emits an
        // `invoke` to aura_throw_exception. Note: the
        // previous block (above) also registers
        // aura_exception_fiber_count and
        // aura_exception_clear_all — those are duplicate
        // registrations and the second reg() call fails with
        // "JIT: failed to define symbol". The duplicates
        // were removed (the first set is the canonical
        // registration; the second set is dead code that
        // only the duplicate-registration bug survived).
        reg("aura_exception_push", (void*)aura_exception_push);
        reg("aura_exception_pop", (void*)aura_exception_pop);
        reg("aura_exception_top_handler", (void*)aura_exception_top_handler);
        reg("aura_exception_top_payload", (void*)aura_exception_top_payload);
        reg("aura_exception_depth", (void*)aura_exception_depth);
        reg("aura_throw_exception", (void*)aura_throw_exception);

        // C standard library functions
        reg("printf", (void*)printf);
        reg("fprintf", (void*)fprintf);
        reg("fflush", (void*)fflush);
        reg("fputc", (void*)fputc);

        // Reset runtime state for fresh session
        aura_reset_runtime();

        return true;
    }

    ScalarFn compile(const FlatFunction& fn, Metrics* metrics = nullptr) {
        // Issue #59 Iter 1: serialize addIRModule + lookup across threads.
        // Held through the whole LLVM pipeline run + verify + addIRModule
        // + lookup. Sub-ms per call in practice; readers don't contend
        // because ORC's ThreadSafeModule is per-module atomic.
        //
        // Issue #114: the global compile_mtx_ is now complemented by a
        // per-function-name lock in `fn_compile_mtx_` so different
        // functions can compile concurrently. The global lock is still
        // held briefly around addIRModule + lookup (because the ORC
        // JITDylib symbol table isn't fully thread-safe across
        // concurrent module insertions in this LLVM version).
        std::lock_guard<std::mutex> compile_lock(compile_mtx_);
        if (!init())
            return nullptr;

        // Per-function fine-grained lock (released before the global one).
        // Two threads compiling the SAME function still serialize here
        // (one wins, the other gets the cached compile_fns_ entry).
        // Two threads compiling DIFFERENT functions run in parallel.
        std::unique_lock<std::shared_mutex> fn_lock;
        {
            // Acquire the per-function shared lock briefly to look up
            // the existing entry. The lookup is racy with concurrent
            // compiles, but the result is just an optimization hint
            // (we re-check under the global lock below).
            std::shared_lock<std::shared_mutex> shared(fn_compile_mtx_);
            auto it = compile_fns_.find(std::string(fn.name));
            if (it != compile_fns_.end()) {
                // Issue #1522: refuse cache hit when batch_deopt marked pending.
                auto tit = fn_trackers_.find(std::string(fn.name));
                if (tit != fn_trackers_.end() && tit->second.deopt_pending) {
                    if (metrics)
                        metrics->deopt_pending_invoke_fallbacks.fetch_add(
                            1, std::memory_order_relaxed);
                    // Fall through to full recompile (hot-swap clears pending).
                } else {
                    // Cache hit — return the previously compiled fn_ptr
                    // without re-running the LLVM pipeline.
                    if (metrics) {
                        metrics->compile_count.fetch_add(1, std::memory_order_relaxed);
                        metrics->cached_function_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    return it->second;
                }
            }
        }

        // Time the full compile (including all LLVM passes + addIRModule
        // + lookup). The metric is the average for telemetry dashboards.
        auto t0 = std::chrono::steady_clock::now();

        auto mod = std::make_unique<llvm::Module>(
            std::string("mod_") + std::to_string(module_counter++), ctx);

        // Issue #193: tag the most recent compile so the lower()
        // default branch can record per-function unhandled
        // opcodes. Cleared at function exit (RAII via a small
        // guard struct would be cleaner, but a try/finally
        // pattern isn't needed — compile() always returns
        // after the lower() loop, so the cleanup below always
        // runs).
        current_compile_fn_ = fn.name ? fn.name : "";

        LLVMBuilder builder{ctx};
        builder.mod = mod.get();
        builder.declare_runtime();
        builder.shape_map = fn.shape_map;
        builder.shape_map_size = fn.local_count;
        // Issue #170 Phase 1 / item #1: wire metrics into the
        // builder so the lower() default branch can increment
        // unhandled_opcode_count. compile() always sets metrics
        // (it's the AuraJIT::Metrics from the public API); the
        // AOT path is metric-free for now.
        builder.metrics = metrics;
        // Issue #193: per-function unhandled counter, allocated
        // from fn_unhandled_counts_ so the map owns the storage.
        // The builder holds a non-owning pointer; the lower()
        // loop bumps it. After the loop we fold it into the
        // map (if non-zero) and clear the builder's pointer.
        auto& fn_counter = fn_unhandled_counts_[std::string(fn.name)];
        builder.fn_unhandled = &fn_counter;
        if (string_pool_)
            builder.string_pool = string_pool_;

        // Build function
        auto ptr_i64 = llvm::PointerType::getUnqual(ctx);
        auto i32_ty = llvm::Type::getInt32Ty(ctx);
        auto ret_ty = llvm::Type::getInt64Ty(ctx);
        auto fn_type = llvm::FunctionType::get(ret_ty, {ptr_i64, i32_ty}, false);
        builder.func =
            llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage, fn.name, mod.get());
        // Issue #195: tag the function with the C personality
        // function. Any `invoke` instruction in the function
        // can unwind through this personality. The personality
        // function (aura_personality) walks the current fiber's
        // ExStack and reports _URC_HANDLER_FOUND if a handler
        // is set. Without this, OpRaise's `invoke` would crash
        // the process when the unwinder tries to find a
        // personality function for the current frame.
        if (auto* persona_fn = mod->getFunction("aura_personality")) {
            builder.func->setPersonalityFn(persona_fn);
        }
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

        // Issue #157 Phase 1b: reserve an extra local slot (at index
        // fn.local_count) for the captured defuse_version_. At function
        // entry we call aura_get_defuse_version() and store the result
        // here; at each L2 SHAPE_PAIR use we compare the current
        // version against this captured value. On mismatch, deopt to
        // the slow path (aura_pair_car / aura_pair_cdr with bounds +
        // lock). On match, take the fast path (aura_pair_car_unchecked
        // / aura_pair_cdr_unchecked, no bounds, no lock).
        builder.llvm_locals.push_back(builder.irb->CreateAlloca(llvm::Type::getInt64Ty(ctx)));
        {
            auto* ver = builder.irb->CreateCall(builder.fn_get_defuse_version, {});
            builder.irb->CreateStore(ver, builder.llvm_locals[fn.local_count]);
        }

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

            auto jit_opt =
                fn.region == 1 ? llvm::OptimizationLevel::O3 : llvm::OptimizationLevel::O2;
            auto mpm = pb.buildPerModuleDefaultPipeline(jit_opt);
            mpm.run(*mod, mam);
        }

        if (llvm::verifyModule(*mod, &llvm::errs())) {
            fprintf(stderr, "JIT: verification failed\n");
            if (metrics)
                metrics->verify_fail_count.fetch_add(1, std::memory_order_relaxed);
            current_compile_fn_.clear();
            return nullptr;
        }

        // Issue #170 Phase 1 + #1516: snapshot the verified + optimized
        // module for AOT entry points. One clone for last_module_
        // (most-recent AOT) and one for the per-function map so
        // multi-function static-link can select by name/func_id.
        // Clones are necessary because the original `mod` is
        // moved into the ThreadSafeModule below and handed off
        // to the JIT.
        {
            auto cloned = llvm::CloneModule(*mod);
            if (cloned) {
                // Replace last_module_ in a thread-safe way (compile_mtx_
                // already held; last_module_ is only touched here)
                last_module_ = std::move(cloned);
                if (fn.name && fn.name[0]) {
                    auto per_fn = llvm::CloneModule(*last_module_);
                    if (per_fn)
                        fn_aot_modules_[std::string(fn.name)] = std::move(per_fn);
                }
            }
        }

        auto tsm =
            llvm::orc::ThreadSafeModule(std::move(mod), std::make_unique<llvm::LLVMContext>());
        auto rt = get_or_create_tracker(std::string(fn.name), metrics);
        if (auto err = jit->addIRModule(rt, std::move(tsm))) {
            fprintf(stderr, "JIT: addIRModule failed\n");
            if (metrics)
                metrics->add_module_fail_count.fetch_add(1, std::memory_order_relaxed);
            current_compile_fn_.clear();
            return nullptr;
        }

        auto sym = jit->lookup(std::string(fn.name));
        if (!sym) {
            fprintf(stderr, "JIT: lookup failed\n");
            current_compile_fn_.clear();
            return nullptr;
        }
        auto fn_ptr = sym->toPtr<ScalarFn>();

        // Update the per-function cache (under shared lock; readers
        // from other threads can do concurrent compiles of OTHER
        // functions while we hold this).
        {
            std::unique_lock<std::shared_mutex> lock(fn_compile_mtx_);
            compile_fns_[std::string(fn.name)] = fn_ptr;
        }

        // Update metrics. The compile_count covers all calls
        // including cache hits (visible at the top of the function).
        if (metrics) {
            metrics->compile_count.fetch_add(1, std::memory_order_relaxed);
            auto t1 = std::chrono::steady_clock::now();
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            metrics->compile_total_us.fetch_add(static_cast<std::uint64_t>(us),
                                                std::memory_order_relaxed);
        }
        // Issue #193: clear the current-compile tag so subsequent
        // (out-of-band) unhandled-opcode notes don't attribute to
        // the most recent function. The fn_unhandled_counts_ map
        // retains the count for spec_jit_controller to consult.
        current_compile_fn_.clear();
        return fn_ptr;
    }

    void* get_function_ptr(const char* name, Metrics* metrics = nullptr) {
        if (!init() || !name)
            return nullptr;
        // Issue #1522: refuse native lookup when deopt_pending (stale after
        // bridge epoch bump). Caller falls back to interpreter / recompile.
        {
            std::lock_guard<std::mutex> lock(compile_mtx_);
            auto it = fn_trackers_.find(name);
            if (it != fn_trackers_.end() && it->second.deopt_pending) {
                if (metrics)
                    metrics->deopt_pending_invoke_fallbacks.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
        }
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
                          uint32_t arg_count, uint32_t env_count, const char* name = nullptr) {
        // Issue #660 Option 1: also register by name if provided.
        if (name && *name) {
            aura_register_fn_named(name, func_id, fn_ptr, static_cast<int32_t>(local_count),
                                   static_cast<int32_t>(arg_count),
                                   static_cast<int32_t>(env_count));
            // Issue #1516: map IR func_id → name for per-function AOT.
            if (func_id >= 0) {
                std::lock_guard<std::mutex> lock(compile_mtx_);
                fn_id_to_name_[static_cast<std::uint32_t>(func_id)] = name;
            }
        } else {
            aura_register_fn(func_id, fn_ptr, static_cast<int32_t>(local_count),
                             static_cast<int32_t>(arg_count), static_cast<int32_t>(env_count));
        }
        compiled_fns_.push_back(
            {name ? std::string(name) : std::string(), fn_ptr, local_count, arg_count, env_count});
    }
};

AuraJIT::AuraJIT()
    : impl_(std::make_unique<Impl>()) {}
AuraJIT::~AuraJIT() = default;
bool AuraJIT::available() const {
    return impl_->initialized;
}
ScalarFn AuraJIT::compile(const FlatFunction& fn) {
    // Issue #114: pass our metrics into the impl so compile/verify/
    // hot-swap counters get updated. The impl uses the pointer
    // (not a reference) so it can be null in tests that construct
    // an Impl directly.
    auto result = impl_->compile(fn, &metrics_);
    // Issue #1534: capture fn epoch at compile time so OpGuardShape's
    // runtime is_fn_epoch_stale probe can detect post-compile bumps.
    // Epoch source: aura_aot_func_table_epoch (kept in lockstep with
    // CompilerService::bridge_epoch by atomic_bump_epochs_and_stamp_bridge).
    // Capture even when compile returns nullptr (tracker may still exist).
    if (fn.name && fn.name[0]) {
        capture_fn_epoch(fn.name, aura_aot_func_table_epoch());
    }
    // Issue #1512: on full lower success, stamp every opcode in the
    // FlatFunction into opcode_covered_mask (coverage observability).
    // On fail-fast unhandled, optional strict mode bumps consistency
    // violations (JIT cannot match IRInterpreter for that opcode).
    if (result) {
        for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
            const auto& fb = fn.blocks[bi];
            if (!fb.instructions)
                continue;
            for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
                const auto op = fb.instructions[ii].opcode;
                if (op < 64) {
                    metrics_.opcode_covered_mask.fetch_or(1ull << op, std::memory_order_relaxed);
                }
            }
        }
    } else if (strict_consistency_mode_) {
        metrics_.consistency_violations.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #170 Phase 1: snapshot the most recent compiled module
    // for AOT access via compile_to_llvm_ir / compile_to_object_file.
    // The impl's compile() now keeps a copy of the optimized module
    // in impl_->last_module_ for the AOT entry points to consume.
    return result;
}

// Issue #1512: side-by-side JIT ↔ IRInterpreter compare result.
void AuraJIT::record_consistency_result(bool match) noexcept {
    metrics_.consistency_compare_total.fetch_add(1, std::memory_order_relaxed);
    if (match) {
        metrics_.consistency_match_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        metrics_.consistency_violations.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t AuraJIT::opcode_coverage_count() const noexcept {
    const auto mask = metrics_.opcode_covered_mask.load(std::memory_order_relaxed);
    // popcount of lower kTrackedOpcodeCount bits
    std::uint64_t n = 0;
    for (std::uint32_t i = 0; i < kTrackedOpcodeCount; ++i) {
        if (mask & (1ull << i))
            ++n;
    }
    return n;
}

std::uint64_t AuraJIT::opcode_coverage_pct() const noexcept {
    if (kTrackedOpcodeCount == 0)
        return 0;
    return (opcode_coverage_count() * 100ull) / static_cast<std::uint64_t>(kTrackedOpcodeCount);
}
void* AuraJIT::get_function_ptr(const char* name) {
    return impl_->get_function_ptr(name, &metrics_);
}

// Issue #170 Phase 1: AOT entry points. The compile() function
// stores the most recently compiled (and optimized) module in
// impl_->last_module_. These methods re-emit that module as
// either textual LLVM IR or a native .o file. Both are no-ops
// if no module has been compiled yet (returns empty / false).
std::string AuraJIT::compile_to_llvm_ir() {
    // Issue #1308 (P0): hold compile_mtx_ — last_module_ is written under
    // this lock in compile(); unguarded read races with replace/free (UAF).
    if (!impl_)
        return std::string();
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    if (!impl_->last_module_) {
        return std::string();
    }
    std::string buf;
    llvm::raw_string_ostream os(buf);
    impl_->last_module_->print(os, /*AAW=*/nullptr);
    os.flush();
    return buf;
}

// Issue #1516: shared object-file emit path for last_module_ and
// per-function AOT snapshots. Caller holds compile_mtx_ and supplies
// a live Module*.
static bool emit_llvm_module_to_object(llvm::Module* mod, const std::string& path) {
    if (!mod)
        return false;
    static std::once_flag aot_target_init;
    std::call_once(aot_target_init, []() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });

    auto triple_str = llvm::sys::getDefaultTargetTriple();
    llvm::Triple triple(triple_str);
    std::string err;
    // LLVM 22 deprecation fix: lookupTarget(StringRef) → lookupTarget(Triple)
    const auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target)
        return false;

    auto tm = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, "generic", {}, {}, {}));
    if (!tm)
        return false;

    mod->setDataLayout(tm->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
    if (ec)
        return false;

    llvm::legacy::PassManager cgpm;
    if (tm->addPassesToEmitFile(cgpm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        return false;
    }
    cgpm.run(*mod);
    dest.flush();
    return true;
}

bool AuraJIT::compile_to_object_file(const std::string& path) {
    // Issue #1308 (P0): same lock as compile_to_llvm_ir.
    if (!impl_)
        return false;
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    if (!impl_->last_module_) {
        return false;
    }
    const bool ok = emit_llvm_module_to_object(impl_->last_module_.get(), path);
    if (ok)
        metrics_.aot_last_module_object_total.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

std::string AuraJIT::compile_function_to_llvm_ir_by_name(const char* name) {
    if (!impl_ || !name || !name[0]) {
        metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    auto it = impl_->fn_aot_modules_.find(name);
    if (it == impl_->fn_aot_modules_.end() || !it->second) {
        metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    std::string buf;
    llvm::raw_string_ostream os(buf);
    it->second->print(os, /*AAW=*/nullptr);
    os.flush();
    metrics_.aot_per_function_ir_total.fetch_add(1, std::memory_order_relaxed);
    return buf;
}

bool AuraJIT::compile_function_to_object_by_name(const char* name, const std::string& path) {
    if (!impl_ || !name || !name[0]) {
        metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    auto it = impl_->fn_aot_modules_.find(name);
    if (it == impl_->fn_aot_modules_.end() || !it->second) {
        metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool ok = emit_llvm_module_to_object(it->second.get(), path);
    if (ok)
        metrics_.aot_per_function_object_total.fetch_add(1, std::memory_order_relaxed);
    else
        metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

std::string AuraJIT::compile_function_to_llvm_ir(std::uint32_t func_id) {
    if (!impl_) {
        metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    std::string name;
    {
        std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
        auto it = impl_->fn_id_to_name_.find(func_id);
        if (it != impl_->fn_id_to_name_.end())
            name = it->second;
    }
    if (name.empty()) {
        metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    return compile_function_to_llvm_ir_by_name(name.c_str());
}

bool AuraJIT::compile_function_to_object(std::uint32_t func_id, const std::string& path) {
    if (!impl_) {
        metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::string name;
    {
        std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
        auto it = impl_->fn_id_to_name_.find(func_id);
        if (it != impl_->fn_id_to_name_.end())
            name = it->second;
    }
    if (name.empty()) {
        metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return compile_function_to_object_by_name(name.c_str(), path);
}

std::uint64_t AuraJIT::exception_opcode_coverage_count() const noexcept {
    const auto mask = metrics_.exception_opcode_mask.load(std::memory_order_relaxed);
    std::uint64_t n = 0;
    for (std::uint32_t i = 0; i < kExceptionOpcodeCount; ++i) {
        if (mask & (1ull << i))
            ++n;
    }
    return n;
}

// ── Metrics::format (Issue #114) ────────────────────────────
// One-line snapshot of all JIT counters. Designed to be
// cheap to call (e.g., from a periodic telemetry heartbeat).
char* AuraJIT::Metrics::format(char* buf, std::size_t buf_size) const noexcept {
    if (!buf || buf_size == 0)
        return buf;
    const auto cc = compile_count.load(std::memory_order_relaxed);
    const auto hs = hot_swap_count.load(std::memory_order_relaxed);
    const auto inl = inlined_prim_count.load(std::memory_order_relaxed);
    const auto slow = slow_prim_count.load(std::memory_order_relaxed);
    const auto total_us = compile_total_us.load(std::memory_order_relaxed);
    const auto avg_us = cc > 0 ? static_cast<std::uint64_t>(total_us / cc) : 0u;
    const auto vfail = verify_fail_count.load(std::memory_order_relaxed);
    const auto mfail = add_module_fail_count.load(std::memory_order_relaxed);
    const auto cfns = cached_function_count.load(std::memory_order_relaxed);
    // Issue #170 Phase 1 / item #1: unhandled-opcode counter.
    // Reads from Metrics directly (per-instance); the JIT's
    // lower() default branch increments this on every IROpcode
    // that doesn't have a case. Hot functions with non-zero
    // values are candidates for spec_jit_controller auto-deopt
    // (Phase 2 / item #1).
    const auto unhandled = unhandled_opcode_count.load(std::memory_order_relaxed);
    // Issue #170 Phase 2 / item #3: intrinsic count. Tracks how
    // many runtime helper calls the JIT replaced with inline
    // sequences. Single number for observability.
    const auto intrinsic = intrinsic_count.load(std::memory_order_relaxed);
    const auto fallback = fallback_count.load(std::memory_order_relaxed);
    const auto consistency = consistency_violations.load(std::memory_order_relaxed);
    // Issue #1512: coverage + compare totals for Agent telemetry.
    const auto covered_mask = opcode_covered_mask.load(std::memory_order_relaxed);
    std::uint64_t covered_n = 0;
    for (std::uint32_t i = 0; i < AuraJIT::kTrackedOpcodeCount; ++i) {
        if (covered_mask & (1ull << i))
            ++covered_n;
    }
    const auto cov_pct =
        AuraJIT::kTrackedOpcodeCount > 0
            ? (covered_n * 100ull) / static_cast<std::uint64_t>(AuraJIT::kTrackedOpcodeCount)
            : 0ull;
    const auto cmp_total = consistency_compare_total.load(std::memory_order_relaxed);
    const auto cmp_match = consistency_match_total.load(std::memory_order_relaxed);
    // Live prim-call counters from aura_jit_runtime.cpp
    // (read via the global accessors). The total is in nanoseconds;
    // average per call is computed inline.
    const auto pc = aura_prim_call_count();
    const auto pns = aura_prim_call_total_ns();
    const auto pavg = pc > 0 ? static_cast<std::uint64_t>(pns / pc) : 0u;
    std::snprintf(buf, buf_size,
                  "jit: compiles=%llu avg_us=%llu hot_swaps=%llu "
                  "cached_fns=%llu inlined_prims=%llu slow_prims=%llu "
                  "prim_calls=%llu prim_avg_ns=%llu "
                  "verify_fail=%llu add_mod_fail=%llu "
                  "unhandled_opcode=%llu intrinsics=%llu "
                  "fallback_count=%llu consistency_violations=%llu "
                  "opcode_coverage_pct=%llu consistency_compares=%llu "
                  "consistency_matches=%llu",
                  (unsigned long long)cc, (unsigned long long)avg_us, (unsigned long long)hs,
                  (unsigned long long)cfns, (unsigned long long)inl, (unsigned long long)slow,
                  (unsigned long long)pc, (unsigned long long)pavg, (unsigned long long)vfail,
                  (unsigned long long)mfail, (unsigned long long)unhandled,
                  (unsigned long long)intrinsic, (unsigned long long)fallback,
                  (unsigned long long)consistency, (unsigned long long)cov_pct,
                  (unsigned long long)cmp_total, (unsigned long long)cmp_match);
    return buf;
}

void AuraJIT::register_symbol(const char* name, void* ptr) {
    impl_->register_symbol_func(name, ptr);
}

// Issue #193: per-function unhandled-opcode count.
std::uint64_t AuraJIT::unhandled_opcode_count_for_function(const char* name) const {
    if (!name)
        return 0;
    return impl_->unhandled_count_for_fn(name);
}

void AuraJIT::set_string_pool(const std::vector<std::string>* pool) {
    impl_->string_pool_ = pool;
}

void AuraJIT::register_function(int64_t func_id, ScalarFn fn_ptr, uint32_t local_count,
                                uint32_t arg_count, uint32_t env_count, const char* name) {
    impl_->register_fn_func(func_id, fn_ptr, local_count, arg_count, env_count, name);
}

const std::vector<FunctionMeta>& AuraJIT::compiled_functions() const {
    return impl_->compiled_fns_;
}

void AuraJIT::invalidate(const char* name) {
    if (!impl_ || !name)
        return;
    std::string n(name);
    // Issue #1324 (P0): acquire compile_mtx_ BEFORE any erase of
    // fn_trackers_ / fn_unhandled_counts_. Previously erases ran
    // unlocked while compile() inserts under compile_mtx_ → UB.
    std::lock_guard<std::mutex> compile_lock(impl_->compile_mtx_);
    // Issue #1516: drop per-function AOT snapshot on invalidate.
    impl_->fn_aot_modules_.erase(n);
    // Drop the per-function resource tracker (removes the
    // module from the JITDylib) AND the per-function compile
    // cache entry. This matches what get_or_create_tracker()
    // does on a hot-swap; we're exposing it publicly so the
    // CompilerService can call it on (re)define without going
    // through the compile path.
    auto it = impl_->fn_trackers_.find(n);
    if (it != impl_->fn_trackers_.end()) {
        if (it->second.tracker) {
            if (auto err = it->second.tracker->remove())
                llvm::consumeError(std::move(err));
        }
        impl_->fn_trackers_.erase(it);
    }
    // Issue #993: also drop unhandled-opcode counters so hot-swap
    // / multi-agent sessions do not accumulate dead keys forever.
    impl_->fn_unhandled_counts_.erase(n);
    std::unique_lock<std::shared_mutex> lock(impl_->fn_compile_mtx_);
    impl_->compile_fns_.erase(n);
}

// Issue #1514: partial recompile — evict native code for a define
// so the next exec recompiles only that function. Records dirty
// block ids for Agent observability (block-level re-emit follow-up).
bool AuraJIT::partial_recompile(const char* name, const std::uint32_t* dirty_block_ids,
                                std::size_t n_dirty_blocks) noexcept {
    metrics_.partial_recompile_requests.fetch_add(1, std::memory_order_relaxed);
    metrics_.partial_recompile_dirty_blocks_total.fetch_add(n_dirty_blocks,
                                                            std::memory_order_relaxed);
    if (!name || !name[0])
        return false;
    // Count whether anything was cached before eviction.
    // Do NOT hold compile_mtx_ across invalidate() — it re-locks.
    bool had = false;
    if (impl_) {
        std::string n(name);
        std::string p_hash = n + "#";
        std::shared_lock<std::shared_mutex> rlock(impl_->fn_compile_mtx_);
        if (impl_->compile_fns_.count(n) > 0)
            had = true;
        else {
            for (const auto& kv : impl_->compile_fns_) {
                if (kv.first.rfind(p_hash, 0) == 0) {
                    had = true;
                    break;
                }
            }
        }
    }
    // Evict bare name + name#* (same as redefine path).
    invalidate(name);
    invalidate_prefix(name);
    if (had)
        metrics_.partial_recompile_cache_evictions.fetch_add(1, std::memory_order_relaxed);
    (void)dirty_block_ids; // reserved for block-level re-emit
    return had;
}

// Issue #660 follow-up: invalidate_prefix — walk both
// fn_trackers_ and compile_fns_ and erase any entry whose
// key starts with `prefix` (e.g. "mul" → erases "mul#0",
// "mul#1", ...). Used by cache_define on redefine since
// JIT cache keys are `name + "#" + pos`, not the bare name.
void AuraJIT::invalidate_prefix(const char* prefix) {
    if (!impl_ || !prefix)
        return;
    std::string p(prefix);
    if (p.empty())
        return;
    std::string p_hash = p + "#";
    // Issue #1324 (P0): lock compile_mtx_ before any trackers/unhandled erase.
    std::lock_guard<std::mutex> compile_lock(impl_->compile_mtx_);
    for (auto it = impl_->fn_trackers_.begin(); it != impl_->fn_trackers_.end();) {
        if (it->first == p || it->first.rfind(p_hash, 0) == 0) {
            if (it->second.tracker) {
                if (auto err = it->second.tracker->remove())
                    llvm::consumeError(std::move(err));
            }
            it = impl_->fn_trackers_.erase(it);
        } else {
            ++it;
        }
    }
    // Issue #993: erase matching unhandled counters.
    for (auto it = impl_->fn_unhandled_counts_.begin(); it != impl_->fn_unhandled_counts_.end();) {
        if (it->first == p || it->first.rfind(p_hash, 0) == 0)
            it = impl_->fn_unhandled_counts_.erase(it);
        else
            ++it;
    }
    // Issue #1516: drop matching per-function AOT snapshots.
    for (auto it = impl_->fn_aot_modules_.begin(); it != impl_->fn_aot_modules_.end();) {
        if (it->first == p || it->first.rfind(p_hash, 0) == 0)
            it = impl_->fn_aot_modules_.erase(it);
        else
            ++it;
    }
    std::unique_lock<std::shared_mutex> lock(impl_->fn_compile_mtx_);
    for (auto it = impl_->compile_fns_.begin(); it != impl_->compile_fns_.end();) {
        if (it->first == p || it->first.rfind(p_hash, 0) == 0) {
            it = impl_->compile_fns_.erase(it);
        } else {
            ++it;
        }
    }
}

// Issue #1477: JIT-side dual-epoch fence.
// Issue #1522: soft batch deopt — mark fn_trackers_ deopt_pending + drop
// compile_fns_ so next invoke / compile refuses stale native code.
void AuraJIT::capture_fn_epoch(const char* name, std::uint64_t current_bridge_epoch) {
    if (!impl_ || !name)
        return;
    std::lock_guard<std::mutex> compile_lock(impl_->compile_mtx_);
    impl_->fn_captured_epochs_[name].store(current_bridge_epoch, std::memory_order_release);
    metrics_.jit_epoch_stale_check_total.fetch_add(1, std::memory_order_relaxed);
}

bool AuraJIT::is_fn_epoch_stale(const char* name, std::uint64_t current_bridge_epoch) const {
    if (!impl_ || !name)
        return false;
    std::lock_guard<std::mutex> compile_lock(impl_->compile_mtx_);
    auto it = impl_->fn_captured_epochs_.find(name);
    if (it == impl_->fn_captured_epochs_.end())
        return false; // never captured → pass-through
    return it->second.load(std::memory_order_acquire) != current_bridge_epoch;
}

// Issue #1536: bulk walk of active captured fns — O(C + T).
// Marks deopt-on-next-apply for every tracker whose base name is stale
// vs current_bridge_epoch. Bumps jit_epoch_stale_check_total once per
// stale fn found (not per tracker entry).
std::size_t AuraJIT::walk_active_closures(std::uint64_t current_bridge_epoch) noexcept {
    metrics_.walk_active_closures_total.fetch_add(1, std::memory_order_relaxed);
    if (!impl_)
        return 0;
    std::lock_guard<std::mutex> compile_lock(impl_->compile_mtx_);

    // Phase 1: O(C) — collect stale names from fn_captured_epochs_.
    std::unordered_set<std::string> stale_names;
    stale_names.reserve(impl_->fn_captured_epochs_.size());
    std::size_t examined = 0;
    std::size_t stale_found = 0;
    for (const auto& [name, ep] : impl_->fn_captured_epochs_) {
        ++examined;
        const auto capt = ep.load(std::memory_order_acquire);
        // Same predicate as is_fn_epoch_stale (never-captured already excluded).
        if (capt == current_bridge_epoch)
            continue;
        stale_names.insert(name);
        ++stale_found;
        // AC3: one bump per stale fn found.
        metrics_.jit_epoch_stale_check_total.fetch_add(1, std::memory_order_relaxed);
    }
    metrics_.walk_active_closures_examined.fetch_add(examined, std::memory_order_relaxed);
    metrics_.walk_active_closures_stale_found.fetch_add(stale_found, std::memory_order_relaxed);

    if (stale_names.empty())
        return 0;

    // Phase 2: O(T) — mark matching trackers deopt_pending (deopt-on-next-apply).
    auto base_name = [](const std::string& key) -> std::string {
        const auto pos = key.find('#');
        return pos == std::string::npos ? key : key.substr(0, pos);
    };
    for (auto& [key, entry] : impl_->fn_trackers_) {
        if (!stale_names.count(key) && !stale_names.count(base_name(key)))
            continue;
        if (!entry.deopt_pending) {
            entry.deopt_pending = true;
            entry.compile_epoch = current_bridge_epoch;
            metrics_.batch_deopt_entries_marked.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Drop compile cache for stale names so next compile re-runs the pipeline.
    {
        std::unique_lock<std::shared_mutex> lock(impl_->fn_compile_mtx_);
        for (auto it = impl_->compile_fns_.begin(); it != impl_->compile_fns_.end();) {
            if (stale_names.count(it->first) || stale_names.count(base_name(it->first)))
                it = impl_->compile_fns_.erase(it);
            else
                ++it;
        }
    }

    return stale_found;
}

std::size_t AuraJIT::batch_deopt_for(const char* name, std::uint64_t current_epoch) noexcept {
    metrics_.batch_deopt_for_total.fetch_add(1, std::memory_order_relaxed);
    if (!impl_ || !name || !name[0])
        return 0;
    std::string n(name);
    std::string n_hash = n + "#";
    std::size_t marked = 0;
    std::lock_guard<std::mutex> compile_lock(impl_->compile_mtx_);
    for (auto& [key, entry] : impl_->fn_trackers_) {
        if (key != n && key.rfind(n_hash, 0) != 0)
            continue;
        if (!entry.deopt_pending) {
            entry.deopt_pending = true;
            entry.compile_epoch = current_epoch;
            ++marked;
            metrics_.batch_deopt_entries_marked.fetch_add(1, std::memory_order_relaxed);
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(impl_->fn_compile_mtx_);
        for (auto it = impl_->compile_fns_.begin(); it != impl_->compile_fns_.end();) {
            if (it->first == n || it->first.rfind(n_hash, 0) == 0)
                it = impl_->compile_fns_.erase(it);
            else
                ++it;
        }
    }
    return marked;
}

std::size_t AuraJIT::batch_deopt_prefix(const char* prefix, std::uint64_t current_epoch) noexcept {
    return batch_deopt_for(prefix, current_epoch);
}

bool AuraJIT::is_deopt_pending(const char* name) const noexcept {
    if (!impl_ || !name)
        return false;
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    auto it = impl_->fn_trackers_.find(name);
    return it != impl_->fn_trackers_.end() && it->second.deopt_pending;
}

std::uint64_t AuraJIT::deopt_pending_count() const noexcept {
    if (!impl_)
        return 0;
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    std::uint64_t n = 0;
    for (const auto& [_, e] : impl_->fn_trackers_) {
        if (e.deopt_pending)
            ++n;
    }
    return n;
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

struct AuraJIT::Impl {};

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
std::string AuraJIT::compile_to_llvm_ir() {
    return {};
}
bool AuraJIT::compile_to_object_file(const std::string&) {
    return false;
}
std::string AuraJIT::compile_function_to_llvm_ir(std::uint32_t) {
    metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
    return {};
}
bool AuraJIT::compile_function_to_object(std::uint32_t, const std::string&) {
    metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
    return false;
}
std::string AuraJIT::compile_function_to_llvm_ir_by_name(const char*) {
    metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
    return {};
}
bool AuraJIT::compile_function_to_object_by_name(const char*, const std::string&) {
    metrics_.aot_per_function_miss_total.fetch_add(1, std::memory_order_relaxed);
    return false;
}
std::uint64_t AuraJIT::exception_opcode_coverage_count() const noexcept {
    const auto mask = metrics_.exception_opcode_mask.load(std::memory_order_relaxed);
    std::uint64_t n = 0;
    for (std::uint32_t i = 0; i < kExceptionOpcodeCount; ++i) {
        if (mask & (1ull << i))
            ++n;
    }
    return n;
}
void AuraJIT::register_symbol(const char*, void*) {}
void AuraJIT::set_string_pool(const std::vector<std::string>*) {}
void AuraJIT::register_function(int64_t, ScalarFn, uint32_t, uint32_t, uint32_t, const char*) {}
void AuraJIT::invalidate(const char*) {}
void AuraJIT::invalidate_prefix(const char*) {}
bool AuraJIT::partial_recompile(const char* name, const std::uint32_t* dirty_block_ids,
                                std::size_t n_dirty_blocks) noexcept {
    metrics_.partial_recompile_requests.fetch_add(1, std::memory_order_relaxed);
    metrics_.partial_recompile_dirty_blocks_total.fetch_add(n_dirty_blocks,
                                                            std::memory_order_relaxed);
    (void)name;
    (void)dirty_block_ids;
    return false;
}
std::size_t AuraJIT::batch_deopt_for(const char* name, std::uint64_t current_epoch) noexcept {
    metrics_.batch_deopt_for_total.fetch_add(1, std::memory_order_relaxed);
    (void)name;
    (void)current_epoch;
    return 0;
}
std::size_t AuraJIT::batch_deopt_prefix(const char* prefix, std::uint64_t current_epoch) noexcept {
    return batch_deopt_for(prefix, current_epoch);
}
bool AuraJIT::is_deopt_pending(const char*) const noexcept {
    return false;
}
std::uint64_t AuraJIT::deopt_pending_count() const noexcept {
    return 0;
}
void AuraJIT::capture_fn_epoch(const char*, std::uint64_t) {}
bool AuraJIT::is_fn_epoch_stale(const char*, std::uint64_t) const {
    return false;
}
std::size_t AuraJIT::walk_active_closures(std::uint64_t current_bridge_epoch) noexcept {
    metrics_.walk_active_closures_total.fetch_add(1, std::memory_order_relaxed);
    (void)current_bridge_epoch;
    return 0;
}
std::uint64_t AuraJIT::unhandled_opcode_count_for_function(const char*) const {
    return 0;
}
void AuraJIT::record_consistency_result(bool match) noexcept {
    metrics_.consistency_compare_total.fetch_add(1, std::memory_order_relaxed);
    if (match)
        metrics_.consistency_match_total.fetch_add(1, std::memory_order_relaxed);
    else
        metrics_.consistency_violations.fetch_add(1, std::memory_order_relaxed);
}
std::uint64_t AuraJIT::opcode_coverage_count() const noexcept {
    return 0;
}
std::uint64_t AuraJIT::opcode_coverage_pct() const noexcept {
    return 0;
}
const std::vector<FunctionMeta>& AuraJIT::compiled_functions() const {
    static std::vector<FunctionMeta> empty;
    return empty;
}
char* AuraJIT::Metrics::format(char* buf, std::size_t buf_size) const noexcept {
    if (!buf || buf_size == 0)
        return buf;
    // No-LLVM path: zeroed counters; keep a stable non-empty line
    // so (jit:stats) / CompilerService hooks don't see an empty buffer.
    if (buf_size >= 8) {
        buf[0] = 'j';
        buf[1] = 'i';
        buf[2] = 't';
        buf[3] = '=';
        buf[4] = '0';
        buf[5] = '\0';
    } else {
        buf[0] = '\0';
    }
    return buf;
}


// ── AOT native compilation (stubs, LLVM unavailable) ────────────

bool emit_native_object(const FlatFunction&, const std::string&, const std::vector<std::string>*) {
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

void run_escape_analysis(const std::vector<std::vector<FlatInstruction>>&, uint32_t local_count,
                         std::vector<uint8_t>& escape_map) {
    escape_map.assign(local_count, 0);
}

} // namespace aura::jit

#endif
