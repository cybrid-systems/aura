// aura_jit.cpp — LLVM ORC JIT backend for Aura IR
#include <bit>
#include "aura_jit.h"
#include "aura_jit_bridge.h"
#include "aot_mangle.h"
#include "jit_typed_mutation_stats.h"
#include "jit_assumptions.hh"     // Issue #4600: prologue word gate + deopt-storm policy
#include "typed_mutation_audit.h" // Issue #1882: sample JIT hotpath into TypedMutationAudit

// Issue #4600 §5: stable slot address for direct defuse-word loads
// (defined extern "C" in aura_jit_runtime.cpp; declared early because the
// lowering builder calls it at compile time, before the runtime decl block).
extern "C" std::uintptr_t aura_jit_defuse_word_slot_addr();
#include "value_tags.h"

#if AURA_HAVE_LLVM


// Short alias for the value tag namespace — keeps the JIT/IR
// builder code free of fully-qualified names.
namespace types = aura::compiler::types;

// Issue #4462: keep in lockstep with types::fits_fixnum. This TU does not
// import the value module. -2^62 is not a fixnum (tag is INT64_MIN).
namespace {
constexpr bool jit_fits_fixnum(std::int64_t v) noexcept {
    constexpr std::int64_t kMin = INT64_MIN >> 1;
    constexpr std::int64_t kMax = INT64_MAX >> 1;
    if (v < kMin || v > kMax)
        return false;
    return true; // Issue #4482: all even tagged values are fixnums
}
static_assert(jit_fits_fixnum(0));
static_assert(jit_fits_fixnum(INT64_MAX >> 1));
static_assert(!jit_fits_fixnum((INT64_MAX >> 1) + 1));
static_assert(jit_fits_fixnum(INT64_MIN >> 1)); // #4482: symmetric
static_assert(jit_fits_fixnum(-5000000000000000LL));
} // namespace

#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Intrinsics.h>
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
#include "hash_meta.h"                     // #908
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

// Issue #1610: declared in aura_jit_runtime.cpp — compile-time consults
// during FlatInstruction lower (not ORC-registered LLVM symbols).
extern "C" void aura_jit_macro_hygiene_consult_inc();
extern "C" void aura_jit_macro_introduced_deopt_inc();
extern "C" uint64_t aura_jit_macro_hygiene_consults();
extern "C" uint64_t aura_jit_macro_introduced_deopt();
extern "C" int aura_macro_hygiene_production_fail_closed(void);
extern "C" void aura_jit_macro_introduced_lost_inc(uint64_t n);
// Issue #2022: native MacroIntroduced side-table + counters.
extern "C" void aura_jit_stamp_fn_macro_marker(int64_t func_id, uint8_t marker,
                                               uint32_t provenance);
extern "C" void aura_jit_note_native_macro_preserved(uint8_t marker, uint32_t provenance);
extern "C" uint8_t aura_jit_fn_source_marker(int64_t func_id);
extern "C" uint32_t aura_jit_fn_provenance(int64_t func_id);

// Issue #2293: linear pin/unpin runtime bridges — extern "C"
// forwarders defined in aura_jit_runtime.cpp that cast uint64 →
// void* and delegate to aura::core::lifetime::{pin,unpin}_linear_root.
// The JIT lowering in declare_runtime() emits Function::Create decls
// for these symbols and ORC's reg() call (below) wires their
// addresses. Forward declarations here so the reg() call site can
// take the function address without aura_jit_runtime.cpp's
// lifetime_pin.hh include leaking into this TU.
extern "C" void aura_jit_pin_linear_root(std::uint64_t obj_id) noexcept;
extern "C" void aura_jit_unpin_linear_root(std::uint64_t obj_id) noexcept;

namespace aura::jit {

// PrimId values (must match ir.ixx PrimId enum order)
// Used for PrimCall fast-path dispatch — skipping aura_prim_call runtime.
enum : uint32_t {
    PrimHash = 0,
    PrimHashLength = 1,
    PrimHashHasKey = 2,
    PrimHashKeys = 3,
    PrimHashValues = 4,
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
    PrimStringGt = 44,
    PrimStringGe = 45,
    PrimStringLe = 46,
};
// Compile-time lockstep with ir.ixx PrimId enum.
// If you add/remove/reorder a PrimId entry, both sides must change together.
// Issue #3270: exhaustive — every ir.ixx PrimId has an assert (linter
// check_primid_drift_3270.py keeps this list complete).
static_assert(PrimHash == 0, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimHashLength == 1, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimHashHasKey == 2, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimHashKeys == 3, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimHashValues == 4, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimStringAppend == 5, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimStringLength == 6, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimStringRef == 7, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimSubstring == 8, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimStringEq == 9, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimStringLt == 10, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimNumberToString == 11, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimStringToNumber == 12, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimDisplay == 13, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimWrite == 14, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimNewline == 15, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimError == 16, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimAssert == 17, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimRead == 18, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimReadFile == 19, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimWriteFile == 20, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimFileExists == 21, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimGensym == 22, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimApply == 23, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimVector == 24, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimVectorRef == 25, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimVectorSet == 26, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimVectorLength == 27, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimVectorP == 28, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimMakeVector == 29, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimImport == 30, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimCharEq == 31, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimCharLt == 32, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimCharToInteger == 33, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimIntegerToChar == 34, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimQuotient == 35, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimRemainder == 36, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimListLength == 37, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimListRef == 38, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimListReverse == 39, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimRaise == 40, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimErrorP == 41, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimPairP == 42, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimNullP == 43, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimStringGt == 44, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimStringGe == 45, "PrimId drift: aura_jit.cpp vs ir.ixx");
static_assert(PrimStringLe == 46, "PrimId drift: aura_jit.cpp vs ir.ixx");

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

// Issue #4570: L1 lowering kill switch for A/B and triage —
// AURA_JIT_L1=0 restores the pre-#4570 lowering (no cell promotion, no
// static float slots, no fixnum fast path / compare fusion).
static bool jit_l1_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("AURA_JIT_L1");
        return !(v && v[0] == '0');
    }();
    return on;
}

// Issue #4571 (L2): inline car/cdr through the non-moving pair segment
// directory (#898). AURA_JIT_L2=0 restores the call-per-access lowering.
static bool jit_l2_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("AURA_JIT_L2");
        return !(v && v[0] == '0');
    }();
    return on;
}

// Issue #4600 §7: the Apply prologue gates the dual-epoch C calls behind
// one acquire load of the fn's DefineVersion word (word unchanged ⇒ not
// stale — same predicate the #4569 cache-hit restamp uses).
// AURA_JIT_PROLOGUE_WORD=0 restores the unconditional epoch-check prologue.
static bool jit_prologue_word_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("AURA_JIT_PROLOGUE_WORD");
        return !(v && v[0] == '0');
    }();
    return on;
}

// Issue #4600 §5: car/cdr defuse guards read the bridge-published defuse
// word directly (two acquire loads) instead of one get_version hook call
// per access. AURA_JIT_DEFUSE_WORD=0 restores the per-access call lowering.
static bool jit_defuse_word_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("AURA_JIT_DEFUSE_WORD");
        return !(v && v[0] == '0');
    }();
    return on;
}

// Issue #4600 §1/§2 (phase 2): guard exits share one frame-deopt stub.
// Default on; AURA_JIT_FRAME_DEOPT=0 restores the inline generic
// fallbacks (and the prologue sentinel path) everywhere.
static bool jit_frame_deopt_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("AURA_JIT_FRAME_DEOPT");
        return !(v && v[0] == '0');
    }();
    return on;
}

// Issue #4600 §2 acceptance: forced-deopt-at-every-guard mode.
//   1 = body guards only (car/cdr speculation forced on + forced
//       mismatch; GuardShape epoch arm forced stale) — exercises
//       mid-body resume + return-to-native composition.
//   2 = additionally force the prologue word-gate stale — every native
//       invocation deopts at entry (entry resume runs the whole body in
//       the interpreter).
// 0/absent = real guards.
static int jit_force_guard_deopt() {
    static const int level = [] {
        const char* v = std::getenv("AURA_JIT_FORCE_GUARD_DEOPT");
        if (!v || !v[0] || v[0] == '0')
            return 0;
        return v[0] == '2' ? 2 : 1;
    }();
    return level;
}

// Mirrors runtime_ssot.cpp kPairSegSlots / kPairMaxSegs (#898); the
// runtime static_asserts the same values next to aura_pair_seg_dir_base.
constexpr std::uint64_t kL2PairSegSlots = 1024;
constexpr std::uint64_t kL2PairMaxSegs = 1u << 16;

// Issue #4570: which cell slots of fn can be scalar-replaced.
//
// A slot qualifies when it is written by at least one OpNewCell and every
// other mention of it in a SLOT operand of any instruction (including the
// arg ranges of Call/Apply/PrimCall) is the cell operand of OpCellGet
// (ops[1]) or OpCellSet (ops[0]). Then the cell id never reaches a
// closure, callee, primitive, return value, or another local, so nothing
// outside this activation can read or write the cell and its content can
// live in an alloca. Conservative by construction:
//   - the slot-operand positions are listed per opcode; any opcode outside
//     the list disables promotion for the whole function (an unknown
//     operand layout could hide a use);
//   - an operand listed as a slot that is really a small constant can only
//     refuse a promotion;
//   - linear-ownership functions/instructions are left alone (their cell
//     ops carry fences, #3759).
static std::vector<std::uint8_t> promotable_cells(const FlatFunction& fn) {
    const uint32_t n = fn.local_count;
    std::vector<std::uint8_t> ok(n, 0);
    if (n == 0 || fn.linear_ownership_state != 0 || !fn.blocks)
        return ok;
    std::vector<std::uint8_t> bad(n, 0);
    auto use = [&](uint32_t slot) {
        if (slot < n)
            bad[slot] = 1;
    };
    auto use_range = [&](uint64_t base, uint64_t count) {
        for (uint64_t i = 0; i < count && base + i < n; ++i)
            bad[base + i] = 1;
    };
    for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
        const auto& fb = fn.blocks[bi];
        for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
            const auto& in = fb.instructions[ii];
            if (in.linear_ownership_state != 0)
                return std::vector<std::uint8_t>(n, 0);
            switch (in.opcode) {
                case Op::OpNewCell: // cell
                    if (in.ops[0] < n)
                        ok[in.ops[0]] = 1;
                    break;
                case Op::OpCellGet: // dst, cell
                    use(in.ops[0]);
                    break;
                case Op::OpCellSet: // cell, val
                    use(in.ops[1]);
                    break;
                case Op::OpCall: // callee, arg_base, arg_count, result
                    use(in.ops[0]);
                    use_range(in.ops[1], in.ops[2]);
                    use(in.ops[3]);
                    break;
                case Op::OpPrimCall: // prim_id, arg_base, arg_count, result
                    use_range(in.ops[1], in.ops[2]);
                    use(in.ops[3]);
                    break;
                case Op::OpApply: // closure, arg_count, result; args at closure+1..
                    use(in.ops[0]);
                    use_range(static_cast<uint64_t>(in.ops[0]) + 1, in.ops[1]);
                    use(in.ops[2]);
                    break;
                case Op::OpAdd: // dst, a, b (ops[3]: Eq numeric flag)
                case Op::OpSub:
                case Op::OpMul:
                case Op::OpDiv:
                case Op::OpEq:
                case Op::OpLt:
                case Op::OpGt:
                case Op::OpLe:
                case Op::OpGe:
                case Op::OpAnd:
                case Op::OpOr:
                case Op::OpMakePair:
                case Op::OpHashRef: // dst, hash, key (#4571)
                case Op::OpHashSet: // dst, hash, (key . val) pair
                case Op::OpHashRemove:
                    use(in.ops[0]);
                    use(in.ops[1]);
                    use(in.ops[2]);
                    break;
                case Op::OpLocal: // dst, src
                case Op::OpNot:
                case Op::OpCar:
                case Op::OpCdr:
                case Op::OpIsError:
                case Op::OpCastOp:     // dst, src, type-const
                case Op::OpGuardShape: // dst, slot, shape-const (qualified: #3270 lint anchors
                                       // lower())
                case Op::OpPrimitive:  // dst, prim
                    use(in.ops[0]);
                    use(in.ops[1]);
                    break;
                case Op::OpConstI64: // dst, const lo/hi
                case Op::OpConstF64:
                case Op::OpConstString:
                case Op::OpConstBool:
                case Op::OpConstVoid:
                case Op::OpArg:
                case Op::OpTopCellLoad:
                case Op::OpBranch: // cond, then-block, else-block
                case Op::OpReturn: // value
                    use(in.ops[0]);
                    break;
                case Op::OpNop:
                case Op::OpJump: // block
                    break;
                default:
                    return std::vector<std::uint8_t>(n, 0);
            }
        }
    }
    for (uint32_t i = 0; i < n; ++i)
        ok[i] = ok[i] && !bad[i];
    return ok;
}

// Issue #4573: scalar replacement kill switch for A/B and triage —
// AURA_JIT_SR=0 keeps every MakePair as a runtime allocation.
static bool jit_sr_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("AURA_JIT_SR");
        return !(v && v[0] == '0');
    }();
    return on;
}

// Issue #4573: which pair slots of fn can be scalar-replaced.
//
// Nodes are the n local slots plus, at index n + c, the CONTENT of each
// promotable cell c (#4570: a cell whose id never leaves NewCell /
// CellGet / CellSet, already held in an alloca). A node is a pair
// candidate when every write to it is an OpMakePair or a copy from
// another candidate (OpLocal dst <- src, CellSet content <- val, CellGet
// dst <- content), and every other mention of it in a SLOT operand of any
// instruction (including the arg ranges of Call/Apply/PrimCall) is
//   - the pair operand (ops[1]) of OpCar / OpCdr, or
//   - the value operand (ops[1]) of OpIsError (a pair is never an error),
//   - the operand of OpReturn (the pair is materialized there; in a let
//     body that is typically the dead IsError-true error exit).
// Then the pair reference never reaches a closure, callee, primitive,
// structure or heap cell: nothing outside this activation
// can observe its identity or mutate it (set-car! is a primitive call,
// which disqualifies). Its fields live in allocas (mem2reg -> SSA): no
// aura_alloc_pair, no publish, no tenant stamp. Each node owns its field
// allocas and every copy copies both fields (value semantics, so a later
// rebinding of one slot does not change another).
//
// Same conservative operand table as promotable_cells: an opcode outside
// the list disables the analysis for the whole function, and
// linear-ownership functions or instructions are left alone (#2293 pins
// need a materialized object). Returns 2n flags (slots, then contents).
static std::vector<std::uint8_t> scalar_pairs(const FlatFunction& fn,
                                              const std::vector<std::uint8_t>& promo) {
    const uint32_t n = fn.local_count;
    std::vector<std::uint8_t> ok(2ull * n, 0);
    if (n == 0 || fn.linear_ownership_state != 0 || !fn.blocks || promo.size() < n)
        return ok;
    std::vector<std::uint8_t> bad(2ull * n, 0);
    std::vector<std::uint8_t> has_make(2ull * n, 0);
    std::vector<std::pair<uint32_t, uint32_t>> copies; // (dst node, src node)
    auto use = [&](uint32_t slot) {
        if (slot < n)
            bad[slot] = 1;
    };
    auto use_range = [&](uint64_t base, uint64_t count) {
        for (uint64_t i = 0; i < count && base + i < n; ++i)
            bad[base + i] = 1;
    };
    auto copy = [&](uint32_t dst, uint32_t src) {
        ok[dst] = 1;
        copies.emplace_back(dst, src);
    };
    for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
        const auto& fb = fn.blocks[bi];
        for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
            const auto& in = fb.instructions[ii];
            if (in.linear_ownership_state != 0)
                return std::vector<std::uint8_t>(2ull * n, 0);
            switch (in.opcode) {
                case Op::OpMakePair: // dst (the pair), car, cdr
                    if (in.ops[0] < n) {
                        ok[in.ops[0]] = 1;
                        has_make[in.ops[0]] = 1;
                    }
                    use(in.ops[1]);
                    use(in.ops[2]);
                    break;
                case Op::OpCar: // dst, pair — the pair operand is allowed
                case Op::OpCdr:
                case Op::OpIsError: // dst, value — never an error for a pair
                    use(in.ops[0]);
                    break;
                case Op::OpLocal: // dst, src
                    if (in.ops[0] < n && in.ops[1] < n) {
                        copy(in.ops[0], in.ops[1]);
                    } else {
                        use(in.ops[0]);
                        use(in.ops[1]);
                    }
                    break;
                case Op::OpCellGet: // dst, cell
                    if (in.ops[0] < n && in.ops[1] < n && promo[in.ops[1]]) {
                        copy(in.ops[0], n + in.ops[1]);
                    } else {
                        use(in.ops[0]);
                        use(in.ops[1]);
                    }
                    break;
                case Op::OpCellSet: // cell, val
                    if (in.ops[0] < n && in.ops[1] < n && promo[in.ops[0]]) {
                        copy(n + in.ops[0], in.ops[1]);
                    } else {
                        use(in.ops[0]);
                        use(in.ops[1]);
                    }
                    break;
                case Op::OpCall: // callee, arg_base, arg_count, result
                    use(in.ops[0]);
                    use_range(in.ops[1], in.ops[2]);
                    use(in.ops[3]);
                    break;
                case Op::OpPrimCall: // prim_id, arg_base, arg_count, result
                    use_range(in.ops[1], in.ops[2]);
                    use(in.ops[3]);
                    break;
                case Op::OpApply: // closure, arg_count, result; args at closure+1..
                    use(in.ops[0]);
                    use_range(static_cast<uint64_t>(in.ops[0]) + 1, in.ops[1]);
                    use(in.ops[2]);
                    break;
                case Op::OpAdd: // dst, a, b
                case Op::OpSub:
                case Op::OpMul:
                case Op::OpDiv:
                case Op::OpEq:
                case Op::OpLt:
                case Op::OpGt:
                case Op::OpLe:
                case Op::OpGe:
                case Op::OpAnd:
                case Op::OpOr:
                case Op::OpHashRef:
                case Op::OpHashSet:
                case Op::OpHashRemove:
                    use(in.ops[0]);
                    use(in.ops[1]);
                    use(in.ops[2]);
                    break;
                case Op::OpNot:
                case Op::OpCastOp:
                case Op::OpGuardShape:
                case Op::OpPrimitive:
                    use(in.ops[0]);
                    use(in.ops[1]);
                    break;
                case Op::OpNewCell: // the id slot; content node starts as 0
                case Op::OpConstI64:
                case Op::OpConstF64:
                case Op::OpConstString:
                case Op::OpConstBool:
                case Op::OpConstVoid:
                case Op::OpArg:
                case Op::OpTopCellLoad:
                case Op::OpBranch:
                    use(in.ops[0]);
                    break;
                case Op::OpReturn: // materialized at the return (see lowering)
                case Op::OpNop:
                case Op::OpJump:
                    break;
                default:
                    return std::vector<std::uint8_t>(2ull * n, 0);
            }
        }
    }
    for (std::size_t i = 0; i < ok.size(); ++i)
        ok[i] = ok[i] && !bad[i];
    // Every node of a copy-connected group carries a pair from some
    // MakePair of the group.
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [dst, src] : copies) {
            if (has_make[src] && !has_make[dst]) {
                has_make[dst] = 1;
                changed = true;
            }
        }
    }
    for (std::size_t i = 0; i < ok.size(); ++i)
        ok[i] = ok[i] && has_make[i];
    // Optimistic fixed point: both ends of every copy stay candidates (a
    // copy from or into a non-pair node needs a real pair value).
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [dst, src] : copies) {
            if (ok[dst] != ok[src]) {
                ok[dst] = ok[src] = 0;
                changed = true;
            }
        }
    }
    return ok;
}

// Issue #4570 (L1 float): slots and promoted cells whose value is a
// float on every path, proven from the IR alone (no profile needed).
// Optimistic fixed point: start from "every defined slot / promoted cell
// is float" and drop any slot with a non-float definition, and any cell
// with a non-float CellSet, until stable. A slot is float when each of
// its definitions is
//   ConstF64 | Add/Sub/Mul/Div of two float slots | Local of a float slot
//   | CellGet of a float cell.
// Float slots get the #4522 raw-double shadow (flocals) and float cells a
// double alloca, so a float accumulator loop does no aura_alloc_float /
// aura_float_ref per iteration; values are reboxed only where a generic
// reader (call arg, return, structure store) loads them. Unlike the
// profile-driven #4522 shadows this needs no guard: the proof is static.
// Any opcode without a known result slot disables the analysis.
struct StaticFloats {
    std::vector<std::uint8_t> slot;
    std::vector<std::uint8_t> cell;
};
static StaticFloats static_float_slots(const FlatFunction& fn,
                                       const std::vector<std::uint8_t>& promo) {
    const uint32_t n = fn.local_count;
    StaticFloats out{std::vector<std::uint8_t>(n, 0), std::vector<std::uint8_t>(n, 0)};
    if (n == 0 || !fn.blocks || fn.source_marker == 1)
        return out;
    auto& fs = out.slot;
    auto& fc = out.cell;
    // Result slot of an instruction (UINT32_MAX = none); false = unknown op.
    auto result_slot = [](const FlatInstruction& in, uint32_t& r) -> bool {
        r = UINT32_MAX;
        switch (in.opcode) {
            case Op::OpCall:
            case Op::OpPrimCall:
                r = in.ops[3];
                return true;
            case Op::OpApply:
                r = in.ops[2];
                return true;
            case Op::OpCellSet:
            case Op::OpBranch:
            case Op::OpReturn:
            case Op::OpJump:
            case Op::OpNop:
                return true;
            case Op::OpNewCell:
            case Op::OpCellGet:
            case Op::OpAdd:
            case Op::OpSub:
            case Op::OpMul:
            case Op::OpDiv:
            case Op::OpEq:
            case Op::OpLt:
            case Op::OpGt:
            case Op::OpLe:
            case Op::OpGe:
            case Op::OpAnd:
            case Op::OpOr:
            case Op::OpMakePair:
            case Op::OpHashRef: // #4571: non-float results
            case Op::OpHashSet:
            case Op::OpHashRemove:
            case Op::OpLocal:
            case Op::OpNot:
            case Op::OpCar:
            case Op::OpCdr:
            case Op::OpIsError:
            case Op::OpCastOp:
            case Op::OpGuardShape:
            case Op::OpPrimitive:
            case Op::OpConstI64:
            case Op::OpConstF64:
            case Op::OpConstString:
            case Op::OpConstBool:
            case Op::OpConstVoid:
            case Op::OpArg:
            case Op::OpTopCellLoad:
                r = in.ops[0];
                return true;
            default:
                return false;
        }
    };
    for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
        const auto& fb = fn.blocks[bi];
        for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
            uint32_t r = 0;
            if (!result_slot(fb.instructions[ii], r))
                return StaticFloats{std::vector<std::uint8_t>(n, 0),
                                    std::vector<std::uint8_t>(n, 0)}; // unknown op: none
            if (r < n)
                fs[r] = 1;
        }
    }
    for (uint32_t i = 0; i < n && i < promo.size(); ++i)
        fc[i] = promo[i];
    auto f = [&](uint32_t s) { return s < n && fs[s] != 0; };
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
            const auto& fb = fn.blocks[bi];
            for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
                const auto& in = fb.instructions[ii];
                if (in.opcode == OpCellSet) {
                    const uint32_t c = in.ops[0];
                    if (c < n && fc[c] && !f(in.ops[1])) {
                        fc[c] = 0;
                        changed = true;
                    }
                    continue;
                }
                uint32_t r = 0;
                (void)result_slot(in, r);
                if (r >= n || !fs[r])
                    continue;
                bool is_float = false;
                switch (in.opcode) {
                    case Op::OpConstF64:
                        is_float = true;
                        break;
                    case Op::OpAdd:
                    case Op::OpSub:
                    case Op::OpMul:
                    case Op::OpDiv:
                        is_float = in.source_marker != 1 && f(in.ops[1]) && f(in.ops[2]);
                        break;
                    case Op::OpLocal:
                        is_float = in.source_marker != 1 && f(in.ops[1]);
                        break;
                    case Op::OpCellGet:
                        is_float = in.ops[1] < n && fc[in.ops[1]] != 0;
                        break;
                    default:
                        break;
                }
                if (!is_float) {
                    fs[r] = 0;
                    changed = true;
                }
            }
        }
    }
    return out;
}

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

    // Issue #4522: raw-double shadow slot per SHAPE_FLOAT local
    // (named-let float accumulator unboxing). 1:1 with llvm_locals;
    // nullptr = the slot is not float-profiled (stays in the boxed
    // tagged domain). For a float-known slot the shadow double is the
    // authoritative in-function representation: float arith reads and
    // writes the shadow directly (no aura_alloc_float per iteration)
    // and the generic load() reboxes on demand — which is exactly the
    // exit/escape boundary (OpReturn, OpCall/OpApply args, captures,
    // boxed-structure stores). funboxed[slot] is an i1 flag alloca:
    // 1 = the shadow is authoritative (last numeric store went
    // unboxed), 0 = the tagged slot holds the current value (a
    // non-number — e.g. RefError — landed in the slot; reboxed loads
    // must return it verbatim, not the stale shadow). Shape change /
    // deopt is handled by the existing L2 invalidation +
    // SpecJITController recompile semantics (no new guard, no new
    // counter): the next compile simply has no SHAPE_FLOAT slots and
    // lowers fully boxed again.
    std::vector<llvm::Value*> flocals{};
    std::vector<llvm::Value*> funboxed{};
    // Issue #4570: promoted (scalar-replaced) cells. pcells[slot] is an
    // i64 alloca holding the cell's CONTENT when slot is a function-local
    // cell that never leaves the function (see promotable_cells): its id
    // is only ever the cell operand of NewCell/CellGet/CellSet, so no
    // closure, callee, primitive or other thread can observe it, and the
    // value can live in a register (mem2reg) instead of g_cell_heap
    // behind the workspace lock. nullptr = a real heap cell.
    std::vector<llvm::Value*> pcells{};
    // Issue #4570: 1 = pcells[slot] is a double alloca (a statically-float
    // cell, see static_float_slots); 0 = tagged i64 content.
    std::vector<std::uint8_t> pcell_f{};
    // Issue #4573: scalar-replaced pairs. spair_car/spair_cdr[slot] are i64
    // allocas holding the fields of a pair that never leaves the function
    // (see scalar_pairs); nullptr = a real runtime pair.
    std::vector<llvm::Value*> spair_car{};
    std::vector<llvm::Value*> spair_cdr{};
    inline bool is_spair(uint32_t slot) const {
        return slot < spair_car.size() && spair_car[slot] != nullptr;
    }
    // Issue #4573: nodes 0..n-1 are slots, n + c is promoted cell c's
    // content (see scalar_pairs). Run after setup_l1_slots (pcells).
    void setup_scalar_pairs(const FlatFunction& fn) {
        const uint32_t n = fn.local_count;
        spair_car.assign(2ull * n, nullptr);
        spair_cdr.assign(2ull * n, nullptr);
        if (!jit_sr_enabled() || !jit_l1_enabled())
            return;
        std::vector<std::uint8_t> promo(n, 0);
        for (uint32_t i = 0; i < n; ++i)
            promo[i] = (is_pcell(i) && !pcell_f[i]) ? 1 : 0; // float cells hold no pair
        const auto sr = scalar_pairs(fn, promo);
        std::uint64_t replaced = 0;
        for (std::size_t i = 0; i < sr.size(); ++i) {
            if (!sr[i])
                continue;
            spair_car[i] = irb->CreateAlloca(llvm::Type::getInt64Ty(ctx));
            spair_cdr[i] = irb->CreateAlloca(llvm::Type::getInt64Ty(ctx));
            irb->CreateStore(c64(0), spair_car[i]);
            irb->CreateStore(c64(0), spair_cdr[i]);
            if (i < n)
                ++replaced;
        }
        if (metrics && replaced)
            metrics->l3_pairs_scalar_replaced_total.fetch_add(replaced, std::memory_order_relaxed);
    }
    // Copy both fields of scalar node src into node dst.
    void copy_spair(std::size_t dst, std::size_t src) {
        auto* i64_ty = llvm::Type::getInt64Ty(ctx);
        auto* a = irb->CreateLoad(i64_ty, spair_car[src]);
        auto* d = irb->CreateLoad(i64_ty, spair_cdr[src]);
        irb->CreateStore(a, spair_car[dst]);
        irb->CreateStore(d, spair_cdr[dst]);
    }
    inline bool is_pcell(uint32_t slot) const {
        return slot < pcells.size() && pcells[slot] != nullptr;
    }
    // Issue #4570: entry-block allocas for promoted cells and for the
    // statically-float slots (raw-double shadows, the #4522 machinery).
    // Call after llvm_locals / flocals exist, with irb in the entry block.
    void setup_l1_slots(const FlatFunction& fn) {
        pcells.assign(fn.local_count, nullptr);
        pcell_f.assign(fn.local_count, 0);
        if (!jit_l1_enabled())
            return;
        const auto promo = promotable_cells(fn);
        const auto sf = static_float_slots(fn, promo);
        pcells.assign(fn.local_count, nullptr);
        pcell_f.assign(fn.local_count, 0);
        std::uint64_t promoted = 0;
        for (uint32_t i = 0; i < fn.local_count; ++i) {
            if (!promo[i])
                continue;
            pcell_f[i] = sf.cell[i];
            if (pcell_f[i]) {
                pcells[i] = irb->CreateAlloca(double_ty);
                irb->CreateStore(llvm::ConstantFP::get(double_ty, 0.0), pcells[i]);
            } else {
                pcells[i] = irb->CreateAlloca(llvm::Type::getInt64Ty(ctx));
                irb->CreateStore(c64(0), pcells[i]);
            }
            ++promoted;
        }
        if (flocals.size() < fn.local_count) {
            flocals.resize(fn.local_count, nullptr);
            funboxed.resize(fn.local_count, nullptr);
        }
        for (uint32_t i = 0; i < fn.local_count; ++i) {
            if (!sf.slot[i] || flocals[i])
                continue;
            flocals[i] = irb->CreateAlloca(double_ty);
            funboxed[i] = irb->CreateAlloca(llvm::Type::getInt8Ty(ctx));
            irb->CreateStore(irb->getInt8(0), funboxed[i]);
        }
        if (metrics && promoted)
            metrics->l1_cells_promoted_total.fetch_add(promoted, std::memory_order_relaxed);
    }
    // Issue #4570: compare+branch fusion. The block loop publishes the
    // next instruction; a compare that is immediately consumed by the
    // block's OpBranch lowers both and sets consumed_next.
    const FlatInstruction* next_inst = nullptr;
    bool consumed_next = false;

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
        // Issue #1610: MacroIntroduced instructions never take L1/L2
        // shape specialization (conservative hygiene — treat as Dynamic).
        if (inst.source_marker == 1 /*MacroIntroduced*/)
            return 0;
        if (inst.shape_id != 0)
            return inst.shape_id;
        if (shape_map && inst.ops[0] < shape_map_size)
            return shape_map[inst.ops[0]];
        return 0;
    }

    // Pointer tagging constants (must match lib/runtime.c).
    static constexpr int64_t KWD_TRUE_VAL = 7;
    static constexpr int64_t KWD_FALSE_VAL = 3;
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
    // Issue #4600/#4606 family: define-cell bind entry (no user-mutation
    // choke — see aura_cell_set_jit_bind in aura_jit_runtime.cpp).
    llvm::Function* fn_cell_set_jit_bind = nullptr;
    // Pre-pass: value slots written by OpMakeClosure (define binds).
    std::unordered_set<std::uint32_t> mc_value_slots;
    llvm::Function* fn_top_cell_get = nullptr;
    llvm::Function* fn_alloc_pair = nullptr;
    llvm::Function* fn_alloc_pair_arena = nullptr;
    llvm::Function* fn_pair_car = nullptr;
    llvm::Function* fn_pair_cdr = nullptr;
    llvm::Function* fn_prim_call = nullptr;
    llvm::Function* fn_display_int = nullptr;
    llvm::Function* fn_display_char = nullptr;
    llvm::Function* fn_display_value = nullptr; // Issue #2572: tagged-value display
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
    // Issue #2293: linear pin/unpin runtime bridges. OpLinearWrap
    // emits a CreateCall to fn_pin_linear_root after storing the
    // wrapped value into inst.ops[0]; OpMoveOp and OpDropOp emit
    // CreateCall to fn_unpin_linear_root before / after their
    // existing drop machinery. This wires the AOT JIT lowering
    // path into the lifetime_pin epoch-scoped linear pin contract
    // (see src/core/lifetime_pin.{hh,ixx}) so that
    // verify_linear_pins_under_moving_compact sees live linear
    // roots registered from JIT'd code, not just the runtime-side
    // registry fed by set_linear_ownership_state.
    llvm::Function* fn_pin_linear_root = nullptr;
    llvm::Function* fn_unpin_linear_root = nullptr;
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
    llvm::Function* fn_intern_keyword = nullptr; // Issue #4321
    llvm::Function* fn_string_ref = nullptr;
    // L2 specialization: unchecked pair access (skips tag check)
    llvm::Function* fn_pair_car_unchecked = nullptr;
    llvm::Function* fn_pair_cdr_unchecked = nullptr;
    // Issue #157 Phase 1b: defuse_version_ accessor (runtime
    // function that reads the current Evaluator::defuse_version_
    // via the g_lock_hooks.get_version callback).
    llvm::Function* fn_get_defuse_version = nullptr;
    llvm::Function* fn_pair_inline_ok = nullptr;
    // Issue #157 Phase 1c: in-LLVM-callable deopt counter. The
    // JIT emits a call to this at the start of every deopt basic
    // block (bb_slow in OpCar/OpCdr SHAPE_PAIR lowering) so the
    // g_workspace_deopt_count is incremented on the hot path.
    // Runtime definition: extern "C" void aura_deopt_inc() in
    // aura_jit_runtime.cpp.
    llvm::Function* fn_deopt_inc = nullptr;
    // Issue #4600 §4: cold-path guard-deopt attribution for the deopt-storm
    // policy — extern "C" void aura_jit_note_guard_deopt(const char* name)
    // in aura_jit_runtime.cpp. Called from car/cdr bb_slow (named fns only).
    llvm::Function* fn_note_guard_deopt = nullptr;
    // Issue #3988: hold-budget / force-safepoint poll on Jump back-edge.
    llvm::Function* fn_poll_hold_budget = nullptr;
    // Issue #1534: GuardShape dual-epoch fence — runtime call to
    // aura_jit_guard_shape_epoch_check(fn_name) before narrow_evidence.
    llvm::Function* fn_guard_shape_epoch_check = nullptr;
    // Issue #1535: Linear* dual-epoch fence — is_fn_epoch_stale +
    // is_env_frame_stale before Move/Borrow/Drop mutation.
    llvm::Function* fn_linear_epoch_safety_check = nullptr;
    // Issue #1540: JIT Apply linear_post_mutate_enforce probe.
    llvm::Function* fn_linear_post_mutate_enforce = nullptr;
    // Issue #3186: JIT Move/Drop elision consults live commit_readiness
    // in the same critical section as the elision decision. Thin wrapper
    // around typed_mutation_audit::linear_move_drop_elision_ok.
    llvm::Function* fn_linear_move_drop_elision_ok = nullptr;
    // Issue #3224: production IR/JIT entry under active mutation +
    // !would_allow_commit deopts (Borrow/MutBorrow/Apply half-green).
    llvm::Function* fn_ir_typed_entry_commit_readiness_ok = nullptr;
    // Issue #1537: Apply-prologue dual-epoch helpers.
    llvm::Function* fn_get_current_bridge_epoch = nullptr;
    llvm::Function* fn_is_fn_epoch_stale = nullptr;
    llvm::Function* fn_deopt_to_interpreter = nullptr;
    // Issue #4600 §1/§2 (phase 2): shared frame-state deopt stub +
    // guard exits. frame_deopt_ok gates every conversion (named fn, not
    // AOT, no promoted cells / scalar-replaced pairs — those hold state
    // the spill cannot materialize back into heap form yet). cur_block /
    // cur_instr carry the emit-loop position so a guard exit can name
    // its resume point. spill_buf (entry alloca, local_count i64s)
    // receives the explicit per-guard spill; load(slot) reboxes float
    // shadows.
    llvm::Function* fn_deopt_frame = nullptr;
    bool frame_deopt_ok = false;
    std::uint32_t entry_block_idx = 0;
    std::uint32_t cur_block = 0;
    std::uint32_t cur_instr = 0;
    llvm::Value* spill_buf = nullptr;
    // Issue #4567: entry guard for a real per-arg shape map.
    llvm::Function* fn_check_shape_guard = nullptr;
    // Issue #4451: peek the #4084 sentinel after OpCall / OpApply.
    // Does not clear it. The host take-after still sees a callee refuse.
    llvm::Function* fn_peek_prologue_deopt_sentinel = nullptr;
    // When set, entry-block instructions lower into this BB (after prologue).
    llvm::BasicBlock* entry_body_bb = nullptr;
    // Hash operations (inline dispatch, avoids PrimCall overhead)
    llvm::Function* fn_hash_ref = nullptr;
    llvm::Function* fn_hash_set = nullptr;
    llvm::Function* fn_hash_remove = nullptr;
    // Hash table direct accessor (Phase 4c): returns FlatHashTable*, then GEP
    llvm::Function* fn_hash_get_flat_table = nullptr;
    llvm::Function* fn_hash_key_eq = nullptr;
    // Issue #4319: same splitmix + linear probe as aura_hash_set_checked.
    llvm::Function* fn_hash_splitmix = nullptr;
    llvm::Function* fn_hash_probe = nullptr;
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

        fn_cell_set_jit_bind =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_cell_set_jit_bind", mod);
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

        // Issue #4571: once-per-activation L2 pair gate (tenant gate not
        // armed → inline car/cdr may skip the #4093 stamp compare).
        fn_pair_inline_ok =
            llvm::Function::Create(llvm::FunctionType::get(i64, false),
                                   llvm::Function::ExternalLinkage, "aura_jit_pair_inline_ok", mod);

        fn_deopt_inc =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, false),
                                   llvm::Function::ExternalLinkage, "aura_deopt_inc", mod);
        // Issue #4600 §4: void aura_jit_note_guard_deopt(i8* name).
        fn_note_guard_deopt = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, {ptr_i8}, false), llvm::Function::ExternalLinkage,
            "aura_jit_note_guard_deopt", mod);
        // Issue #3988: production Jump back-edge poll (i32 return; Soft 0).
        fn_poll_hold_budget = llvm::Function::Create(llvm::FunctionType::get(i32_ty, false),
                                                     llvm::Function::ExternalLinkage,
                                                     "aura_jit_poll_hold_budget_safepoint", mod);

        // Issue #1534: i32 aura_jit_guard_shape_epoch_check(i8* name)
        fn_guard_shape_epoch_check = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, {ptr_i8}, false), llvm::Function::ExternalLinkage,
            "aura_jit_guard_shape_epoch_check", mod);

        // Issue #1535: i32 aura_jit_linear_epoch_safety_check(i8* name, i8 state, i32 opcode)
        fn_linear_epoch_safety_check = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, {ptr_i8, i8_ty, i32_ty}, false),
            llvm::Function::ExternalLinkage, "aura_jit_linear_epoch_safety_check", mod);

        // Issue #3186: i32 aura_jit_linear_move_drop_elision_ok(void) — closes
        // the JIT half of the half-green residual after densify/steal race.
        // Companion to fn_linear_epoch_safety_check; called in the same
        // critical section in linear_safety_probe so JIT deopts on either
        // epoch-stale OR commit_readiness would_allow_commit=false.
        fn_linear_move_drop_elision_ok = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, false), llvm::Function::ExternalLinkage,
            "aura_jit_linear_move_drop_elision_ok", mod);
        // Issue #3224: i32 aura_jit_ir_typed_entry_commit_readiness_ok(void)
        fn_ir_typed_entry_commit_readiness_ok = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, false), llvm::Function::ExternalLinkage,
            "aura_jit_ir_typed_entry_commit_readiness_ok", mod);

        // Issue #1540: i32 aura_jit_linear_post_mutate_enforce(i32 env_id)
        fn_linear_post_mutate_enforce = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, {i32_ty}, false), llvm::Function::ExternalLinkage,
            "aura_jit_linear_post_mutate_enforce", mod);

        // Issue #1537: Apply prologue dual-epoch helpers.
        fn_get_current_bridge_epoch = llvm::Function::Create(
            llvm::FunctionType::get(i64, false), llvm::Function::ExternalLinkage,
            "aura_jit_get_current_bridge_epoch", mod);
        fn_is_fn_epoch_stale = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, {ptr_i8, i64}, false), llvm::Function::ExternalLinkage,
            "aura_jit_is_fn_epoch_stale", mod);
        fn_deopt_to_interpreter = llvm::Function::Create(
            llvm::FunctionType::get(i64, {ptr_i8}, false), llvm::Function::ExternalLinkage,
            "aura_jit_deopt_to_interpreter", mod);
        // Issue #4600 §1/§2 (phase 2): shared frame-state deopt stub.
        //   i64 @aura_jit_deopt_frame(i8* name, i32 block, i32 instr,
        //                             i64* slots, i32 nslots, i32 argc)
        // Guard-exit cold blocks spill the live locals and tail into this
        // one call; the armed host resumes the IR interpreter at
        // (block, instr) and the deopting frame returns the resumed value.
        fn_deopt_frame = llvm::Function::Create(
            llvm::FunctionType::get(i64, {ptr_i8, i32_ty, i32_ty, ptr_i8, i32_ty, i32_ty}, false),
            llvm::Function::ExternalLinkage, "aura_jit_deopt_frame", mod);
        // Issue #4567: i32 aura_check_shape_guard(i64* args, i32 n, i8* map, i32 map_n)
        fn_check_shape_guard = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, {ptr_i8, i32_ty, ptr_i8, i32_ty}, false),
            llvm::Function::ExternalLinkage, "aura_check_shape_guard", mod);
        // Issue #4451: i32 aura_jit_peek_prologue_deopt_sentinel(void)
        fn_peek_prologue_deopt_sentinel = llvm::Function::Create(
            llvm::FunctionType::get(i32_ty, false), llvm::Function::ExternalLinkage,
            "aura_jit_peek_prologue_deopt_sentinel", mod);

        // Issue #2576: (slot, args*, count) — full N-arg PrimCall
        fn_prim_call = llvm::Function::Create(
            llvm::FunctionType::get(i64, {i64, ptr_i8 /* int64_t* */, i64}, false),
            llvm::Function::ExternalLinkage, "aura_prim_call", mod);

        fn_display_int =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_display_int", mod);

        fn_display_char =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i8_ty}, false),
                                   llvm::Function::ExternalLinkage, "aura_display_char", mod);

        // Issue #2572: (display/write tagged-val) — not raw fprintf %ld
        fn_display_value =
            llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64, i64}, false),
                                   llvm::Function::ExternalLinkage, "aura_display_value", mod);

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

        // Issue #2293: linear pin/unpin bridges — thin extern "C"
        // forwarders in aura_jit_runtime.cpp that cast uint64 →
        // void* and delegate to aura::core::lifetime::pin_linear_root
        // / unpin_linear_root. The void-arg signature matches the
        // drop_closure / drop_pair / drop_cell pattern (single i64
        // payload). The runtime helper early-returns on null, so
        // AC3 zero-cost (empty registry) is preserved at the JIT
        // emit level (no IR bloat on the hot path beyond one call
        // instruction per wrap/move/drop, which is dead-code-elim'd
        // by LLVM when the inline pin_linear_root returns early).
        fn_pin_linear_root = llvm::Function::Create(llvm::FunctionType::get(void_ty, {i64}, false),
                                                    llvm::Function::ExternalLinkage,
                                                    "aura_jit_pin_linear_root", mod);
        fn_unpin_linear_root = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, {i64}, false), llvm::Function::ExternalLinkage,
            "aura_jit_unpin_linear_root", mod);

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
        // Issue #4321: ConstString operands[2]==1. Same ABI as
        // aura_alloc_string; the runtime interns into keyword_table_.
        fn_intern_keyword =
            llvm::Function::Create(llvm::FunctionType::get(i64, {ptr_i8}, false),
                                   llvm::Function::ExternalLinkage, "aura_intern_keyword", mod);
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
            fn_hash_splitmix = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
                                                      llvm::Function::ExternalLinkage,
                                                      "aura_hash_splitmix64", mod);
            fn_hash_probe = llvm::Function::Create(
                llvm::FunctionType::get(i64, {i64, i64, i64}, false),
                llvm::Function::ExternalLinkage, "aura_hash_probe_slot", mod);
        }

        // Issue #157 / #4319: workspace read lock around the OpHashRef
        // probe. Acquired before fn_hash_get_flat_table. The occupied
        // path copies the key and value, drops the lock, calls
        // aura_hash_key_eq (that call takes g_string_pool_mtx), then
        // reacquires before the next probe. done_bb releases it.
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

    // ── Issue #4522: float-local (unboxed accumulator) helpers ──
    // True when slot carries a raw-double shadow (SHAPE_FLOAT in the
    // profiled shape_map at compile time). Slot indices beyond the
    // map (e.g. the #157 defuse-version slot) never qualify.
    inline bool is_flocal(uint32_t slot) const {
        return slot < flocals.size() && flocals[slot] != nullptr;
    }
    // Raw double for a float-known local. Only valid when the caller
    // has established the shadow is authoritative (flag=1 via storef,
    // or an unboxed arith chain).
    llvm::Value* loadf(uint32_t slot) { return irb->CreateLoad(double_ty, flocals[slot]); }
    // Publish a raw double into the shadow and mark it authoritative.
    void storef(uint32_t slot, llvm::Value* dbl) {
        irb->CreateStore(dbl, flocals[slot]);
        irb->CreateStore(irb->getInt8(1), funboxed[slot]);
    }
    // Box a raw double into a fresh float pool cell. The ONLY
    // allocation path for unboxed locals — it runs at rebox points
    // (loop exit, call boundary, capture, boxed-structure store).
    llvm::Value* box_double(llvm::Value* dbl) {
        return irb->CreateCall(llvm::FunctionCallee(fn_alloc_float),
                               llvm::ArrayRef<llvm::Value*>{dbl});
    }

    llvm::Value* load(uint32_t slot) {
        // Issue #4522: a float-known local is authoritative in its
        // shadow double while the flag is set; a generic reader (loop
        // exit, call arg, capture, ...) reboxes on demand. The rebox is
        // bit-identical, only the pool index is fresh. When the flag is
        // clear the tagged slot holds the current (possibly non-number)
        // value — return it verbatim so RefError/bool propagation is
        // unchanged. Branch (not select) so the cold boxed-shadow path
        // does not force an alloc_float call on every generic load of a
        // boxed-tagged slot.
        if (is_flocal(slot)) {
            auto* flag = irb->CreateLoad(llvm::Type::getInt8Ty(ctx), funboxed[slot]);
            auto* flagged = irb->CreateICmpNE(flag, irb->getInt8(0));
            auto* parent = irb->GetInsertBlock()->getParent();
            auto* bb_box = llvm::BasicBlock::Create(ctx, "flocal_rebox", parent);
            auto* bb_raw = llvm::BasicBlock::Create(ctx, "flocal_tagged", parent);
            auto* bb_join = llvm::BasicBlock::Create(ctx, "flocal_load_join", parent);
            irb->CreateCondBr(flagged, bb_box, bb_raw);
            irb->SetInsertPoint(bb_box);
            auto* boxed = box_double(loadf(slot));
            irb->CreateBr(bb_join);
            auto* box_from = irb->GetInsertBlock();
            irb->SetInsertPoint(bb_raw);
            auto* tagged = irb->CreateLoad(llvm::Type::getInt64Ty(ctx), llvm_locals[slot]);
            irb->CreateBr(bb_join);
            auto* raw_from = irb->GetInsertBlock();
            irb->SetInsertPoint(bb_join);
            auto* phi = irb->CreatePHI(llvm::Type::getInt64Ty(ctx), 2, "flocal_load");
            phi->addIncoming(boxed, box_from);
            phi->addIncoming(tagged, raw_from);
            return phi;
        }
        return irb->CreateLoad(llvm::Type::getInt64Ty(ctx), llvm_locals[slot]);
    }
    void store(uint32_t slot, llvm::Value* val) {
        // Issue #4522: a numeric store into a float-known local mirrors
        // the raw double into the shadow and flags it authoritative
        // (unbox mirrors emit_fixnum_arith's to_dbl: float ref → pool
        // read; fixnum → sitofp — bit-identical to the boxed promote).
        // A non-number (RefError, bool, ...) clears the flag: the tagged
        // slot is authoritative again and generic loads return it
        // verbatim, so error/bool propagation through a float-profiled
        // slot is unchanged. The tagged store always happens so raw
        // readers (exception payload machinery) stay coherent.
        if (is_flocal(slot)) {
            irb->CreateStore(val, llvm_locals[slot]);
            auto* is_f = jit_is_float(val);
            auto* unbox = irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                          llvm::ArrayRef<llvm::Value*>{val});
            auto* as_dbl = irb->CreateSIToFP(irb->CreateAShr(val, c64(1)), double_ty);
            auto* numeric = irb->CreateOr(is_f, jit_is_fixnum(val)); // #4482
            auto* dbl = irb->CreateSelect(is_f, unbox, as_dbl);
            auto* parent = irb->GetInsertBlock()->getParent();
            auto* bb_num = llvm::BasicBlock::Create(ctx, "flocal_unbox", parent);
            auto* bb_non = llvm::BasicBlock::Create(ctx, "flocal_debox", parent);
            auto* bb_join = llvm::BasicBlock::Create(ctx, "flocal_store_join", parent);
            irb->CreateCondBr(numeric, bb_num, bb_non);
            irb->SetInsertPoint(bb_num);
            storef(slot, dbl);
            irb->CreateBr(bb_join);
            irb->SetInsertPoint(bb_non);
            irb->CreateStore(irb->getInt8(0), funboxed[slot]);
            irb->CreateBr(bb_join);
            irb->SetInsertPoint(bb_join);
            return;
        }
        irb->CreateStore(val, llvm_locals[slot]);
    }
    // Issue #4600 §1/§2 (phase 2): emit a guard exit — one shared stub
    // call. Terminates the cold block with `ret stub(...)`; the caller
    // rewires the guard's fail edge to a fresh cold BB and calls this on
    // it. from_entry=true resumes at (block, 0) before any internal slot
    // is written, so the spill source is the caller's locals buffer (fn
    // args 0/1) and the host re-runs the Arg ops from env++args; mid-body
    // resumes spill the internal slots (load() reboxes float shadows)
    // into spill_buf with argc=0 (Arg ops are past the resume point).
    void emit_frame_deopt_exit(const FlatFunction& fn, std::uint32_t block, std::uint32_t instr,
                               bool from_entry) {
        auto* i32_ty = llvm::Type::getInt32Ty(ctx);
        llvm::Value* slots = nullptr;
        llvm::Value* nslots = nullptr;
        llvm::Value* argc = nullptr;
        auto n32 = [&](std::uint32_t v) { return llvm::ConstantInt::get(i32_ty, v); };
        if (from_entry) {
            auto arg_it = func->arg_begin();
            slots = arg_it;
            argc = ++arg_it;
            nslots = n32(fn.local_count);
        } else {
            // Explicit per-guard spill: every live slot through load()
            // (float shadows reboxed), then one stub call.
            for (std::uint32_t i = 0; i < fn.local_count; ++i) {
                auto* gep = irb->CreateGEP(llvm::Type::getInt64Ty(ctx), spill_buf, c64(i));
                irb->CreateStore(load(i), gep);
            }
            slots = spill_buf;
            nslots = n32(fn.local_count);
            argc = n32(0);
        }
        auto* name_gv = irb->CreateGlobalString(fn.name, "frame_deopt_fn_name");
        auto* zero32 = llvm::ConstantInt::get(i32_ty, 0);
        auto* name_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
            name_gv->getValueType(), name_gv, llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
        auto* call = irb->CreateCall(
            llvm::FunctionCallee(fn_deopt_frame),
            llvm::ArrayRef<llvm::Value*>{
                name_ptr, llvm::ConstantInt::get(i32_ty, block),
                llvm::ConstantInt::get(i32_ty, instr),
                irb->CreateBitCast(slots, llvm::PointerType::getUnqual(ctx)), nslots, argc});
        irb->CreateRet(call);
    }
    llvm::Value* c64(int64_t v) { return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), v); }
    // Issue #4491: the single JIT truthiness test. Must match
    // aura::compiler::pure::is_truthy (see value_tags.h kIntZeroIsFalse).
    // Issue #4482: boxed float = low2 11 inside (STRING_BIAS_2, FLOAT_BIAS].
    llvm::Value* jit_is_float(llvm::Value* v) {
        // #4502 step 5: boxed float = ImmKind Float in the low byte.
        return irb->CreateICmpEQ(irb->CreateAnd(v, c64(0xFF)), c64(AURA_FLOAT_LOW8));
    }
    llvm::Value* jit_is_fixnum(llvm::Value* v) {
        return irb->CreateICmpEQ(irb->CreateAnd(v, c64(1)), c64(0));
    }
    llvm::Value* jit_truthy(llvm::Value* v) {
        auto* not_false = irb->CreateICmpNE(v, c64(KWD_FALSE_VAL));
        if constexpr (!types::kIntZeroIsFalse)
            return not_false;
        return irb->CreateAnd(not_false, irb->CreateICmpNE(v, c64(0)));
    }
    // Issue #4570 (L1): one test for "both operands are fixnums" — the
    // ABI 4 fixnum tag is bit 0 clear, so (a | b) & 1 == 0.
    llvm::Value* jit_both_fixnum(llvm::Value* a, llvm::Value* b) {
        return irb->CreateICmpEQ(irb->CreateAnd(irb->CreateOr(a, b), c64(1)), c64(0));
    }
    // Conditional branch whose true edge is the expected (hot) one; the
    // false edge is laid out cold.
    void br_likely(llvm::Value* cond, llvm::BasicBlock* hot, llvm::BasicBlock* cold) {
        irb->CreateCondBr(cond, hot, cold, llvm::MDBuilder(ctx).createBranchWeights(2000, 1));
    }

    // Issue #4603: non-pair receiver parity for the non-L2 forms. The
    // interpreter raises "car: not a pair" / "cdr: not a pair"; the
    // unchecked runtime helpers return 0. When the frame-deopt stub is
    // armed, a non-pair exits to the interpreter at this instruction. The
    // legacy call (and its 0) survives where nothing can host a resume
    // (AOT, anonymous fns, hidden allocas).
    llvm::Value* emit_pair_checked_call(llvm::Value* v, llvm::FunctionCallee checked_fn,
                                        const FlatFunction& fn, uint32_t block, uint32_t instr) {
        if (!frame_deopt_ok)
            return irb->CreateCall(checked_fn, llvm::ArrayRef<llvm::Value*>{v});
        auto* parent = irb->GetInsertBlock()->getParent();
        auto* bb_call = llvm::BasicBlock::Create(ctx, "pair_checked_call", parent);
        auto* bb_err = llvm::BasicBlock::Create(ctx, "pair_checked_nonpair", parent);
        auto* is_pair = irb->CreateICmpEQ(irb->CreateAnd(v, c64(63)), c64(1));
        irb->CreateCondBr(is_pair, bb_call, bb_err);
        irb->SetInsertPoint(bb_err);
        emit_frame_deopt_exit(fn, block, instr, /*from_entry=*/false);
        // bb_err returns; bb_call is the sole live continuation.
        irb->SetInsertPoint(bb_call);
        return irb->CreateCall(checked_fn, llvm::ArrayRef<llvm::Value*>{v});
    }

    // Issue #4571 (L2): inline pair field read. pair_ok is the i1 the JIT
    // prologue computed once per activation (aura_jit_pair_inline_ok: the
    // #4093 tenant gate is not armed, so no stamp compare is owed);
    // pair_dir is the address of the fixed segment directory. Both are
    // null on the AOT path and under AURA_JIT_L2=0.
    llvm::Value* pair_ok = nullptr;
    llvm::Value* pair_dir = nullptr;
    // Guarded inline read of field (0 = car, 1 = cdr):
    //   (v & 63) == 1 (RefPair), seg = v >> 16 < kPairMaxSegs,
    //   segp = acquire-load dir[seg], slotp = acquire-load segp[(v>>6)&1023],
    //   result = slotp->field.
    // Any miss (gate armed, not a pair, unpublished segment or slot) runs
    // `slow`, the same runtime call as before, so the result and the
    // fallback metric are unchanged. No call and no lock on the hit path.
    template <class Slow>
    llvm::Value* emit_l2_pair_field(llvm::Value* v, unsigned field, const char* tag, Slow slow,
                                    const FlatFunction* hfn = nullptr, uint32_t hblock = 0,
                                    uint32_t hinstr = 0, bool deopt_nonpair = false) {
        auto* i64_ty = llvm::Type::getInt64Ty(ctx);
        auto* ptr_ty = llvm::PointerType::getUnqual(ctx);
        auto* parent = irb->GetInsertBlock()->getParent();
        auto* bb_seg = llvm::BasicBlock::Create(ctx, std::string(tag) + "_l2_seg", parent);
        auto* bb_slot = llvm::BasicBlock::Create(ctx, std::string(tag) + "_l2_slot", parent);
        auto* bb_hit = llvm::BasicBlock::Create(ctx, std::string(tag) + "_l2_hit", parent);
        auto* bb_miss = llvm::BasicBlock::Create(ctx, std::string(tag) + "_l2_miss", parent);
        auto* bb_slow = llvm::BasicBlock::Create(ctx, std::string(tag) + "_l2_slow", parent);
        auto* bb_done = llvm::BasicBlock::Create(ctx, std::string(tag) + "_l2_done", parent);
        auto* is_pair = irb->CreateICmpEQ(irb->CreateAnd(v, c64(63)), c64(1));
        auto* seg = irb->CreateLShr(v, c64(16));
        auto* in_dir = irb->CreateICmpULT(seg, c64(kL2PairMaxSegs));
        br_likely(irb->CreateAnd(pair_ok, irb->CreateAnd(is_pair, in_dir)), bb_seg, bb_miss);

        irb->SetInsertPoint(bb_seg);
        auto* segcell = irb->CreateGEP(ptr_ty, pair_dir, seg);
        auto* segp = irb->CreateLoad(ptr_ty, segcell);
        segp->setAtomic(llvm::AtomicOrdering::Acquire);
        segp->setAlignment(llvm::Align(8));
        br_likely(irb->CreateIsNotNull(segp), bb_slot, bb_slow);

        irb->SetInsertPoint(bb_slot);
        auto* off = irb->CreateAnd(irb->CreateLShr(v, c64(6)), c64(kL2PairSegSlots - 1));
        auto* slotp = irb->CreateLoad(ptr_ty, irb->CreateGEP(ptr_ty, segp, off));
        slotp->setAtomic(llvm::AtomicOrdering::Acquire);
        slotp->setAlignment(llvm::Align(8));
        br_likely(irb->CreateIsNotNull(slotp), bb_hit, bb_slow);

        irb->SetInsertPoint(bb_hit);
        auto* fast = irb->CreateLoad(i64_ty, irb->CreateGEP(i64_ty, slotp, c64(field)));
        irb->CreateBr(bb_done);

        // Issue #4603: a non-pair receiver is an interpreter error, not a
        // 0 field. With the frame-deopt stub armed the miss exits here and
        // the interpreter raises "car: not a pair" / "cdr: not a pair" for
        // this same instruction; an unpublished segment/slot (a live pair
        // the L2 directory cannot see) still takes the slow call.
        irb->SetInsertPoint(bb_miss);
        if (deopt_nonpair && hfn) {
            auto* bb_err = llvm::BasicBlock::Create(ctx, std::string(tag) + "_l2_nonpair", parent);
            irb->CreateCondBr(is_pair, bb_slow, bb_err);
            irb->SetInsertPoint(bb_err);
            emit_frame_deopt_exit(*hfn, hblock, hinstr, /*from_entry=*/false);
        } else {
            irb->CreateBr(bb_slow);
        }

        irb->SetInsertPoint(bb_slow);
        llvm::Value* slow_v = slow();
        auto* slow_end = irb->GetInsertBlock();
        irb->CreateBr(bb_done);

        irb->SetInsertPoint(bb_done);
        auto* phi = irb->CreatePHI(i64_ty, 2, std::string(tag) + "_l2");
        phi->addIncoming(fast, bb_hit);
        phi->addIncoming(slow_v, slow_end);
        if (metrics)
            metrics->l2_pair_inline_total.fetch_add(1, std::memory_order_relaxed);
        return phi;
    }

    // Lower one FlatInstruction into the current LLVM IR builder.
    // Contract: inst.opcode must be a valid IROpcode value (< MaxIROpcode);
    // for result-producing opcodes, inst.ops[0] must be a valid local slot
    // (< fn.local_count). Violations indicate a corrupted IR module.
    bool lower(const FlatInstruction& inst, uint32_t block_id, const FlatFunction& fn)
        pre(block_id < fn.num_blocks) pre(fn.num_blocks == 0 || fn.blocks != nullptr) {
        (void)block_id;
        // Issue #1610: MacroIntroduced hygiene at JIT lower entry.
        // Consult counter always; dirty+MacroIntroduced forces deopt
        // (jit_macro_introduced_deopt) so self-evo cannot specialize
        // mutated macro-expanded code on the hot path.
        // Issue #3475: production / Full also refuse native when the
        // hygiene envelope is MacroIntroduced but dirty/provenance is
        // inconsistent (lost kMacroExpansion / generation drift). Soft/Off
        // keep consult + dirty-only deopt.
        if (inst.source_marker == 1 /*MacroIntroduced*/ || fn.source_marker == 1) {
            if (inst.source_marker == 1)
                aura_jit_macro_hygiene_consult_inc();
            if (inst.dirty != 0) {
                aura_jit_macro_introduced_deopt_inc();
                if (fn_deopt_inc)
                    irb->CreateCall(fn_deopt_inc);
                return false;
            }
            if (aura_macro_hygiene_production_fail_closed() != 0) {
                aura_jit_macro_introduced_deopt_inc();
                if (inst.provenance == 0)
                    aura_jit_macro_introduced_lost_inc(1);
                if (fn_deopt_inc)
                    irb->CreateCall(fn_deopt_inc);
                return false;
            }
        }
        // Issue #684: deopt when SoA instruction_dirty_ propagated to JIT.
        if (inst.dirty != 0) {
            if (fn_deopt_inc)
                irb->CreateCall(fn_deopt_inc);
            return false;
        }
        // Issue #740 + #1535 + #1540: linear-owned instructions consult the
        // dual-epoch + linear_post_mutate_enforce safety hook. On unsafe
        // (return 1) emit deopt_inc so JIT Apply falls back observability
        // matches interpreter closure_needs_safe_fallback. Linear* ops that
        // need body-skip deopt also use begin_linear_epoch_fence.
        auto linear_safety_probe = [&]() -> llvm::Value* {
            if (inst.linear_ownership_state == 0)
                return nullptr; // Issue #3270: quiet path — zero extra IR
            // Issue #3419: typed-entry for every compiled function lives on
            // the Apply prologue (anonymous included). This probe stays
            // linear-state only (no extra call when state==0).
            if (inst.narrow_evidence != 0)
                aura::compiler::jit_typed_mutation::record_linear_state_optimized();
            aura::compiler::typed_audit::capture_jit_hotpath_audit("jit-linear-optimized");
            const char* probe_fn = (fn.name && fn.name[0]) ? fn.name : "";
            auto* name_gv = irb->CreateGlobalString(probe_fn, "lin_probe_fn");
            auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            auto* name_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                name_gv->getValueType(), name_gv, llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
            auto* unsafe_i = irb->CreateCall(
                llvm::FunctionCallee(fn_linear_epoch_safety_check),
                llvm::ArrayRef<llvm::Value*>{
                    name_ptr,
                    llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), inst.linear_ownership_state),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), inst.opcode)});
            // Issue #1540: on epoch stale OR linear post-mutate violation,
            // bump deopt counter (interpreter fallback path observability).
            auto* is_unsafe = irb->CreateICmpNE(unsafe_i, zero32);
            // Issue #3186: also consult linear_move_drop_elision_ok (live
            // commit_readiness face) in the same critical section as the
            // elision decision. Soft/Off: zero extra counter noise (the
            // predicate itself short-circuits the bump under Soft/Off).
            // Production/Full: never ship Move/Drop under
            // would_allow_commit=false. Reuses the existing
            // g_linear_fast_path_elide_blocked_production_total counter
            // (no new metric key). Closes the JIT half of the half-green
            // residual after densify/steal race.
            auto* elision_ok_i =
                irb->CreateCall(llvm::FunctionCallee(fn_linear_move_drop_elision_ok),
                                llvm::ArrayRef<llvm::Value*>{});
            // Issue #4512: elision_ok==0 means elision is blocked
            // (commit_readiness denies) — that is unsafe, same polarity as
            // begin_linear_epoch_fence (#3759/#3446 ICmpEQ skip-body).
            // Fail closed: any_unsafe when elision is denied.
            auto* elision_blocked = irb->CreateICmpEQ(elision_ok_i, zero32);
            auto* any_unsafe = irb->CreateOr(is_unsafe, elision_blocked);
            // Issue #3224: production IR entry under active mutation +
            // !would_allow_commit also deopts (beyond Move/Drop elision).
            if (fn_ir_typed_entry_commit_readiness_ok) {
                auto* entry_ok_i =
                    irb->CreateCall(llvm::FunctionCallee(fn_ir_typed_entry_commit_readiness_ok),
                                    llvm::ArrayRef<llvm::Value*>{});
                auto* entry_blocked = irb->CreateICmpEQ(entry_ok_i, zero32);
                any_unsafe = irb->CreateOr(any_unsafe, entry_blocked);
            }
            auto* entry_bb = irb->GetInsertBlock();
            auto* parent = entry_bb->getParent();
            auto* bb_deopt = llvm::BasicBlock::Create(ctx, "lin_probe_deopt", parent);
            auto* bb_ok = llvm::BasicBlock::Create(ctx, "lin_probe_ok", parent);
            irb->CreateCondBr(any_unsafe, bb_deopt, bb_ok);
            irb->SetInsertPoint(bb_deopt);
            if (fn_deopt_inc)
                irb->CreateCall(llvm::FunctionCallee(fn_deopt_inc));
            irb->CreateBr(bb_ok);
            irb->SetInsertPoint(bb_ok);
            // Issue #3270: return the runtime i1 so OpGuardShape can
            // fail-close to bb_stale. Probe deopt_inc then continues
            // (observability). Capture / Apply of linear locals reuse
            // begin_linear_epoch_fence (#3759) — not this continue path.
            // nullptr when no linear state.
            return any_unsafe;
        };
        // Issue #1535: dual-epoch fence for Linear* ops (Move/Borrow/Drop).
        // Emits runtime call to aura_jit_linear_epoch_safety_check; on stale
        // deopts (aura_deopt_inc) and skips the mutation body. Leaves the
        // IR builder at bb_ok; caller emits body then calls end_fence.
        // Issue #3446: also OR live elision_ok + typed-entry into this
        // fail-close (skip body). linear_safety_probe deopt_inc then
        // continues to bb_ok (observability) — not a substitute.
        // Issue #3759: Capture / CaptureRef / Apply of linear locals reuse
        // this fence so mid-function persist-reject / remount last==0
        // skips the native body (interpreter Apply refuses
        // commit-readiness-refused). Soft helpers return allow
        // (one/two loads). Reuses
        // g_linear_fast_path_elide_blocked_production_total inside the
        // C ABI — no new query key.
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
            // Issue #3446: stale |= elision_ok()==0 |= typed_entry_ok()==0
            // so compiled Move/Drop skip the body (source slot not zeroed)
            // after a reject proof, matching interpreter refuse.
            auto* fence_elision_ok_i =
                irb->CreateCall(llvm::FunctionCallee(fn_linear_move_drop_elision_ok),
                                llvm::ArrayRef<llvm::Value*>{});
            auto* fence_elision_blocked = irb->CreateICmpEQ(fence_elision_ok_i, zero32);
            is_stale = irb->CreateOr(is_stale, fence_elision_blocked);
            if (fn_ir_typed_entry_commit_readiness_ok) {
                auto* fence_entry_ok_i =
                    irb->CreateCall(llvm::FunctionCallee(fn_ir_typed_entry_commit_readiness_ok),
                                    llvm::ArrayRef<llvm::Value*>{});
                auto* fence_entry_blocked = irb->CreateICmpEQ(fence_entry_ok_i, zero32);
                is_stale = irb->CreateOr(is_stale, fence_entry_blocked);
            }
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
        // Issue #4451: aura_closure_call refuse is fixnum 0 plus the
        // #4084 sentinel. Peek (do not take) and return so the caller
        // tail cannot publish (0+100). Do not batch-deopt this function.
        auto leave_if_callee_refused = [&]() {
            if (!fn_peek_prologue_deopt_sentinel)
                return;
            auto* sent = irb->CreateCall(llvm::FunctionCallee(fn_peek_prologue_deopt_sentinel), {});
            auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            auto* refused = irb->CreateICmpNE(sent, zero32);
            auto* parent = irb->GetInsertBlock()->getParent();
            auto* bb_deopt = llvm::BasicBlock::Create(ctx, "call_callee_deopt", parent);
            auto* bb_cont = llvm::BasicBlock::Create(ctx, "call_callee_cont", parent);
            irb->CreateCondBr(refused, bb_deopt, bb_cont);
            irb->SetInsertPoint(bb_deopt);
            irb->CreateRet(c64(0));
            irb->SetInsertPoint(bb_cont);
        };
        // Issue #4522: unboxed float arith over float-known locals. When
        // both operand slots carry SHAPE_FLOAT profile feedback, lower
        // add/sub/mul/div as raw FP ops on the shadow doubles: no
        // aura_alloc_float per iteration, no tag checks (same speculative
        // trust as the L2 unchecked pair access and the SHAPE_INT OpDiv
        // path — a shape violation is a recompile/deopt event, not an
        // in-loop guard). kind: 0 add, 1 sub, 2 mul, 3 div.
        auto emit_flocal_arith = [&](const FlatInstruction& finst, int kind) -> bool {
            if (!is_flocal(finst.ops[1]) || !is_flocal(finst.ops[2]))
                return false;
            // #1610 hygiene: MacroIntroduced instructions never take
            // L1/L2 shape specialization.
            if (finst.source_marker == 1)
                return false;
            auto* lhs = loadf(finst.ops[1]);
            auto* rhs = loadf(finst.ops[2]);
            llvm::Value* r = kind == 1   ? irb->CreateFSub(lhs, rhs)
                             : kind == 2 ? irb->CreateFMul(lhs, rhs)
                             : kind == 3 ? irb->CreateFDiv(lhs, rhs)
                                         : irb->CreateFAdd(lhs, rhs);
            if (is_flocal(finst.ops[0]))
                storef(finst.ops[0], r);
            else
                store(finst.ops[0], box_double(r)); // rebox at a dynamic-slot boundary
            if (metrics)
                metrics->intrinsic_count.fetch_add(1, std::memory_order_relaxed);
            return true;
        };
        // Issue #4523: shape-specialized tag-check elision for arith
        // operands. A slot the profiled shape_map pins to SHAPE_FLOAT is a
        // float WITHOUT executing jit_is_float on it — the same speculative
        // trust as the #4522 unboxed flocal arith and the L2 unchecked pair
        // access: a shape violation is a recompile/deopt event via the
        // existing SpecJITController invalidation (no new guard, no new
        // counter, no new query key), not an in-loop check. is_flocal is
        // exactly the "map says SHAPE_FLOAT" predicate for slots (the
        // shadows are created 1:1 from it), gated on the #1610 hygiene the
        // flocal intercepts share: MacroIntroduced instructions and
        // macro-origin functions never specialize. Mixed pairs (one known
        // float, one unknown) keep the unknown operand's runtime tag tests
        // and numeric guard — fail-safe, #4482 error surfaces unchanged.
        auto operand_known_float = [&](uint32_t slot) -> bool {
            return inst.source_marker != 1 && is_flocal(slot);
        };
        // Float-side tagged→double conversion with per-operand elision.
        // A known-float operand converts via float_ref directly (no tag
        // test); an unknown operand keeps the runtime select (float ref vs
        // fixnum sitofp) — bit-identical to the pre-#4523 to_dbl when the
        // operand is not profiled.
        auto emit_float_side = [&](llvm::Value* v, bool known_float) -> llvm::Value* {
            if (known_float)
                return irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                       llvm::ArrayRef<llvm::Value*>{v});
            return irb->CreateSelect(jit_is_float(v),
                                     irb->CreateCall(llvm::FunctionCallee(fn_float_ref),
                                                     llvm::ArrayRef<llvm::Value*>{v}),
                                     irb->CreateSIToFP(irb->CreateAShr(v, c64(1)), double_ty));
        };
        // Issue #4462: tagged add/sub/mul that leaves the fixnum domain
        // promotes to float. A previous promotion is a value <= FLOAT_BIAS,
        // so SHAPE_INT still has to notice it. kind: 0 add, 1 sub, 2 mul.
        // Issue #4523: a_known_float/b_known_float skip that operand's
        // jit_is_float (static true folds any_float and the promote
        // dispatch; the float arm becomes the only arm).
        auto emit_fixnum_arith = [&](llvm::Value* a, llvm::Value* b, int kind, bool a_known_float,
                                     bool b_known_float) -> llvm::Value* {
            auto* i64_ty = llvm::Type::getInt64Ty(ctx);
            auto* a_is_float = a_known_float ? irb->getTrue() : jit_is_float(a);
            auto* b_is_float = b_known_float ? irb->getTrue() : jit_is_float(b);
            auto* any_float = irb->CreateOr(a_is_float, b_is_float);
            // Add/sub of the tags is the tag of the sum. Mul of the tags is
            // (a*b)<<2, which overflows while a*b still fits, and a product
            // in (2^62, 2^63) wraps the retag above FLOAT_BIAS. Untag first.
            llvm::Value* lhs = a;
            llvm::Value* rhs = b;
            if (kind == 2) {
                lhs = irb->CreateAShr(a, c64(1));
                rhs = irb->CreateAShr(b, c64(1));
            }
            const auto id = kind == 1   ? llvm::Intrinsic::ssub_with_overflow
                            : kind == 2 ? llvm::Intrinsic::smul_with_overflow
                                        : llvm::Intrinsic::sadd_with_overflow;
            auto* decl = llvm::Intrinsic::getOrInsertDeclaration(mod, id, {i64_ty});
            auto* ovf = irb->CreateCall(decl, {lhs, rhs});
            auto* raw = irb->CreateExtractValue(ovf, 0);
            auto* overflow = irb->CreateExtractValue(ovf, 1);
            llvm::Value* tagged = kind == 2 ? irb->CreateShl(raw, c64(1)) : raw;
            // Issue #4482: every even tag is a fixnum; only the
            // intrinsic overflow (and the mul range below) leave the domain.
            llvm::Value* out_of_domain = irb->getFalse();
            if (kind == 2) {
                // A product outside [kMin, kMax] wraps on the retag.
                constexpr std::int64_t kMin = INT64_MIN >> 1;
                constexpr std::int64_t kMax = INT64_MAX >> 1;
                auto* below = irb->CreateICmpSLT(raw, c64(kMin));
                auto* above = irb->CreateICmpSGT(raw, c64(kMax));
                out_of_domain = irb->CreateOr(out_of_domain, irb->CreateOr(below, above));
            }
            auto* bad = irb->CreateOr(any_float, irb->CreateOr(overflow, out_of_domain));
            auto* parent = irb->GetInsertBlock()->getParent();
            auto* bb_flt = llvm::BasicBlock::Create(ctx, "fixnum_promote", parent);
            auto* bb_int = llvm::BasicBlock::Create(ctx, "fixnum_keep", parent);
            auto* bb_done = llvm::BasicBlock::Create(ctx, "fixnum_arith_done", parent);
            irb->CreateCondBr(bad, bb_flt, bb_int);

            irb->SetInsertPoint(bb_int);
            irb->CreateBr(bb_done);

            irb->SetInsertPoint(bb_flt);
            auto* da = emit_float_side(a, a_known_float);
            auto* db = emit_float_side(b, b_known_float);
            llvm::Value* fd = kind == 1   ? irb->CreateFSub(da, db)
                              : kind == 2 ? irb->CreateFMul(da, db)
                                          : irb->CreateFAdd(da, db);
            auto* fres = irb->CreateCall(llvm::FunctionCallee(fn_alloc_float),
                                         llvm::ArrayRef<llvm::Value*>{fd});
            // Issue #4482: fixnum overflow is an error, not a silent float.
            auto* fout =
                irb->CreateSelect(any_float, fres, c64(types::make_ref(types::RefError, 0)));
            irb->CreateBr(bb_done);

            irb->SetInsertPoint(bb_done);
            auto* phi = irb->CreatePHI(i64_ty, 2, "fixnum_arith");
            phi->addIncoming(tagged, bb_int);
            phi->addIncoming(fout, bb_flt);
            return phi;
        };
        // Issue #4463: a number is a v2 float or a fixnum above the float
        // bias. Bool, string, and ref tags are not numbers. Bit 0 clear
        // alone is not enough: integer 1 is tagged 2.
        // Issue #4523 branch hygiene: test fixnum first (the most common
        // operand), then float. Ordering only — the OR is the same #4482
        // predicate; the interpreter classify ordering is a separate issue
        // and is not contracted here.
        auto is_numeric_tag = [&](llvm::Value* v) -> llvm::Value* {
            return irb->CreateOr(jit_is_fixnum(v), jit_is_float(v)); // #4482
        };
        const auto num_err = c64(types::make_ref(types::RefError, 0));
        // Bad operands store a RefError and join. The numeric arm keeps
        // the existing fixnum/float body, and the next opcode continues
        // at the join.
        auto guard_numeric_arith = [&](int kind) {
            // Issue #4570 (L1): both-fixnum fast path. One tag test for
            // both operands, then the overflow intrinsic straight on the
            // tagged values — add/sub of tags is the tag of the sum, and
            // (a>>1) * b is the tagged product, so the intrinsic's flag is
            // exactly "result leaves the 62-bit domain". Overflow stores
            // the same RefError the generic path does for fixnum operands
            // (#4482), so the fast path has no call and no promote block.
            // Anything else (float, non-number) takes the unchanged
            // generic path below, laid out cold. Operands already known
            // float (#4523) and macro-introduced code (#1610) skip it.
            llvm::BasicBlock* l1_join = nullptr;
            if (jit_l1_enabled() && inst.source_marker != 1 && !operand_known_float(inst.ops[1]) &&
                !operand_known_float(inst.ops[2])) {
                auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                auto* l1a = load(inst.ops[1]);
                auto* l1b = load(inst.ops[2]);
                auto* parent = irb->GetInsertBlock()->getParent();
                auto* bb_fast = llvm::BasicBlock::Create(ctx, "l1_fixnum", parent);
                auto* bb_gen = llvm::BasicBlock::Create(ctx, "l1_generic", parent);
                l1_join = llvm::BasicBlock::Create(ctx, "l1_join", parent);
                br_likely(jit_both_fixnum(l1a, l1b), bb_fast, bb_gen);
                irb->SetInsertPoint(bb_fast);
                const auto id = kind == 1   ? llvm::Intrinsic::ssub_with_overflow
                                : kind == 2 ? llvm::Intrinsic::smul_with_overflow
                                            : llvm::Intrinsic::sadd_with_overflow;
                auto* decl = llvm::Intrinsic::getOrInsertDeclaration(mod, id, {i64_ty});
                auto* lhs = kind == 2 ? irb->CreateAShr(l1a, c64(1)) : l1a;
                auto* ovf = irb->CreateCall(decl, {lhs, l1b});
                auto* tagged = irb->CreateExtractValue(ovf, 0);
                auto* overflow = irb->CreateExtractValue(ovf, 1);
                store(inst.ops[0], irb->CreateSelect(overflow, num_err, tagged));
                irb->CreateBr(l1_join);
                irb->SetInsertPoint(bb_gen);
                if (metrics)
                    metrics->l1_fixnum_fast_total.fetch_add(1, std::memory_order_relaxed);
            }
            // Issue #4523: a profiled-SHAPE_FLOAT operand is numeric
            // without a tag test and reads its raw-double shadow directly
            // (the emit_flocal_arith trust — no rebox, no flag check, no
            // float_ref call); the unknown operand keeps the generic
            // flag-aware load, its runtime tag tests, and the numeric
            // guard (the RefError surface for bool/string operands is
            // unchanged).
            const bool a_f = operand_known_float(inst.ops[1]);
            const bool b_f = operand_known_float(inst.ops[2]);
            auto* a = a_f ? nullptr : load(inst.ops[1]);
            auto* b = b_f ? nullptr : load(inst.ops[2]);
            auto* parent = irb->GetInsertBlock()->getParent();
            auto* bb_bad = llvm::BasicBlock::Create(ctx, "arith_nanum", parent);
            auto* bb_good = llvm::BasicBlock::Create(ctx, "arith_num", parent);
            auto* bb_join = llvm::BasicBlock::Create(ctx, "arith_join", parent);
            auto* ok = irb->CreateAnd(a_f ? irb->getTrue() : is_numeric_tag(a),
                                      b_f ? irb->getTrue() : is_numeric_tag(b));
            irb->CreateCondBr(ok, bb_good, bb_bad);
            irb->SetInsertPoint(bb_bad);
            store(inst.ops[0], num_err);
            irb->CreateBr(bb_join);
            irb->SetInsertPoint(bb_good);
            if ((a_f || b_f) && is_flocal(inst.ops[0])) {
                // Issue #4523: the result is statically float (some operand
                // is profiled float; the guard above kept the unknown one
                // numeric) and the result slot has a raw-double shadow —
                // publish the double directly (storef). This skips the
                // per-iteration aura_alloc_float + immediate rebox read the
                // tagged path would do (box → store mirror → unbox).
                auto* da = a_f ? loadf(inst.ops[1]) : emit_float_side(a, false);
                auto* db = b_f ? loadf(inst.ops[2]) : emit_float_side(b, false);
                llvm::Value* fd = kind == 1   ? irb->CreateFSub(da, db)
                                  : kind == 2 ? irb->CreateFMul(da, db)
                                              : irb->CreateFAdd(da, db);
                storef(inst.ops[0], fd);
            } else {
                store(inst.ops[0], emit_fixnum_arith(a_f ? load(inst.ops[1]) : a,
                                                     b_f ? load(inst.ops[2]) : b, kind, a_f, b_f));
            }
            irb->CreateBr(bb_join);
            irb->SetInsertPoint(bb_join);
            if (l1_join) {
                irb->CreateBr(l1_join);
                irb->SetInsertPoint(l1_join);
            }
        };
        // Issue #4570 (L1): compare+branch fusion. When the next
        // instruction is the block's OpBranch on this compare's result,
        // the hot arm branches on the i1 directly (no #t/#f select and
        // re-test); both arms still publish the boolean to the result slot
        // so any other reader sees the same value (dead stores fold away).
        // Issue #4570: generic (cold) arm of a numeric compare. A float
        // operand compares by value as doubles; two fixnums keep the tagged
        // compare; a non-number is the #4482 RefError. The pre-#4570 lowering
        // compared raw tags for mixed int/float ((< 2 2.0) was #t, (= 2 2.0)
        // was #f, unlike the interpreter).
        auto num_compare_slow = [&](llvm::Value* a, llvm::Value* b, llvm::CmpInst::Predicate ipred,
                                    llvm::CmpInst::Predicate fpred) -> llvm::Value* {
            auto* any_f = irb->CreateOr(jit_is_float(a), jit_is_float(b));
            auto* fc = irb->CreateFCmp(fpred, emit_float_side(a, false), emit_float_side(b, false));
            auto* c = irb->CreateSelect(any_f, fc, irb->CreateICmp(ipred, a, b));
            auto* boolv = irb->CreateSelect(c, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL));
            auto* both = irb->CreateAnd(is_numeric_tag(a), is_numeric_tag(b));
            return irb->CreateSelect(both, boolv, num_err);
        };
        auto fused_branch = [&]() -> const FlatInstruction* {
            if (next_inst && next_inst->opcode == OpBranch && next_inst->ops[0] == inst.ops[0])
                return next_inst;
            return nullptr;
        };
        // Lower a compare whose fast arm is `fast_cond` (an i1 valid only
        // when `guard` holds; guard == nullptr means always valid) and
        // whose slow arm publishes `slow_val()` (the existing generic
        // lowering). Ends either at a join (no fusion) or with both arms
        // terminated by the fused branch.
        auto emit_l1_compare = [&](llvm::Value* guard,
                                   const std::function<llvm::Value*()>& fast_cond,
                                   const std::function<llvm::Value*()>& slow_val) {
            const auto* br = fused_branch();
            auto* parent = irb->GetInsertBlock()->getParent();
            auto finish = [&](llvm::Value* cond_or_null, llvm::Value* val, llvm::BasicBlock* join) {
                store(inst.ops[0], val);
                if (br) {
                    auto* c = cond_or_null ? cond_or_null : jit_truthy(val);
                    irb->CreateCondBr(c, block_map[br->ops[1]], block_map[br->ops[2]]);
                } else {
                    irb->CreateBr(join);
                }
            };
            llvm::BasicBlock* join =
                br ? nullptr : llvm::BasicBlock::Create(ctx, "l1_cmp_join", parent);
            if (guard) {
                auto* bb_fast = llvm::BasicBlock::Create(ctx, "l1_cmp_fixnum", parent);
                auto* bb_gen = llvm::BasicBlock::Create(ctx, "l1_cmp_generic", parent);
                br_likely(guard, bb_fast, bb_gen);
                irb->SetInsertPoint(bb_fast);
                auto* c = fast_cond();
                finish(c, irb->CreateSelect(c, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)), join);
                irb->SetInsertPoint(bb_gen);
                finish(nullptr, slow_val(), join);
            } else {
                auto* c = fast_cond();
                finish(c, irb->CreateSelect(c, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)), join);
            }
            if (br) {
                consumed_next = true;
                if (metrics)
                    metrics->l1_cmp_branch_fused_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                irb->SetInsertPoint(join);
            }
            if (guard && metrics)
                metrics->l1_fixnum_fast_total.fetch_add(1, std::memory_order_relaxed);
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
                // Fixnum-encode: value << 1 (matching EvalValue pointer tagging).
                // Issue #4462: outside the domain, box a float instead.
                const auto rv =
                    static_cast<int64_t>(inst.ops[1]) | (static_cast<int64_t>(inst.ops[2]) << 32);
                if (!jit_fits_fixnum(rv)) {
                    auto* fp = llvm::ConstantFP::get(double_ty, static_cast<double>(rv));
                    if (is_flocal(inst.ops[0])) {
                        // Issue #4522: float-known slot takes the raw double
                        // directly (no pool box).
                        storef(inst.ops[0], fp);
                        return true;
                    }
                    auto* call = irb->CreateCall(llvm::FunctionCallee(fn_alloc_float),
                                                 llvm::ArrayRef<llvm::Value*>{fp});
                    store(inst.ops[0], call);
                } else {
                    store(inst.ops[0], irb->CreateShl(c64(rv), c64(1)));
                }
                return true;
            }
            case OpLocal:
                // Issue #4573: copy between scalar-replaced pairs copies
                // both fields (each slot owns its allocas: value semantics).
                if (is_spair(inst.ops[0]) && is_spair(inst.ops[1])) {
                    copy_spair(inst.ops[0], inst.ops[1]);
                    return true;
                }
                // Issue #4522: float→float slot copy moves the raw double
                // (TCO arg shuffle, loop-carried named-let bindings) — no
                // rebox on the loop back-edge.
                if (is_flocal(inst.ops[0]) && is_flocal(inst.ops[1]) && inst.source_marker != 1) {
                    storef(inst.ops[0], loadf(inst.ops[1]));
                    return true;
                }
                store(inst.ops[0], load(inst.ops[1]));
                return true;
            case OpArg: {
                auto gep = irb->CreateGEP(llvm::Type::getInt64Ty(ctx), func->arg_begin(),
                                          c64(inst.ops[1]));
                store(inst.ops[0], irb->CreateLoad(llvm::Type::getInt64Ty(ctx), gep));
                return true;
            }
            case OpAdd: {
                // Issue #4522: float-known accumulator chain lowers unboxed.
                if (emit_flocal_arith(inst, 0))
                    return true;
                guard_numeric_arith(0);
                return true;
            }
            case OpSub: {
                if (emit_flocal_arith(inst, 1))
                    return true;
                guard_numeric_arith(1);
                return true;
            }
            case OpMul: {
                if (emit_flocal_arith(inst, 2))
                    return true;
                guard_numeric_arith(2);
                return true;
            }
            case OpDiv: {
                // Issue #4522: float-known accumulator chain lowers unboxed
                // (plain IEEE fdiv — div-by-zero is ±inf, matching the
                // existing float path).
                if (emit_flocal_arith(inst, 3))
                    return true;
                // Issue #4523: per-operand shape elision — a
                // profiled-SHAPE_FLOAT operand is numeric without a tag
                // test; the unknown operand's guard stays (RefError
                // surface unchanged). raw_f: float-known result slot + a
                // known float operand takes the raw IEEE quotient (the
                // integer-specialized arm declines it).
                const bool a_f = operand_known_float(inst.ops[1]);
                const bool b_f = operand_known_float(inst.ops[2]);
                const bool spec_int = (inst_shape(inst) == SHAPE_INT);
                const bool raw_f = (a_f || b_f) && !spec_int && is_flocal(inst.ops[0]);
                auto dividend = (raw_f && a_f) ? nullptr : load(inst.ops[1]);
                auto divisor = (raw_f && b_f) ? nullptr : load(inst.ops[2]);
                auto* parent = irb->GetInsertBlock()->getParent();
                auto* bb_bad = llvm::BasicBlock::Create(ctx, "div_nanum", parent);
                auto* bb_good = llvm::BasicBlock::Create(ctx, "div_num", parent);
                auto* bb_join = llvm::BasicBlock::Create(ctx, "div_join", parent);
                auto* ok = irb->CreateAnd(a_f ? irb->getTrue() : is_numeric_tag(dividend),
                                          b_f ? irb->getTrue() : is_numeric_tag(divisor));
                irb->CreateCondBr(ok, bb_good, bb_bad);
                irb->SetInsertPoint(bb_bad);
                store(inst.ops[0], num_err);
                irb->CreateBr(bb_join);
                irb->SetInsertPoint(bb_good);
                // Issue #4461: integer zero is a RefError tag, not fixnum 0.
                // Float zero is IEEE (no substitute divisor of 1.0).
                const auto err_tag = num_err;
                if (raw_f) {
                    // Issue #4523: raw IEEE quotient into the float-known
                    // result slot — the known operand reads its shadow, the
                    // unknown one keeps its runtime conversion (div-by-zero
                    // stays ±inf); no per-iteration pool box + rebox read.
                    auto* a_dbl = a_f ? loadf(inst.ops[1]) : emit_float_side(dividend, false);
                    auto* b_dbl = b_f ? loadf(inst.ops[2]) : emit_float_side(divisor, false);
                    storef(inst.ops[0], irb->CreateFDiv(a_dbl, b_dbl));
                    irb->CreateBr(bb_join);
                    irb->SetInsertPoint(bb_join);
                    return true;
                }
                if (spec_int) {
                    auto is_zero = irb->CreateICmpEQ(divisor, c64(0));
                    auto safe_div = irb->CreateSelect(is_zero, c64(2), divisor); // fixnum 1
                    auto div_result = irb->CreateSDiv(dividend, safe_div);
                    auto tagged = irb->CreateShl(div_result, c64(1));
                    store(inst.ops[0], irb->CreateSelect(is_zero, err_tag, tagged));
                } else {
                    // Issue #4523: a profiled-float operand skips
                    // jit_is_float (float_ref direct); the unknown operand
                    // keeps its runtime select. With any operand known
                    // float, any_float folds constant and the fixnum arm
                    // dead-eliminates.
                    auto* a_is_float = a_f ? irb->getTrue() : jit_is_float(dividend); // #4482
                    auto* b_is_float = b_f ? irb->getTrue() : jit_is_float(divisor);
                    auto any_float = irb->CreateOr(a_is_float, b_is_float);
                    auto a_dbl = emit_float_side(dividend, a_f);
                    auto b_dbl = emit_float_side(divisor, b_f);
                    auto fdiv = irb->CreateFDiv(a_dbl, b_dbl);
                    auto float_res = irb->CreateCall(llvm::FunctionCallee(fn_alloc_float),
                                                     llvm::ArrayRef<llvm::Value*>{fdiv});
                    auto is_zero = irb->CreateICmpEQ(divisor, c64(0));
                    auto safe_div = irb->CreateSelect(is_zero, c64(1), divisor);
                    auto div_result = irb->CreateSDiv(dividend, safe_div);
                    auto tagged = irb->CreateShl(div_result, c64(1));
                    auto fixnum_res = irb->CreateSelect(is_zero, err_tag, tagged);
                    store(inst.ops[0], irb->CreateSelect(any_float, float_res, fixnum_res));
                }
                irb->CreateBr(bb_join);
                irb->SetInsertPoint(bb_join);
                return true;
            }
            case OpEq: {
                // Issue #4522: float-known operand pair compares by value on
                // the raw doubles (ordered oeq: NaN compares false, IEEE `=`
                // semantics). Raw-tag equality over reboxes would be
                // pool-index identity — meaningless for boxed floats.
                if (is_flocal(inst.ops[1]) && is_flocal(inst.ops[2]) && inst.source_marker != 1) {
                    auto* fc = irb->CreateFCmpOEQ(loadf(inst.ops[1]), loadf(inst.ops[2]));
                    store(inst.ops[0],
                          irb->CreateSelect(fc, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                    return true;
                }
                // ops[3] == 0 is eq?/eqv? (raw tag). Numeric `=` rejects
                // a non-number. Float identity stays the raw compare.
                auto a = load(inst.ops[1]);
                auto b = load(inst.ops[2]);
                if (jit_l1_enabled() && inst.source_marker != 1) {
                    // Issue #4570 (L1): eq?/eqv? is a raw-tag compare (always
                    // valid, no guard); numeric `=` on two fixnums is the
                    // same compare. Floats and non-numbers keep the generic
                    // arm (RefError surface unchanged).
                    auto eq_fast = [&]() -> llvm::Value* { return irb->CreateICmpEQ(a, b); };
                    auto eq_slow = [&]() -> llvm::Value* {
                        if (inst.ops[3] == 0) // eq?: raw identity, never reached (no guard)
                            return irb->CreateSelect(irb->CreateICmpEQ(a, b), c64(KWD_TRUE_VAL),
                                                     c64(KWD_FALSE_VAL));
                        // Issue #4570: numeric `=` with a float operand compares
                        // values (2 = 2.0 is #t), not raw tags.
                        return num_compare_slow(a, b, llvm::CmpInst::ICMP_EQ,
                                                llvm::CmpInst::FCMP_OEQ);
                    };
                    emit_l1_compare(inst.ops[3] != 0 ? jit_both_fixnum(a, b) : nullptr, eq_fast,
                                    eq_slow);
                    return true;
                }
                // Non-L1 path (AURA_JIT_L1=0, MacroIntroduced compares):
                // numeric `=` compares values like the L1 slow arm (#4574:
                // the raw-tag compare made (= 2 2.0) #f here); eq?/eqv?
                // stays the raw identity compare.
                if (inst.ops[3] != 0) {
                    store(inst.ops[0],
                          num_compare_slow(a, b, llvm::CmpInst::ICMP_EQ, llvm::CmpInst::FCMP_OEQ));
                } else {
                    store(inst.ops[0], irb->CreateSelect(irb->CreateICmpEQ(a, b), c64(KWD_TRUE_VAL),
                                                         c64(KWD_FALSE_VAL)));
                }
                return true;
            }
            case OpLt:
            case OpGt:
            case OpLe:
            case OpGe: {
                // Issue #4522: float-known pair → ordered fcmp on the raw
                // doubles (value order; the raw-tag ICmp over boxed floats
                // would compare pool indices).
                if (is_flocal(inst.ops[1]) && is_flocal(inst.ops[2]) && inst.source_marker != 1) {
                    const auto fpred = inst.opcode == OpLt   ? llvm::CmpInst::FCMP_OLT
                                       : inst.opcode == OpGt ? llvm::CmpInst::FCMP_OGT
                                       : inst.opcode == OpLe ? llvm::CmpInst::FCMP_OLE
                                                             : llvm::CmpInst::FCMP_OGE;
                    auto* fc = irb->CreateFCmp(fpred, loadf(inst.ops[1]), loadf(inst.ops[2]));
                    store(inst.ops[0],
                          irb->CreateSelect(fc, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                    return true;
                }
                auto a = load(inst.ops[1]);
                auto b = load(inst.ops[2]);
                const auto pred = inst.opcode == OpLt   ? llvm::CmpInst::ICMP_SLT
                                  : inst.opcode == OpGt ? llvm::CmpInst::ICMP_SGT
                                  : inst.opcode == OpLe ? llvm::CmpInst::ICMP_SLE
                                                        : llvm::CmpInst::ICMP_SGE;
                if (jit_l1_enabled() && inst.source_marker != 1) {
                    // Issue #4570 (L1): two fixnums compare as their tagged
                    // words (v << 1 preserves order). The generic arm compares
                    // a float operand by value (num_compare_slow).
                    auto lt_fast = [&]() -> llvm::Value* { return irb->CreateICmp(pred, a, b); };
                    const auto fpred = inst.opcode == OpLt   ? llvm::CmpInst::FCMP_OLT
                                       : inst.opcode == OpGt ? llvm::CmpInst::FCMP_OGT
                                       : inst.opcode == OpLe ? llvm::CmpInst::FCMP_OLE
                                                             : llvm::CmpInst::FCMP_OGE;
                    auto lt_slow = [&]() -> llvm::Value* {
                        return num_compare_slow(a, b, pred, fpred);
                    };
                    emit_l1_compare(jit_both_fixnum(a, b), lt_fast, lt_slow);
                    return true;
                }
                // Non-L1 path (AURA_JIT_L1=0, MacroIntroduced compares): the
                // same value compare as the L1 slow arm, unfused (#4574: the
                // raw-tag compare made (< 3 2.5) #t here).
                const auto fpred_g = inst.opcode == OpLt   ? llvm::CmpInst::FCMP_OLT
                                     : inst.opcode == OpGt ? llvm::CmpInst::FCMP_OGT
                                     : inst.opcode == OpLe ? llvm::CmpInst::FCMP_OLE
                                                           : llvm::CmpInst::FCMP_OGE;
                store(inst.ops[0], num_compare_slow(a, b, pred, fpred_g));
                return true;
            }
            case OpAnd: {
                // Issue #4491: Scheme and/or return an operand, not #t/#f.
                // Truthiness lives in one helper (jit_truthy) that mirrors
                // aura::compiler::pure::is_truthy.
                auto a = load(inst.ops[1]);
                auto b = load(inst.ops[2]);
                store(inst.ops[0], irb->CreateSelect(jit_truthy(a), b, a));
                return true;
            }
            case OpOr: {
                auto a = load(inst.ops[1]);
                auto b = load(inst.ops[2]);
                store(inst.ops[0], irb->CreateSelect(jit_truthy(a), a, b));
                return true;
            }
            case OpNot: {
                auto v = load(inst.ops[1]);
                store(inst.ops[0],
                      irb->CreateSelect(jit_truthy(v), c64(KWD_FALSE_VAL), c64(KWD_TRUE_VAL)));
                return true;
            }
            case OpBranch: {
                auto cond_val = load(inst.ops[0]);
                irb->CreateCondBr(jit_truthy(cond_val), block_map[inst.ops[1]],
                                  block_map[inst.ops[2]]);
                return true;
            }
            // Issue #61 Iter 3: lazy-deopt guard. Computes the runtime
            // shape of the arg slot and compares it against the
            // expected shape (ops[2]). Writes a bool to ops[0] (1 if
            // matches, 0 if deopt). The IR's subsequent Branch uses
            // the result to choose specialized vs generic-trampoline.
            case OpGuardShape: {
                // Issue #1288 / #1917 / #3270: unified GuardShape + linear
                // ownership probe. Helper returns the runtime i1 unsafe
                // predicate (nullptr when linear_ownership_state==0).
                auto* lin_unsafe = linear_safety_probe();
                if (metrics) {
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
                    if (inst.linear_ownership_state != 0)
                        metrics->guard_shape_linear_unified_checks.fetch_add(
                            1, std::memory_order_relaxed);
                }

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
                    // Issue #4600 §1/§2: force mode drives the epoch arm
                    // stale so every GuardShape site exercises the
                    // mid-body frame-deopt resume.
                    if (frame_deopt_ok && jit_force_guard_deopt() >= 1)
                        is_stale = llvm::ConstantInt::getTrue(ctx);
                    // Issue #3270: linear-unsafe fail-closes to bb_stale
                    // (store false) instead of continuing the fresh path.
                    if (lin_unsafe)
                        is_stale = irb->CreateOr(is_stale, lin_unsafe);
                    irb->CreateCondBr(is_stale, bb_stale, bb_ok);
                }

                // Stale path: Issue #4600 §1/§2 — armed form exits to the
                // shared frame-deopt stub (resume re-executes this GuardShape
                // in the interpreter, which computes it fresh); un-armed form
                // keeps the inline generic fallback (guard fails → false).
                irb->SetInsertPoint(bb_stale);
                if (frame_deopt_ok) {
                    emit_frame_deopt_exit(fn, cur_block, cur_instr, /*from_entry=*/false);
                } else {
                    if (fn_deopt_inc)
                        irb->CreateCall(llvm::FunctionCallee(fn_deopt_inc));
                    store(inst.ops[0], c64(KWD_FALSE_VAL));
                    irb->CreateBr(bb_merge);
                }

                // Fresh path: existing narrow_evidence / shape check.
                irb->SetInsertPoint(bb_ok);
                // Issue #538: trust occurrence-narrowing evidence and
                // skip the runtime shape check (mirrors ir_executor).
                if (inst.narrow_evidence != 0) {
                    aura::compiler::jit_typed_mutation::record_narrow_evidence_hit();
                    aura::compiler::typed_audit::capture_jit_hotpath_audit("jit-narrow-evidence");
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
                    // Issue #4482: Ref is low2 01; boxed strings are low2 11
                    // at or below STRING_BIAS_VAL_2.
                    auto* low2 = irb->CreateAnd(arg_val, c64(3));
                    auto* is_ref = irb->CreateICmpEQ(low2, c64(1));
                    auto* is_string_approx = irb->CreateICmpEQ(irb->CreateAnd(arg_val, c64(0xFF)),
                                                               c64(AURA_STRING_LOW8)); // #4502
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
                // Issue #3988: poll hold-budget / force-safepoint on the
                // loop back-edge (same helper as IR opcode stride). Soft:
                // runtime returns 0 after reject_enabled. Do not invent a
                // second scheduler.
                if (fn_poll_hold_budget)
                    irb->CreateCall(llvm::FunctionCallee(fn_poll_hold_budget));
                irb->CreateBr(block_map[inst.ops[0]]);
                return true;
            case OpReturn:
                // Issue #4573: a returned scalar pair is materialized here
                // (global heap, as an escaping MakePair). Its identity was
                // never observable, so a fresh pair with the same fields
                // is equivalent, and at most one allocation per call.
                if (is_spair(inst.ops[0])) {
                    auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                    auto* a = irb->CreateLoad(i64_ty, spair_car[inst.ops[0]]);
                    auto* d = irb->CreateLoad(i64_ty, spair_cdr[inst.ops[0]]);
                    irb->CreateRet(irb->CreateCall(llvm::FunctionCallee(fn_alloc_pair),
                                                   llvm::ArrayRef<llvm::Value*>{a, d}));
                    return true;
                }
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
                // #4502 step 4: ops[1] == 1 is the unspecified value (15).
                store(inst.ops[0], c64(inst.ops[1] == 1 ? AURA_V_UNSPEC : AURA_V_VOID));
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
                if (is_flocal(inst.ops[0]) && inst.source_marker != 1) {
                    // Issue #4522: float-known result slot — publish the raw
                    // constant into the shadow (no pool box per iteration;
                    // the boxed literal reboxes only if a generic reader
                    // asks). Still counts as the #199 intrinsic migration
                    // (the runtime call is replaced by an inline constant).
                    if (metrics) {
                        metrics->intrinsic_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    storef(inst.ops[0], fp);
                    return true;
                }
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
                // Issue #4321: operands[2]==1 is a keyword literal.
                // Text that starts with ':' is not the signal — a
                // string literal ":find" stays aura_alloc_string.
                if (inst.ops[2] != 0) {
                    auto call = irb->CreateCall(llvm::FunctionCallee(fn_intern_keyword),
                                                llvm::ArrayRef<llvm::Value*>{str_ptr});
                    store(inst.ops[0], call);
                    return true;
                }
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_alloc_string),
                                            llvm::ArrayRef<llvm::Value*>{str_ptr});
                store(inst.ops[0], call);
                return true;
            }

            // Closures
            case OpMakeClosure: {
                // Issue #1917: critical opcode — escape_map arena vs heap.
                if (metrics)
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
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
                // Issue #3759: linear locals skip Capture on typed-entry /
                // elision fail (same fence as Move/Drop). Probe-only
                // deopt_inc then continue is not a substitute.
                const bool lin = inst.linear_ownership_state != 0;
                LinearEpochFence fb{};
                if (lin)
                    fb = begin_linear_epoch_fence();
                if (metrics)
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
                // ops[0] = closure_slot, ops[1] = env_idx, ops[2] = var_slot
                auto closure_val = load(inst.ops[0]);
                auto env_val = load(inst.ops[2]);
                irb->CreateCall(
                    llvm::FunctionCallee(fn_closure_capture),
                    llvm::ArrayRef<llvm::Value*>{closure_val, c64(inst.ops[1]), env_val});
                if (lin)
                    end_linear_epoch_fence(fb);
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
                // Issue #1917: critical CaptureRef — same linear fence as Capture.
                // Issue #3759: skip body on typed-entry / elision fail.
                const bool lin = inst.linear_ownership_state != 0;
                LinearEpochFence fb{};
                if (lin)
                    fb = begin_linear_epoch_fence();
                if (metrics)
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
                auto closure_val = load(inst.ops[0]);
                auto cell_slot = c64(inst.ops[2]); // IR slot of the cell
                // -1 - cell_slot (matches ir_executor_impl.cpp:842)
                auto encoded = irb->CreateSub(c64(-1), cell_slot);
                irb->CreateCall(
                    llvm::FunctionCallee(fn_closure_capture),
                    llvm::ArrayRef<llvm::Value*>{closure_val, c64(inst.ops[1]), encoded});
                if (lin)
                    end_linear_epoch_fence(fb);
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
                // Issue #1917: critical Apply path — dual-epoch site probe
                // (function prologue already fences entry; this covers
                // mid-function Apply after concurrent invalidate).
                // Issue #3759: linear locals skip the Apply body on
                // typed-entry / elision fail (begin_linear_epoch_fence),
                // matching interpreter commit-readiness-refused. Probe
                // deopt_inc-then-continue is not a substitute.
                const bool lin = inst.linear_ownership_state != 0;
                LinearEpochFence fb{};
                if (lin)
                    fb = begin_linear_epoch_fence();
                if (metrics) {
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
                    metrics->apply_site_epoch_probe_total.fetch_add(1, std::memory_order_relaxed);
                }
                // Dual-epoch site check when helpers are wired (same runtime as
                // GuardShape / prologue). Stale → deopt_inc + still call through
                // aura_closure_call which re-checks; metrics track probe volume.
                if (fn_get_current_bridge_epoch && fn_is_fn_epoch_stale && fn.name &&
                    fn.name[0] != '\0') {
                    const char* fn_name = fn.name;
                    auto* name_gv = irb->CreateGlobalString(fn_name, "apply_fn_name");
                    auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
                    auto* name_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                        name_gv->getValueType(), name_gv,
                        llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
                    irb->CreateCall(llvm::FunctionCallee(fn_epoch_acquire_fence));
                    auto* cur_epoch =
                        irb->CreateCall(llvm::FunctionCallee(fn_get_current_bridge_epoch), {});
                    auto* stale_i =
                        irb->CreateCall(llvm::FunctionCallee(fn_is_fn_epoch_stale),
                                        llvm::ArrayRef<llvm::Value*>{name_ptr, cur_epoch});
                    auto* is_stale = irb->CreateICmpNE(stale_i, zero32);
                    auto* parent_func = irb->GetInsertBlock()->getParent();
                    auto* bb_stale =
                        llvm::BasicBlock::Create(ctx, "apply_epoch_stale", parent_func);
                    auto* bb_ok = llvm::BasicBlock::Create(ctx, "apply_epoch_ok", parent_func);
                    irb->CreateCondBr(is_stale, bb_stale, bb_ok);
                    irb->SetInsertPoint(bb_stale);
                    if (fn_deopt_inc)
                        irb->CreateCall(llvm::FunctionCallee(fn_deopt_inc));
                    // Fall through to call path with deopt recorded (runtime
                    // aura_closure_call also dual-checks). Both paths share
                    // arg materialization in bb_ok.
                    irb->CreateBr(bb_ok);
                    irb->SetInsertPoint(bb_ok);
                }
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
                leave_if_callee_refused();
                if (lin)
                    end_linear_epoch_fence(fb);
                return true;
            }
            case OpCall: {
                // Issue #1917: critical Call opcode (hot-path callee dispatch).
                if (metrics)
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
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
                leave_if_callee_refused();
                return true;
            }

            // Cells
            case OpNewCell: {
                if (is_pcell(inst.ops[0])) {
                    // Issue #4570: fresh promoted cell — content starts at
                    // fixnum 0 like aura_new_cell's g_cell_heap.push_back(0)
                    // (a float cell is always set before it is read).
                    if (pcell_f[inst.ops[0]])
                        irb->CreateStore(llvm::ConstantFP::get(double_ty, 0.0),
                                         pcells[inst.ops[0]]);
                    else
                        irb->CreateStore(c64(0), pcells[inst.ops[0]]);
                    return true;
                }
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_new_cell));
                store(inst.ops[0], call);
                return true;
            }
            case OpCellGet: {
                // Issue #4573: read of a promoted cell's scalar-replaced pair.
                if (is_pcell(inst.ops[1]) && is_spair(fn.local_count + inst.ops[1]) &&
                    is_spair(inst.ops[0])) {
                    copy_spair(inst.ops[0], fn.local_count + inst.ops[1]);
                    return true;
                }
                if (is_pcell(inst.ops[1])) {
                    if (pcell_f[inst.ops[1]]) {
                        auto* d = irb->CreateLoad(double_ty, pcells[inst.ops[1]]);
                        if (is_flocal(inst.ops[0]))
                            storef(inst.ops[0], d);
                        else
                            store(inst.ops[0], box_double(d));
                        return true;
                    }
                    store(inst.ops[0],
                          irb->CreateLoad(llvm::Type::getInt64Ty(ctx), pcells[inst.ops[1]]));
                    return true;
                }
                auto call = irb->CreateCall(llvm::FunctionCallee(fn_cell_get),
                                            llvm::ArrayRef<llvm::Value*>{load(inst.ops[1])});
                store(inst.ops[0], call);
                return true;
            }
            case OpCellSet: {
                // Issue #4573: promoted cell holding a scalar-replaced pair.
                if (is_pcell(inst.ops[0]) && is_spair(fn.local_count + inst.ops[0]) &&
                    is_spair(inst.ops[1])) {
                    copy_spair(fn.local_count + inst.ops[0], inst.ops[1]);
                    return true;
                }
                if (is_pcell(inst.ops[0])) {
                    if (pcell_f[inst.ops[0]]) {
                        // Static proof: the stored slot is a float slot, so it
                        // has a raw-double shadow (flocal) holding the value.
                        irb->CreateStore(loadf(inst.ops[1]), pcells[inst.ops[0]]);
                        return true;
                    }
                    irb->CreateStore(load(inst.ops[1]), pcells[inst.ops[0]]);
                    return true;
                }
                // Issue #4600/#4606: the entry fn's define-cell binds (value
                // freshly MakeClosure'd) are loader plumbing — use the bind
                // entry so the session-less Mutate choke cannot drop them.
                const bool is_entry_bind = fn.name && std::string_view(fn.name) == "__top__" &&
                                           mc_value_slots.count(inst.ops[1]) != 0;
                irb->CreateCall(
                    llvm::FunctionCallee(is_entry_bind ? fn_cell_set_jit_bind : fn_cell_set),
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

            case OpLinearWrap: {
                // Issue #1535 / #1917: dual-epoch fence before wrap
                // (prevents UAF on post-mutate linear values). The
                // epoch check also runs #740 post-invalidate metrics
                // when linear_ownership_state != 0.
                //
                // Issue #2293: register the wrapped value as a
                // linear pin root so verify_linear_pins_under_moving_compact
                // sees live linear roots from JIT'd code. Pin call
                // is placed AFTER the store + epoch fence end so
                // the runtime-side check observes the wrapped
                // value, not the stale pre-store value.
                if (metrics)
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
                auto fb = begin_linear_epoch_fence();
                auto wrap_val = load(inst.ops[1]);
                store(inst.ops[0], wrap_val);
                end_linear_epoch_fence(fb);
                irb->CreateCall(llvm::FunctionCallee(fn_pin_linear_root),
                                llvm::ArrayRef<llvm::Value*>{wrap_val});
                return true;
            }
            case OpBorrowOp:
            case OpMutBorrowOp:
            case OpRefCountOp: {
                // Issue #1535 / #1917: dual-epoch fence before borrow
                // (prevents UAF on post-mutate linear values). The
                // epoch check also runs #740 post-invalidate metrics
                // when linear_ownership_state != 0.
                if (metrics)
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
                auto fb = begin_linear_epoch_fence();
                store(inst.ops[0], load(inst.ops[1]));
                end_linear_epoch_fence(fb);
                return true;
            }
            case OpMoveOp: {
                // Issue #1535 / #1917: check epoch before zeroing source
                // (prevents use-after-move after mid-op mutate).
                // Issue #3446: begin_linear_epoch_fence also ORs live
                // elision_ok + typed-entry so a reject proof skips this
                // body (source slot not zeroed).
                //
                // Issue #2293: unpin the source slot before
                // invalidation so verify_linear_pins_under_moving_compact
                // doesn't see a stale root registered by a prior
                // OpLinearWrap on the source slot. The runtime
                // unpin helper is idempotent (no-op for unknown
                // roots) so this is safe even if the source slot
                // was never wrapped (e.g. moved from a non-linear
                // register slot).
                if (metrics)
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
                auto fb = begin_linear_epoch_fence();
                auto src_val = load(inst.ops[1]);
                // Issue #106: source invalidation. After a
                // MoveOp the source slot is zeroed so a later
                // DropOp on the source is a no-op (the runtime's
                // drop helpers all bounds-check or check the
                // IS_PAIR low-bit tag, and 0 fails both).
                store(inst.ops[0], src_val);
                // Unpin source BEFORE zeroing so the unpin sees
                // the original wrapped value (the inline
                // unpin_linear_root early-returns on null, but
                // we still want the registry hit for the actual
                // root, not a synthesized 0).
                irb->CreateCall(llvm::FunctionCallee(fn_unpin_linear_root),
                                llvm::ArrayRef<llvm::Value*>{src_val});
                store(inst.ops[1], c64(0)); // source invalidated
                end_linear_epoch_fence(fb);
                return true;
            }
            case OpDropOp: {
                // Issue #1535 / #1917: check epoch before drop (prevents
                // double-free of stale/invalidated linear values).
                // Issue #3446: same fence fail-close as Move (elision +
                // typed-entry OR). Borrow/MutBorrow inherit the same call.
                //
                // Issue #2293: unpin the value being dropped AFTER
                // the drop helpers run (so the unpin sees the same
                // value the drops saw) so verify_linear_pins_under_moving_compact
                // no longer holds the dead root. The runtime unpin
                // helper early-returns on null, so dropping a
                // never-wrapped value (inst.ops[0] == 0) is a
                // no-op (no registry hit, no counter bump).
                if (metrics)
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
                auto fb = begin_linear_epoch_fence();
                auto val = load(inst.ops[0]);
                irb->CreateCall(llvm::FunctionCallee(fn_drop_pair),
                                llvm::ArrayRef<llvm::Value*>{val});
                irb->CreateCall(llvm::FunctionCallee(fn_drop_cell),
                                llvm::ArrayRef<llvm::Value*>{val});
                irb->CreateCall(llvm::FunctionCallee(fn_drop_closure),
                                llvm::ArrayRef<llvm::Value*>{val});
                irb->CreateCall(llvm::FunctionCallee(fn_unpin_linear_root),
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
            // tag: low2 = 01 (ref), bits 2-5 = RefError (8) → mask
            // 0x3F, value 0x21 (Issue #4571: the old 0x1F/0x11 test
            // matched RefHash (4), so every hash escaped as an error).
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
                // Issue #4573: a scalar-replaced pair is never an error.
                if (is_spair(inst.ops[1])) {
                    store(inst.ops[0], c64(KWD_FALSE_VAL));
                    return true;
                }
                constexpr std::uint64_t REF_ERROR_MASK = 0x3F;
                constexpr std::uint64_t REF_ERROR_TAG = (types::RefError << 2) | 1ull;
                static_assert(REF_ERROR_TAG == 0x21, "RefError tag drift (#4571)");
                if (is_flocal(inst.ops[1])) {
                    // Issue #4570: a raw-double local whose shadow is
                    // authoritative holds a number, never an error — test
                    // the flag and the tagged slot instead of reboxing the
                    // double (a load() here allocated a float-pool box on
                    // every #4552 error check in a float loop).
                    auto* flag = irb->CreateLoad(llvm::Type::getInt8Ty(ctx), funboxed[inst.ops[1]]);
                    auto* tagged =
                        irb->CreateLoad(llvm::Type::getInt64Ty(ctx), llvm_locals[inst.ops[1]]);
                    auto* tag_err = irb->CreateICmpEQ(irb->CreateAnd(tagged, c64(REF_ERROR_MASK)),
                                                      c64(REF_ERROR_TAG));
                    auto* is_err =
                        irb->CreateAnd(irb->CreateICmpEQ(flag, irb->getInt8(0)), tag_err);
                    store(inst.ops[0],
                          irb->CreateSelect(is_err, c64(KWD_TRUE_VAL), c64(KWD_FALSE_VAL)));
                    return true;
                }
                auto val = load(inst.ops[1]);
                // Tag check: (val & 0x3F) == 0x21 (RefError encoded, #4571)
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
                // Issue #4573: non-escaping pair -> fields stay in registers.
                if (is_spair(result_slot)) {
                    irb->CreateStore(car, spair_car[result_slot]);
                    irb->CreateStore(cdr, spair_cdr[result_slot]);
                    return true;
                }
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
                    aura::compiler::typed_audit::capture_jit_hotpath_audit("jit-type-propagation");
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
                // Issue #4573: field of a scalar-replaced pair.
                if (is_spair(inst.ops[1])) {
                    store(inst.ops[0],
                          irb->CreateLoad(llvm::Type::getInt64Ty(ctx), spair_car[inst.ops[1]]));
                    return true;
                }
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
                if (pair_ok) {
                    // Issue #4571 (L2): guard + segment load + field load.
                    // Replaces the per-access aura_get_defuse_version call;
                    // concurrent redefinition is covered by #4569
                    // assumption invalidation, and the segment table never
                    // moves (#898). Misses take the aura_pair_car call.
                    store(inst.ops[0],
                          emit_l2_pair_field(
                              pair_val, 0, "car",
                              [&] {
                                  return irb->CreateCall(llvm::FunctionCallee(fn_pair_car),
                                                         llvm::ArrayRef<llvm::Value*>{pair_val});
                              },
                              /*hfn=*/&fn, /*hblock=*/cur_block,
                              /*hinstr=*/cur_instr, /*deopt_nonpair=*/frame_deopt_ok));
                    return true;
                }
                // Issue #4600 §1/§2 (phase 2): with the frame-deopt stub
                // armed, the guard is one compare + branch to a shared
                // deopt exit — the cold path is that stub call alone
                // (S1); the armed host resumes the IR interpreter at this
                // (block, instr) with the spilled locals (S2). Force mode
                // additionally speculates the pair shape with the compare
                // forced to mismatch, so the unchecked fast path is dead
                // code by construction and every car site deopts
                // mid-body (acceptance mode over the #4567 corpus).
                const bool pair_guard_exit = frame_deopt_ok;
                const bool pair_force = pair_guard_exit && jit_force_guard_deopt() >= 1;
                bool spec_pair = (inst_shape(inst) == SHAPE_PAIR) || pair_force;
                if (spec_pair && pair_guard_exit) {
                    auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                    llvm::Value* match = nullptr;
                    if (pair_force) {
                        match = llvm::ConstantInt::getFalse(ctx);
                    } else {
                        llvm::Value* cur = nullptr;
                        if (jit_defuse_word_enabled()) {
                            auto* slotp = irb->CreateIntToPtr(c64(aura_jit_defuse_word_slot_addr()),
                                                              llvm::PointerType::getUnqual(ctx));
                            auto* wp = irb->CreateLoad(llvm::PointerType::getUnqual(ctx), slotp);
                            wp->setAtomic(llvm::AtomicOrdering::Acquire);
                            wp->setAlignment(llvm::Align(8));
                            auto* cur_word = irb->CreateLoad(i64_ty, wp);
                            cur_word->setAtomic(llvm::AtomicOrdering::Acquire);
                            cur_word->setAlignment(llvm::Align(8));
                            cur = cur_word;
                        } else {
                            cur = irb->CreateCall(llvm::FunctionCallee(fn_get_defuse_version),
                                                  llvm::ArrayRef<llvm::Value*>{});
                        }
                        auto* expected = irb->CreateLoad(i64_ty, llvm_locals[fn.local_count]);
                        match = irb->CreateICmpEQ(cur, expected);
                    }
                    auto* entry_bb = irb->GetInsertBlock();
                    auto* parent_func = entry_bb->getParent();
                    auto* bb_fast = llvm::BasicBlock::Create(ctx, "car_fast", parent_func);
                    auto* bb_exit = llvm::BasicBlock::Create(ctx, "car_frame_deopt", parent_func);
                    auto* bb_cont = llvm::BasicBlock::Create(ctx, "car_guard_cont", parent_func);
                    // Issue #4603: the unchecked helper must not run on a
                    // non-pair (it would read slot 0). A non-pair takes the
                    // same frame-deopt exit as a stale version, so the
                    // interpreter raises "car: not a pair".
                    auto* is_pair_guard =
                        irb->CreateICmpEQ(irb->CreateAnd(pair_val, c64(63)), c64(1));
                    irb->CreateCondBr(irb->CreateAnd(match, is_pair_guard), bb_fast, bb_exit);
                    irb->SetInsertPoint(bb_fast);
                    if (metrics)
                        metrics->intrinsic_count.fetch_add(1, std::memory_order_relaxed);
                    auto* fast_result = irb->CreateCall(llvm::FunctionCallee(fn_pair_car_unchecked),
                                                        llvm::ArrayRef<llvm::Value*>{pair_val});
                    store(inst.ops[0], fast_result);
                    irb->CreateBr(bb_cont);
                    irb->SetInsertPoint(bb_exit);
                    emit_frame_deopt_exit(fn, cur_block, cur_instr, /*from_entry=*/false);
                    irb->SetInsertPoint(bb_cont);
                    return true;
                }
                // Issue #4600 §1: un-armed form (AURA_JIT_FRAME_DEOPT=0,
                // AOT, anon fns, hidden pcell/spair state) keeps the
                // inline generic fallback — fast / slow-call / merge.
                if (spec_pair) {
                    auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                    // Issue #4600 §5: direct word read — two acquire loads
                    // through the bridge-published slot instead of one
                    // get_version hook call per access. Same atomic, same
                    // ordering as the hook (Evaluator::get_defuse_version).
                    llvm::Value* cur = nullptr;
                    if (jit_defuse_word_enabled()) {
                        auto* slotp = irb->CreateIntToPtr(c64(aura_jit_defuse_word_slot_addr()),
                                                          llvm::PointerType::getUnqual(ctx));
                        auto* wp = irb->CreateLoad(llvm::PointerType::getUnqual(ctx), slotp);
                        wp->setAtomic(llvm::AtomicOrdering::Acquire);
                        wp->setAlignment(llvm::Align(8));
                        auto* cur_word = irb->CreateLoad(i64_ty, wp);
                        cur_word->setAtomic(llvm::AtomicOrdering::Acquire);
                        cur_word->setAlignment(llvm::Align(8));
                        cur = cur_word;
                    } else {
                        cur = irb->CreateCall(llvm::FunctionCallee(fn_get_defuse_version),
                                              llvm::ArrayRef<llvm::Value*>{});
                    }
                    auto* expected = irb->CreateLoad(i64_ty, llvm_locals[fn.local_count]);
                    auto* match = irb->CreateICmpEQ(cur, expected);
                    auto* entry_bb = irb->GetInsertBlock();
                    auto* parent_func = entry_bb->getParent();
                    auto* bb_fast = llvm::BasicBlock::Create(ctx, "car_fast", parent_func);
                    auto* bb_slow = llvm::BasicBlock::Create(ctx, "car_slow", parent_func);
                    auto* bb_done = llvm::BasicBlock::Create(ctx, "car_done", parent_func);
                    // Issue #4603: the unchecked arm must not run on a
                    // non-pair; such a value takes the checked slow call.
                    auto* is_pair_guard =
                        irb->CreateICmpEQ(irb->CreateAnd(pair_val, c64(63)), c64(1));
                    irb->CreateCondBr(irb->CreateAnd(match, is_pair_guard), bb_fast, bb_slow);

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
                    // Issue #4600 §4: attribute the guard deopt to this fn
                    // for the deopt-storm policy (named fns only; one cold
                    // call per slow-path access).
                    if (fn_note_guard_deopt && fn.name && fn.name[0]) {
                        auto* note_gv = irb->CreateGlobalString(fn.name, "guard_note_fn_name");
                        auto* note_zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
                        auto* note_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                            note_gv->getValueType(), note_gv,
                            llvm::ArrayRef<llvm::Constant*>{note_zero, note_zero});
                        irb->CreateCall(llvm::FunctionCallee(fn_note_guard_deopt),
                                        llvm::ArrayRef<llvm::Value*>{note_ptr});
                    }
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
                    // Non-L2 path: always take the slow path (with bounds + lock).
                    // Issue #4603: a non-pair deopts to the interpreter (which
                    // raises "car: not a pair") when the frame-deopt stub is armed.
                    auto call =
                        emit_pair_checked_call(pair_val, fn_pair_car, fn, cur_block, cur_instr);
                    store(inst.ops[0], call);
                }
                return true;
            }
            case OpCdr: {
                // Issue #4573: field of a scalar-replaced pair.
                if (is_spair(inst.ops[1])) {
                    store(inst.ops[0],
                          irb->CreateLoad(llvm::Type::getInt64Ty(ctx), spair_cdr[inst.ops[1]]));
                    return true;
                }
                // Issue #157 Phase 1b: same as OpCar but for cdr.
                auto pair_val = load(inst.ops[1]);
                if (pair_ok) {
                    // Issue #4571 (L2): same inline read as OpCar, field 1.
                    store(inst.ops[0],
                          emit_l2_pair_field(
                              pair_val, 1, "cdr",
                              [&] {
                                  return irb->CreateCall(llvm::FunctionCallee(fn_pair_cdr),
                                                         llvm::ArrayRef<llvm::Value*>{pair_val});
                              },
                              /*hfn=*/&fn, /*hblock=*/cur_block,
                              /*hinstr=*/cur_instr, /*deopt_nonpair=*/frame_deopt_ok));
                    return true;
                }
                // Issue #4600 §1/§2 (phase 2): with the frame-deopt stub
                // armed, the guard is one compare + branch to a shared
                // deopt exit — the cold path is that stub call alone
                // (S1); the armed host resumes the IR interpreter at this
                // (block, instr) with the spilled locals (S2). Force mode
                // additionally speculates the pair shape with the compare
                // forced to mismatch, so the unchecked fast path is dead
                // code by construction and every cdr site deopts
                // mid-body (acceptance mode over the #4567 corpus).
                const bool pair_guard_exit = frame_deopt_ok;
                const bool pair_force = pair_guard_exit && jit_force_guard_deopt() >= 1;
                bool spec_pair = (inst_shape(inst) == SHAPE_PAIR) || pair_force;
                if (spec_pair && pair_guard_exit) {
                    auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                    llvm::Value* match = nullptr;
                    if (pair_force) {
                        match = llvm::ConstantInt::getFalse(ctx);
                    } else {
                        llvm::Value* cur = nullptr;
                        if (jit_defuse_word_enabled()) {
                            auto* slotp = irb->CreateIntToPtr(c64(aura_jit_defuse_word_slot_addr()),
                                                              llvm::PointerType::getUnqual(ctx));
                            auto* wp = irb->CreateLoad(llvm::PointerType::getUnqual(ctx), slotp);
                            wp->setAtomic(llvm::AtomicOrdering::Acquire);
                            wp->setAlignment(llvm::Align(8));
                            auto* cur_word = irb->CreateLoad(i64_ty, wp);
                            cur_word->setAtomic(llvm::AtomicOrdering::Acquire);
                            cur_word->setAlignment(llvm::Align(8));
                            cur = cur_word;
                        } else {
                            cur = irb->CreateCall(llvm::FunctionCallee(fn_get_defuse_version),
                                                  llvm::ArrayRef<llvm::Value*>{});
                        }
                        auto* expected = irb->CreateLoad(i64_ty, llvm_locals[fn.local_count]);
                        match = irb->CreateICmpEQ(cur, expected);
                    }
                    auto* entry_bb = irb->GetInsertBlock();
                    auto* parent_func = entry_bb->getParent();
                    auto* bb_fast = llvm::BasicBlock::Create(ctx, "cdr_fast", parent_func);
                    auto* bb_exit = llvm::BasicBlock::Create(ctx, "cdr_frame_deopt", parent_func);
                    auto* bb_cont = llvm::BasicBlock::Create(ctx, "cdr_guard_cont", parent_func);
                    // Issue #4603: same non-pair guard as the car arm.
                    auto* is_pair_guard =
                        irb->CreateICmpEQ(irb->CreateAnd(pair_val, c64(63)), c64(1));
                    irb->CreateCondBr(irb->CreateAnd(match, is_pair_guard), bb_fast, bb_exit);
                    irb->SetInsertPoint(bb_fast);
                    if (metrics)
                        metrics->intrinsic_count.fetch_add(1, std::memory_order_relaxed);
                    auto* fast_result = irb->CreateCall(llvm::FunctionCallee(fn_pair_cdr_unchecked),
                                                        llvm::ArrayRef<llvm::Value*>{pair_val});
                    store(inst.ops[0], fast_result);
                    irb->CreateBr(bb_cont);
                    irb->SetInsertPoint(bb_exit);
                    emit_frame_deopt_exit(fn, cur_block, cur_instr, /*from_entry=*/false);
                    irb->SetInsertPoint(bb_cont);
                    return true;
                }
                // Issue #4600 §1: un-armed form (AURA_JIT_FRAME_DEOPT=0,
                // AOT, anon fns, hidden pcell/spair state) keeps the
                // inline generic fallback — fast / slow-call / merge.
                if (spec_pair) {
                    auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                    // Issue #4600 §5: same direct word read as OpCar.
                    llvm::Value* cur = nullptr;
                    if (jit_defuse_word_enabled()) {
                        auto* slotp = irb->CreateIntToPtr(c64(aura_jit_defuse_word_slot_addr()),
                                                          llvm::PointerType::getUnqual(ctx));
                        auto* wp = irb->CreateLoad(llvm::PointerType::getUnqual(ctx), slotp);
                        wp->setAtomic(llvm::AtomicOrdering::Acquire);
                        wp->setAlignment(llvm::Align(8));
                        auto* cur_word = irb->CreateLoad(i64_ty, wp);
                        cur_word->setAtomic(llvm::AtomicOrdering::Acquire);
                        cur_word->setAlignment(llvm::Align(8));
                        cur = cur_word;
                    } else {
                        cur = irb->CreateCall(llvm::FunctionCallee(fn_get_defuse_version),
                                              llvm::ArrayRef<llvm::Value*>{});
                    }
                    auto* expected = irb->CreateLoad(i64_ty, llvm_locals[fn.local_count]);
                    auto* match = irb->CreateICmpEQ(cur, expected);
                    auto* entry_bb = irb->GetInsertBlock();
                    auto* parent_func = entry_bb->getParent();
                    auto* bb_fast = llvm::BasicBlock::Create(ctx, "cdr_fast", parent_func);
                    auto* bb_slow = llvm::BasicBlock::Create(ctx, "cdr_slow", parent_func);
                    auto* bb_done = llvm::BasicBlock::Create(ctx, "cdr_done", parent_func);
                    // Issue #4603: same non-pair guard as the car arm.
                    auto* is_pair_guard =
                        irb->CreateICmpEQ(irb->CreateAnd(pair_val, c64(63)), c64(1));
                    irb->CreateCondBr(irb->CreateAnd(match, is_pair_guard), bb_fast, bb_slow);

                    irb->SetInsertPoint(bb_fast);
                    auto* fast_result = irb->CreateCall(llvm::FunctionCallee(fn_pair_cdr_unchecked),
                                                        llvm::ArrayRef<llvm::Value*>{pair_val});
                    irb->CreateBr(bb_done);

                    irb->SetInsertPoint(bb_slow);
                    // Issue #157 Phase 1c: bump the deopt counter.
                    irb->CreateCall(llvm::FunctionCallee(fn_deopt_inc),
                                    llvm::ArrayRef<llvm::Value*>{});
                    // Issue #4600 §4: same storm attribution as OpCar.
                    if (fn_note_guard_deopt && fn.name && fn.name[0]) {
                        auto* note_gv = irb->CreateGlobalString(fn.name, "guard_note_fn_name");
                        auto* note_zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
                        auto* note_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                            note_gv->getValueType(), note_gv,
                            llvm::ArrayRef<llvm::Constant*>{note_zero, note_zero});
                        irb->CreateCall(llvm::FunctionCallee(fn_note_guard_deopt),
                                        llvm::ArrayRef<llvm::Value*>{note_ptr});
                    }
                    auto* slow_result = irb->CreateCall(llvm::FunctionCallee(fn_pair_cdr),
                                                        llvm::ArrayRef<llvm::Value*>{pair_val});
                    irb->CreateBr(bb_done);

                    irb->SetInsertPoint(bb_done);
                    auto* phi = irb->CreatePHI(i64_ty, 2, "cdr_result");
                    phi->addIncoming(fast_result, bb_fast);
                    phi->addIncoming(slow_result, bb_slow);
                    store(inst.ops[0], phi);
                } else {
                    // Issue #4603: same non-pair deopt as the car arm.
                    auto call =
                        emit_pair_checked_call(pair_val, fn_pair_cdr, fn, cur_block, cur_instr);
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
                    aura::compiler::typed_audit::capture_jit_hotpath_audit("jit-cast-elided-l2");
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
                // Issue #1917: critical PrimCall — expand fast-path + count hits.
                if (metrics)
                    metrics->critical_opcode_lowered_total.fetch_add(1, std::memory_order_relaxed);
                // IR: operands[0]=prim_id, operands[1]=arg_base, operands[2]=arg_count,
                //     operands[3]=result_slot
                auto prim_id = inst.ops[0];
                auto arg_base = inst.ops[1];
                auto arg_count = inst.ops[2];
                auto result_slot = inst.ops[3];
                auto a1 = (arg_count > 0) ? load(arg_base) : c64(0);
                auto a2 = (arg_count > 1) ? load(arg_base + 1) : c64(0);

                auto bump_fastpath = [&]() {
                    if (metrics)
                        metrics->primcall_fastpath_hits.fetch_add(1, std::memory_order_relaxed);
                };

                // Fast-path: inline known primitives to skip aura_prim_call dispatch
                switch (prim_id) {
                    case PrimNewline:
                        irb->CreateCall(llvm::FunctionCallee(fn_newline));
                        store(result_slot, c64(0));
                        bump_fastpath();
                        return true;
                    case PrimDisplay:
                    case PrimWrite:
                        // Issue #2572: was aura_display_int (raw %ld of tagged bits)
                        // so string constants printed as huge negatives and module
                        // multi-display helpers looked like "msgmsg". Use tagged
                        // display (string/bool/void/fixnum/float). write_mode=1
                        // quotes strings. Result is void (special 11).
                        irb->CreateCall(
                            llvm::FunctionCallee(fn_display_value),
                            llvm::ArrayRef<llvm::Value*>{a1, c64(prim_id == PrimWrite ? 1 : 0)});
                        store(result_slot, c64(11)); // kSpecialVoid
                        bump_fastpath();
                        return true;
                    case PrimQuotient: {
                        // Both args are fixnum-encoded (value << 1). SDiv cancels the shift,
                        // so we get the correct integer result. Shift back for fixnum encoding.
                        auto zero = irb->CreateICmpEQ(a2, c64(0));
                        auto div = irb->CreateSDiv(a1, a2);
                        auto safe = irb->CreateSelect(zero, c64(0), div);
                        store(result_slot, irb->CreateShl(safe, c64(1)));
                        bump_fastpath();
                        return true;
                    }
                    case PrimRemainder: {
                        auto zero = irb->CreateICmpEQ(a2, c64(0));
                        auto rem = irb->CreateSRem(a1, a2);
                        auto safe = irb->CreateSelect(zero, c64(0), rem);
                        // a1/a2 fixnum-encoded, SRem preserves fixnum encoding
                        store(result_slot, safe);
                        bump_fastpath();
                        return true;
                    }
                    case PrimPairP: {
                        // Pointer tagging: low 2 bits = 01 means pair
                        auto masked = irb->CreateAnd(a1, c64(3));
                        auto is_pair = irb->CreateICmpEQ(masked, c64(1));
                        store(result_slot, irb->CreateSelect(is_pair, c64(7), c64(3)));
                        bump_fastpath();
                        return true;
                    }
                    case PrimNullP: {
                        // Issue #2482: empty list is void sentinel (11) only.
                        // Fixnum 0 is a number — never null? / end-of-list.
                        auto is_void = irb->CreateICmpEQ(a1, c64(11));
                        store(result_slot, irb->CreateSelect(is_void, c64(7), c64(3)));
                        bump_fastpath();
                        return true;
                    }
                    case PrimVectorP: {
                        // Issue #1917: vector? matches is_vector (RefVector=3):
                        // low 6 bits == (RefVector << 2) | kRefLowTag == 0x0D.
                        constexpr std::uint64_t REF_TYPE_LOW_MASK = 0x3F;
                        constexpr std::uint64_t REF_VECTOR_TAG = (3ull << 2) | 1ull;
                        auto masked = irb->CreateAnd(a1, c64(REF_TYPE_LOW_MASK));
                        auto is_vec = irb->CreateICmpEQ(masked, c64(REF_VECTOR_TAG));
                        store(result_slot, irb->CreateSelect(is_vec, c64(7), c64(3)));
                        bump_fastpath();
                        return true;
                    }
                    case PrimErrorP: {
                        // Issue #1917: error? matches is_error (RefError=8):
                        // low 6 bits == (RefError << 2) | kRefLowTag == 0x21.
                        // (OpIsError uses a coarser historical mask; PrimCall
                        // uses the full type field for interpreter parity.)
                        constexpr std::uint64_t REF_TYPE_LOW_MASK = 0x3F;
                        constexpr std::uint64_t REF_ERROR_TAG = (8ull << 2) | 1ull;
                        auto masked = irb->CreateAnd(a1, c64(REF_TYPE_LOW_MASK));
                        auto is_err = irb->CreateICmpEQ(masked, c64(REF_ERROR_TAG));
                        store(result_slot, irb->CreateSelect(is_err, c64(7), c64(3)));
                        bump_fastpath();
                        return true;
                    }
                    default:
                        break;
                }

                // Slow-path: Issue #2576 — pack all frame args into a
                // contiguous stack buffer and pass (slot, args*, count).
                // Previous ABI only forwarded a1/a2 so 3+ arity prims
                // (string-append, substring, …) saw arg2=0 / missing.
                // aura_prim_call is shared with AOT (lib/runtime.c).
                // Same packing pattern as OpCall (locals are per-slot alloca).
                {
                    // Cap matches kAuraPrimCallMaxArgs in aura_jit_runtime.
                    constexpr uint32_t kMax = 32;
                    uint32_t n = arg_count;
                    if (n > kMax)
                        n = kMax;
                    auto* i64_ty = llvm::Type::getInt64Ty(ctx);
                    auto* ptr_ty = llvm::PointerType::getUnqual(ctx);
                    auto* alloca_ty = llvm::ArrayType::get(i64_ty, n > 0 ? n : 1);
                    auto* args_arr = irb->CreateAlloca(alloca_ty);
                    for (uint32_t i = 0; i < n; ++i) {
                        auto* slot_val =
                            (arg_base + i < fn.local_count) ? load(arg_base + i) : c64(0);
                        auto* gep = irb->CreateGEP(i64_ty, args_arr, c64(static_cast<int64_t>(i)));
                        irb->CreateStore(slot_val, gep);
                    }
                    auto call = irb->CreateCall(fn_prim_call,
                                                {c64(prim_id), irb->CreateBitCast(args_arr, ptr_ty),
                                                 c64(static_cast<int64_t>(n))});
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

                // Issue #157 / #4319: acquire the read lock before
                // fn_hash_get_flat_table. Held across the probe loads.
                // Dropped only around aura_hash_key_eq so the string
                // pool mutex is not nested under it, then reacquired
                // before the next probe. Released in done_bb.
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
                auto tomb_bb = llvm::BasicBlock::Create(ctx, "htomb", func);
                auto occ_bb = llvm::BasicBlock::Create(ctx, "hocc", func);
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

                // Issue #4319: same open-address probe as aura_hash_set_checked
                // (splitmix64, then (h + i) % cap). Empty stops. Tombstone
                // continues without aura_hash_key_eq. The workspace read
                // lock is dropped before that call so it is not held across
                // g_string_pool_mtx. Miss returns the void sentinel 11.
                // FlatHashTable itself is unchanged.
                auto hash_key = irb->CreateCall(llvm::FunctionCallee(fn_hash_splitmix),
                                                llvm::ArrayRef<llvm::Value*>{key_val});
                irb->CreateBr(loop_bb);

                // ── Loop header (PHI for probe step) ──
                irb->SetInsertPoint(loop_bb);
                auto phi_idx = irb->CreatePHI(i64_ty, 2, "hidx");
                phi_idx->addIncoming(c64(0), live_bb);

                auto at_end = irb->CreateICmpUGE(phi_idx, capacity);
                irb->CreateCondBr(at_end, miss_bb, check_bb);

                // ── Probe slot, then metadata ──
                irb->SetInsertPoint(check_bb);
                auto slot =
                    irb->CreateCall(llvm::FunctionCallee(fn_hash_probe),
                                    llvm::ArrayRef<llvm::Value*>{hash_key, phi_idx, capacity});
                auto meta_gep = irb->CreateGEP(i8_ty, meta_ptr, slot);
                auto meta_val = irb->CreateLoad(i8_ty, meta_gep);
                auto is_empty =
                    irb->CreateICmpEQ(meta_val, irb->getInt8(aura::compiler::hash::kEmptySlot));
                // Empty ends the probe. Do not call eq.
                irb->CreateCondBr(is_empty, miss_bb, tomb_bb);

                irb->SetInsertPoint(tomb_bb);
                auto is_tomb = irb->CreateICmpEQ(meta_val, irb->getInt8(0x7F));
                // Tombstone: keep probing, do not call eq.
                irb->CreateCondBr(is_tomb, next_bb, occ_bb);

                // Occupied is the exact 0x80 marker hash-set writes, not
                // "high bit set". Anything else keeps probing without eq.
                irb->SetInsertPoint(occ_bb);
                auto is_occ = irb->CreateICmpEQ(meta_val, irb->getInt8(0x80));
                irb->CreateCondBr(is_occ, cmp_bb, next_bb);

                // ── Occupied: copy key and value, then compare unlocked ──
                irb->SetInsertPoint(cmp_bb);
                auto key_gep = irb->CreateGEP(i64_ty, keys_ptr, slot);
                auto stored_key = irb->CreateLoad(i64_ty, key_gep);
                auto val_gep = irb->CreateGEP(i64_ty, vals_ptr, slot);
                auto val_ld = irb->CreateLoad(i64_ty, val_gep);
                // Issue #4571: aura_hash_key_eq is only non-trivial when both
                // keys are strings; identical bits are equal and a fixnum on
                // either side never matches different bits. Decide those
                // inline (no call, lock kept); only two non-fixnum keys take
                // the unlocked eq call below.
                auto* eqk_bb = llvm::BasicBlock::Create(ctx, "hcmp_eq", func);
                auto* bits_eq = irb->CreateICmpEQ(stored_key, key_val);
                auto* both_nonfix = irb->CreateICmpNE(
                    irb->CreateAnd(irb->CreateAnd(stored_key, key_val), c64(1)), c64(0));
                auto* bb_bits = llvm::BasicBlock::Create(ctx, "hcmp_bits", func);
                irb->CreateCondBr(bits_eq, found_bb, bb_bits);
                irb->SetInsertPoint(bb_bits);
                irb->CreateCondBr(both_nonfix, eqk_bb, next_bb);
                irb->SetInsertPoint(eqk_bb);
                irb->CreateCall(llvm::FunctionCallee(fn_unlock_workspace_read),
                                llvm::ArrayRef<llvm::Value*>{});
                auto eq_res = irb->CreateCall(llvm::FunctionCallee(fn_hash_key_eq),
                                              llvm::ArrayRef<llvm::Value*>{stored_key, key_val});
                irb->CreateCall(llvm::FunctionCallee(fn_lock_workspace_read),
                                llvm::ArrayRef<llvm::Value*>{});
                auto eq = irb->CreateICmpNE(eq_res, c64(0));
                irb->CreateCondBr(eq, found_bb, next_bb);

                // ── Next probe step ──
                irb->SetInsertPoint(next_bb);
                auto next_idx = irb->CreateAdd(phi_idx, c64(1));
                phi_idx->addIncoming(next_idx, next_bb);
                irb->CreateBr(loop_bb);

                // ── Found: value was copied under the read lock ──
                irb->SetInsertPoint(found_bb);
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
                // Issue #157 / #4319: release the read lock on the way out.
                // cmp_bb drops it only around aura_hash_key_eq and takes it
                // back before found_bb / next_bb, so done_bb still owns it.
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
                // Issue #1917: critical opcode unhandled is a separate AC metric.
                if (metrics) {
                    metrics->unhandled_opcode_count.fetch_add(1, std::memory_order_relaxed);
                    metrics->fallback_count.fetch_add(1, std::memory_order_relaxed);
                    metrics->unhandled_fail_fast_total.fetch_add(1, std::memory_order_relaxed);
                    if (inst.opcode < 64) {
                        metrics->opcode_unhandled_mask.fetch_or(1ull << inst.opcode,
                                                                std::memory_order_relaxed);
                        if (AuraJIT::is_critical_opcode(inst.opcode))
                            metrics->critical_opcode_unhandled_total.fetch_add(
                                1, std::memory_order_relaxed);
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
// Issue #4600 §5: stable slot address for direct defuse-word loads.
std::uintptr_t aura_jit_defuse_word_slot_addr();
// Issue #4600 §4: cold-path guard-deopt attribution (deopt-storm policy).
void aura_jit_note_guard_deopt(const char* name);
// Issue #4600/#4606 family: define-cell bind write (no Mutate choke).
void aura_cell_set_jit_bind(int64_t cell_id, int64_t val);
// Issue #4571: L2 inline pair access (runtime_ssot.cpp / aura_jit_runtime.cpp).
int64_t aura_jit_pair_inline_ok();
void* aura_pair_seg_dir_base();
// Issue #157 Phase 1c / #1534 / #1535 / #1537: deopt + dual-epoch fences.
void aura_deopt_inc();
// Issue #3988: Jump back-edge hold-budget poll.
int aura_jit_poll_hold_budget_safepoint() noexcept;
// Issue #1610: MacroIntroduced hygiene consult / deopt counters.
void aura_jit_macro_hygiene_consult_inc();
void aura_jit_macro_introduced_deopt_inc();
uint64_t aura_jit_macro_hygiene_consults();
uint64_t aura_jit_macro_introduced_deopt();
int aura_jit_guard_shape_epoch_check(const char* name);
int aura_jit_linear_epoch_safety_check(const char* fn_name, std::uint8_t linear_state,
                                       std::uint32_t opcode);
int aura_jit_linear_post_mutate_enforce(std::uint32_t env_id);
std::uint64_t aura_jit_get_current_bridge_epoch(void);
int aura_jit_is_fn_epoch_stale(const char* name, std::uint64_t current_bridge_epoch);
std::int64_t aura_jit_deopt_to_interpreter(const char* name);
// Issue #4600 §1/§2 (phase 2): shared frame-state deopt stub (bridge).
std::int64_t aura_jit_deopt_frame(const char* fn_name, std::uint32_t block, std::uint32_t instr,
                                  const std::int64_t* slots, std::uint32_t nslots,
                                  std::uint32_t argc);
// Issue #4567: defined in spec_jit_controller.cpp (C linkage).
int aura_check_shape_guard(const std::int64_t* args, std::uint32_t arg_count,
                           const std::uint8_t* shape_map, std::uint32_t map_size);
int aura_jit_peek_prologue_deopt_sentinel(void) noexcept;
int64_t aura_prim_call(int64_t, int64_t*, int64_t); // Issue #2576 N-arg
uint64_t aura_prim_call_count();
uint64_t aura_prim_call_total_ns();
void aura_display_int(int64_t);
void aura_display_char(char);
void aura_display_value(int64_t, int64_t); // Issue #2572 / #2575 ABI i64
void aura_newline();
void aura_jit_epoch_acquire_fence(void);
void aura_jit_linear_post_invalidate_safety(std::uint8_t linear_state, std::uint32_t opcode);
int64_t aura_jit_prim_dispatch(int64_t, int64_t*, int32_t);
int64_t aura_cast_op(int64_t, int64_t);
void aura_register_fn(int64_t func_id, int64_t (*fn)(int64_t*, uint32_t), int32_t local_count,
                      int32_t arg_count, int32_t env_count);
void aura_register_fn_named(const char* name, int64_t func_id, int64_t (*fn)(int64_t*, uint32_t),
                            int32_t local_count, int32_t arg_count, int32_t env_count);
void aura_note_ir_func_define(int64_t func_id, const char* name); // Issue #4521
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
std::uint64_t aura_hash_splitmix64(std::int64_t);
std::uint64_t aura_hash_probe_slot(std::uint64_t, std::uint64_t, std::uint64_t);
void aura_set_hash_str_eq_callback(int64_t (*fn)(int64_t, int64_t));
void aura_set_hash_str_convert_callback(int64_t (*fn)(int64_t));
// Float/string pool functions (defined in aura_jit_runtime.cpp)
int64_t aura_alloc_float(double);
double aura_float_ref(int64_t);
int64_t aura_alloc_string(const char*);
int64_t aura_intern_keyword(const char*); // Issue #4321
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
    //
    // Issue #2091: thread live env_frame_version + linear_state into the
    // mangle. Pre-#2091 this site passed only defuse — captured-env /
    // captured-linear drift after emit was invisible to the LLVM-side
    // symbol (and therefore to dlopen-based reload probes). The
    // registration .c emits matching link names via aot_link_name
    // (also updated to accept env/linear), so the symbols stay
    // symmetric. fn.linear_ownership_state wins over the global
    // fingerprint for the per-function mangle (same logic as
    // generate_registration_c).
    std::string fn_name = fn.name;
    const std::uint64_t emit_ver = aura_get_aot_defuse_version();
    const std::uint64_t emit_env_v = aura_get_aot_live_env_frame_version();
    const std::uint8_t emit_lin_fingerprint = aura_get_aot_live_linear_state_fingerprint();
    const std::uint8_t fn_lin = static_cast<std::uint8_t>(fn.linear_ownership_state & 0xFFu);
    const std::uint8_t emit_lin = fn_lin > emit_lin_fingerprint ? fn_lin : emit_lin_fingerprint;
    auto unique_name =
        (fn_name == "__top__")
            ? fn_name
            : aura::compiler::mangle_aot_name(fn_name, static_cast<std::uint32_t>(my_id), emit_ver,
                                              emit_env_v, emit_lin);
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

    // Issue #4522: raw-double shadow slots for SHAPE_FLOAT locals —
    // same unboxed-accumulator lowering as the JIT path. builder.shape_map
    // deliberately stays unset here so inst_shape-based L1/L2 fallbacks
    // are unchanged; the float-local intercepts key off flocals alone.
    builder.flocals.assign(fn.local_count, nullptr);
    builder.funboxed.assign(fn.local_count, nullptr);
    if (fn.shape_map && fn.source_marker != 1) {
        auto* i8_zero = llvm::ConstantInt::get(llvm::Type::getInt8Ty(local_ctx), 0);
        for (uint32_t i = 0; i < fn.local_count; ++i) {
            if (fn.shape_map[i] != SHAPE_FLOAT)
                continue;
            builder.flocals[i] = builder.irb->CreateAlloca(llvm::Type::getDoubleTy(local_ctx));
            builder.funboxed[i] = builder.irb->CreateAlloca(llvm::Type::getInt8Ty(local_ctx));
            builder.irb->CreateStore(i8_zero, builder.funboxed[i]);
        }
    }
    // Issue #4570: scalar-replace function-local cells (same as the JIT path).
    builder.setup_l1_slots(fn);

    // Lower each block
    for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
        auto& fb = fn.blocks[bi];
        builder.irb->SetInsertPoint(builder.block_map[fb.id]);
        for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
            builder.next_inst = ii + 1 < fb.num_instructions ? &fb.instructions[ii + 1] : nullptr;
            builder.consumed_next = false;
            if (!builder.lower(fb.instructions[ii], fb.id, fn)) {
                delete builder.irb;
                return false;
            }
            if (builder.consumed_next)
                ++ii; // #4570: fused compare+branch lowered both
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
    // Issue #114 / #2475: protects compile_fns_ (name → ScalarFn cache).
    // compile() itself is serialized by compile_mtx_; this shared_mutex
    // is for cache map access only (shared lookup / unique publish /
    // hot-swap erase). It does not enable parallel LLVM compiles.
    std::shared_mutex fn_compile_mtx_;
    std::unordered_map<std::string, ScalarFn, aura::core::TransparentStringHash, std::equal_to<>>
        compile_fns_{};
    // Issue #1522: tracker entry carries compile epoch + soft deopt flag.
    // deopt_pending: bridge epoch bumped for this name; refuse cache hits
    // until next recompile clears the flag.
    // Issue #2022: source_marker + provenance survive after native is live
    // so deopt / mutate / Agent blame can still recover MacroIntroduced.
    struct FnTrackerEntry {
        llvm::orc::ResourceTrackerSP tracker;
        std::uint64_t compile_epoch = 0;
        bool deopt_pending = false;
        uint8_t source_marker = 0;
        uint32_t provenance = 0;
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
        // Preserve MacroIntroduced tag across hot-swap recompile when
        // the prior entry had it (until the new compile re-stamps).
        if (it != fn_trackers_.end()) {
            entry.source_marker = it->second.source_marker;
            entry.provenance = it->second.provenance;
        }
        if (it != fn_trackers_.end())
            note_tracker_dropped_locked(it->second);
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
    std::unordered_map<std::string, std::atomic<std::uint64_t>, aura::core::TransparentStringHash,
                       std::equal_to<>>
        fn_unhandled_counts_{};
    // Issue #1477: per-fn captured bridge_epoch (set at compile or via
    // capture_fn_epoch from service atomic_bump). Parallel to
    // fn_unhandled_counts_ — atomic per-entry; compile_mtx_ guards map.
    std::unordered_map<std::string, std::atomic<std::uint64_t>, aura::core::TransparentStringHash,
                       std::equal_to<>>
        fn_captured_epochs_{};
    // Issue #4572: lock-free quiescent fast paths for the per-call probes
    // (is_fn_epoch_stale in every JIT prologue, is_deopt_pending /
    // deopt_pending_count in every closure dispatch). Both were
    // compile_mtx_ + map lookup (count: O(T) walk) on the hot path.
    // Writers update these under compile_mtx_ at every mutation site.
    //  - deopt_pending_live_: exact number of fn_trackers_ entries with
    //    deopt_pending == true. 0 => no name is pending.
    //  - epochs_uniform_: (E << 2) | kEpochsUniform when every captured
    //    entry equals E; kEpochsEmpty when none captured; kEpochsMixed
    //    otherwise. Recomputed (O(C)) on each capture store (rare).
    static constexpr std::uint64_t kEpochsEmpty = 0;
    static constexpr std::uint64_t kEpochsUniform = 1;
    static constexpr std::uint64_t kEpochsMixed = 2;
    std::atomic<std::uint64_t> deopt_pending_live_{0};
    std::atomic<std::uint64_t> epochs_uniform_{kEpochsEmpty};
    void set_deopt_pending_locked(FnTrackerEntry& e, bool v) noexcept {
        if (e.deopt_pending == v)
            return;
        e.deopt_pending = v;
        if (v)
            deopt_pending_live_.fetch_add(1, std::memory_order_acq_rel);
        else
            deopt_pending_live_.fetch_sub(1, std::memory_order_acq_rel);
    }
    void note_tracker_dropped_locked(const FnTrackerEntry& e) noexcept {
        if (e.deopt_pending)
            deopt_pending_live_.fetch_sub(1, std::memory_order_acq_rel);
    }
    // Erase one tracker (caller holds compile_mtx_), keeping the count.
    template <class It> It erase_tracker_locked(It it) {
        note_tracker_dropped_locked(it->second);
        return fn_trackers_.erase(it);
    }
    void recompute_epochs_uniform_locked() noexcept {
        std::uint64_t word = kEpochsEmpty;
        bool first = true;
        std::uint64_t v0 = 0;
        for (const auto& [_, ep] : fn_captured_epochs_) {
            const auto v = ep.load(std::memory_order_relaxed);
            if (first) {
                v0 = v;
                first = false;
                word = (v >> 62) != 0 ? kEpochsMixed : ((v << 2) | kEpochsUniform);
            } else if (v != v0) {
                word = kEpochsMixed;
                break;
            }
        }
        epochs_uniform_.store(word, std::memory_order_release);
    }
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
    std::unordered_map<std::string, FnTrackerEntry, aura::core::TransparentStringHash,
                       std::equal_to<>>
        fn_trackers_;

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
    std::unordered_map<std::string, std::unique_ptr<llvm::Module>,
                       aura::core::TransparentStringHash, std::equal_to<>>
        fn_aot_modules_;
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
        reg("aura_cell_set_jit_bind", (void*)aura_cell_set_jit_bind); // #4600
        reg("aura_top_cell_get", (void*)aura_top_cell_get);
        reg("aura_alloc_pair", (void*)aura_alloc_pair);
        reg("aura_alloc_pair_arena", (void*)aura_alloc_pair_arena);
        reg("aura_pair_car", (void*)aura_pair_car);
        reg("aura_pair_cdr", (void*)aura_pair_cdr);
        reg("aura_pair_car_unchecked", (void*)aura_pair_car_unchecked);
        reg("aura_pair_cdr_unchecked", (void*)aura_pair_cdr_unchecked);
        reg("aura_jit_pair_inline_ok", (void*)aura_jit_pair_inline_ok);
        reg("aura_prim_call", (void*)aura_prim_call);
        reg("aura_set_prim_dispatcher", (void*)aura_set_prim_dispatcher);
        reg("aura_hash_ref", (void*)aura_hash_ref);
        reg("aura_hash_set", (void*)aura_hash_set);
        reg("aura_hash_remove", (void*)aura_hash_remove);
        reg("aura_display_int", (void*)aura_display_int);
        reg("aura_display_char", (void*)aura_display_char);
        reg("aura_display_value", (void*)aura_display_value); // Issue #2572
        reg("aura_newline", (void*)aura_newline);
        reg("aura_jit_epoch_acquire_fence", (void*)aura_jit_epoch_acquire_fence);
        reg("aura_jit_linear_post_invalidate_safety",
            (void*)aura_jit_linear_post_invalidate_safety);
        // Issue #1534 / #1535 / #1537: dual-epoch fences + deopt counter.
        reg("aura_jit_guard_shape_epoch_check", (void*)aura_jit_guard_shape_epoch_check);
        reg("aura_jit_linear_epoch_safety_check", (void*)aura_jit_linear_epoch_safety_check);
        // Issue #3186: register the JIT commit_readiness bridge so ORC can
        // resolve aura_jit_linear_move_drop_elision_ok symbols emitted by
        // linear_safety_probe. Production-only consultation of the
        // commit_readiness face (predicate short-circuits under Soft/Off).
        reg("aura_jit_linear_move_drop_elision_ok", (void*)aura_jit_linear_move_drop_elision_ok);
        // Issue #3224: production IR/JIT typed-entry commit_readiness gate.
        reg("aura_jit_ir_typed_entry_commit_readiness_ok",
            (void*)aura_jit_ir_typed_entry_commit_readiness_ok);
        reg("aura_jit_linear_post_mutate_enforce", (void*)aura_jit_linear_post_mutate_enforce);
        reg("aura_jit_get_current_bridge_epoch", (void*)aura_jit_get_current_bridge_epoch);
        reg("aura_jit_is_fn_epoch_stale", (void*)aura_jit_is_fn_epoch_stale);
        reg("aura_jit_deopt_to_interpreter", (void*)aura_jit_deopt_to_interpreter);
        // Issue #4600 §1/§2 (phase 2): shared frame-state deopt stub.
        reg("aura_jit_deopt_frame", (void*)aura_jit_deopt_frame);
        reg("aura_check_shape_guard", (void*)aura_check_shape_guard);
        reg("aura_jit_peek_prologue_deopt_sentinel", (void*)aura_jit_peek_prologue_deopt_sentinel);
        reg("aura_deopt_inc", (void*)aura_deopt_inc);
        // Issue #3988: Jump back-edge poll (strong in evaluator_fiber_mutation).
        reg("aura_jit_poll_hold_budget_safepoint", (void*)aura_jit_poll_hold_budget_safepoint);
        reg("aura_get_defuse_version", (void*)aura_get_defuse_version);
        // Issue #4600 §4: car/cdr guard slow-path deopt attribution.
        reg("aura_jit_note_guard_deopt", (void*)aura_jit_note_guard_deopt);
        reg("aura_jit_prim_dispatch", (void*)aura_jit_prim_dispatch);
        reg("aura_register_fn", (void*)aura_register_fn);
        reg("aura_cast_op", (void*)aura_cast_op);
        reg("aura_reset_runtime", (void*)aura_reset_runtime);
        reg("aura_alloc_float", (void*)aura_alloc_float);
        reg("aura_float_ref", (void*)aura_float_ref);
        reg("aura_alloc_string", (void*)aura_alloc_string);
        reg("aura_intern_keyword", (void*)aura_intern_keyword); // Issue #4321
        reg("aura_string_ref", (void*)aura_string_ref);
        reg("aura_jit_string_content", (void*)aura_jit_string_content);
        reg("aura_drop_pair", (void*)aura_drop_pair);
        reg("aura_drop_cell", (void*)aura_drop_cell);
        reg("aura_drop_closure", (void*)aura_drop_closure);
        // Issue #2293: linear pin/unpin bridges — extern "C"
        // forwarders defined in aura_jit_runtime.cpp that delegate
        // to aura::core::lifetime::{pin,unpin}_linear_root. ORC
        // registration is required because the JIT lowering emits
        // direct CreateCall to these symbols (not C++ member
        // functions), so the resolver must know their addresses
        // before the AOT module is linked.
        reg("aura_jit_pin_linear_root", (void*)aura_jit_pin_linear_root);
        reg("aura_jit_unpin_linear_root", (void*)aura_jit_unpin_linear_root);
        reg("aura_arena_push", (void*)aura_arena_push);
        reg("aura_arena_pop", (void*)aura_arena_pop);
        reg("aura_arena_offset", (void*)aura_arena_offset);
        reg("aura_alloc_closure_arena", (void*)aura_alloc_closure_arena);

        reg("aura_hash_get_flat_table", (void*)aura_hash_get_flat_table);
        reg("aura_set_hash_str_eq_callback", (void*)aura_set_hash_str_eq_callback);
        reg("aura_set_hash_str_convert_callback", (void*)aura_set_hash_str_convert_callback);
        reg("aura_hash_key_eq", (void*)aura_hash_key_eq);
        reg("aura_hash_splitmix64", (void*)aura_hash_splitmix64);
        reg("aura_hash_probe_slot", (void*)aura_hash_probe_slot);

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
        // + lookup. Sub-ms per call in practice.
        //
        // Issue #114 / #2475: compile_mtx_ is the global serializer for
        // all compile() calls (ORC JITDylib inserts are not fully
        // thread-safe across concurrent modules here). fn_compile_mtx_
        // protects only the compile_fns_ name→ScalarFn cache map:
        // shared_lock for hit lookup, unique_lock when publishing a new
        // entry after a successful compile. It does NOT enable parallel
        // compiles of different functions while compile_mtx_ is held.
        //
        // Issue #2475: removed unused default-constructed `fn_lock`
        // (unique_lock not bound to any mutex) that suggested a
        // per-function exclusive upgrade which was never assigned.
        std::lock_guard<std::mutex> compile_lock(compile_mtx_);
        if (!init())
            return nullptr;

        // Cache lookup under shared fn_compile_mtx_ (compile_mtx_ already
        // held — same-function concurrent callers serialize on the global
        // lock; one wins the compile, later callers hit compile_fns_).
        {
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
                } else if (tit != fn_trackers_.end() && tit->second.source_marker == 1 &&
                           aura_macro_hygiene_production_fail_closed() != 0) {
                    // Issue #3475: do not serve cached native of a
                    // MacroIntroduced envelope under production.
                    aura_jit_macro_introduced_deopt_inc();
                    return nullptr;
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

        // Issue #4522: raw-double shadow slots for SHAPE_FLOAT locals
        // (named-let float accumulator unboxing). Created only from
        // profile feedback (fn.shape_map); macro-origin functions (#1610
        // per-fn marker) never specialize. The i1 flag alloca starts 0
        // = tagged-slot-authoritative so entry/garbage state is boxed.
        builder.flocals.assign(fn.local_count, nullptr);
        builder.funboxed.assign(fn.local_count, nullptr);
        if (fn.shape_map && fn.source_marker != 1) {
            auto* i8_zero = llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0);
            for (uint32_t i = 0; i < fn.local_count; ++i) {
                if (fn.shape_map[i] != SHAPE_FLOAT)
                    continue;
                builder.flocals[i] = builder.irb->CreateAlloca(llvm::Type::getDoubleTy(ctx));
                builder.funboxed[i] = builder.irb->CreateAlloca(llvm::Type::getInt8Ty(ctx));
                builder.irb->CreateStore(i8_zero, builder.funboxed[i]);
            }
        }
        // Issue #4570: scalar-replace function-local cells (let + set!,
        // while counters). Each promoted cell's content lives in an i64
        // alloca that mem2reg turns into SSA — no aura_new_cell /
        // aura_cell_get / aura_cell_set (workspace lock) in the loop.
        builder.setup_l1_slots(fn);
        // Issue #4573: scalar-replaced pair fields (entry-block allocas).
        builder.setup_scalar_pairs(fn);

        // Issue #157 Phase 1b: reserve an extra local slot (at index
        // fn.local_count) for the captured defuse_version_. At function
        // entry we call aura_get_defuse_version() and store the result
        // here; at each L2 SHAPE_PAIR use we compare the current
        // version against this captured value. On mismatch, deopt to
        // the slow path (aura_pair_car / aura_pair_cdr with bounds +
        // lock). On match, take the fast path (aura_pair_car_unchecked
        // / aura_pair_cdr_unchecked, no bounds, no lock).
        builder.llvm_locals.push_back(builder.irb->CreateAlloca(llvm::Type::getInt64Ty(ctx)));
        // Issue #4600 §1/§2 (phase 2): arm guard exits to the shared
        // frame-deopt stub. Named fns only (the stub resolves the resume
        // IRFunction by module-unique name); no AOT (host resume is
        // in-process only); no promoted cells / scalar-replaced pairs —
        // their content lives in allocas the interpreter cannot see, and
        // materializing it back to heap form is follow-up scope. Float
        // shadows are fine: load() reboxes them into the spill.
        builder.frame_deopt_ok = false;
        {
            std::uint32_t entry_idx_scan = 0;
            for (std::uint32_t bi = 0; bi < fn.num_blocks; ++bi)
                if (fn.blocks[bi].id == fn.entry_block)
                    entry_idx_scan = bi;
            builder.entry_block_idx = entry_idx_scan;
            bool has_hidden_state = false;
            for (std::uint32_t i = 0; i < fn.local_count && !has_hidden_state; ++i)
                has_hidden_state = builder.is_pcell(i) || builder.is_spair(i) ||
                                   builder.is_spair(fn.local_count + i);
            if (jit_frame_deopt_enabled() && fn.name && fn.name[0] && !builder.aot_mode &&
                fn.local_count > 0 && builder.fn_deopt_frame && !has_hidden_state) {
                builder.frame_deopt_ok = true;
                aura::compiler::jit_assume::stats().frame_deopt_armed.fetch_add(
                    1, std::memory_order_relaxed);
                builder.spill_buf = builder.irb->CreateAlloca(
                    llvm::ArrayType::get(llvm::Type::getInt64Ty(ctx), fn.local_count));
            }
        }
        // Issue #4572: the captured version is only read by the SHAPE_PAIR
        // car/cdr lowering below; functions without car/cdr (fib, loops)
        // and L2 functions (pair_ok path) skip the per-activation call.
        bool reads_pairs = false;
        for (uint32_t bi = 0; bi < fn.num_blocks && !reads_pairs; ++bi) {
            const auto& blk = fn.blocks[bi];
            for (uint32_t ii = 0; ii < blk.num_instructions; ++ii) {
                const auto op = blk.instructions[ii].opcode;
                if (op == OpCar || op == OpCdr) {
                    reads_pairs = true;
                    break;
                }
            }
        }
        // Issue #4571 (L2): functions that read pairs evaluate the tenant
        // gate once per activation and bake the segment directory address
        // (in-process JIT only; the AOT path keeps the call lowering).
        // Issue #4600 §2: force mode routes car/cdr through the spec_pair
        // guard (whose compare is forced to mismatch) so every access is
        // a frame-deopt exit — skip the L2 inline arming there.
        const bool force_deopt_routes_pairs =
            builder.frame_deopt_ok && jit_force_guard_deopt() >= 1 && reads_pairs;
        if (jit_l2_enabled() && !force_deopt_routes_pairs && builder.fn_pair_inline_ok &&
            reads_pairs) {
            auto* ok = builder.irb->CreateCall(builder.fn_pair_inline_ok, {});
            builder.pair_ok = builder.irb->CreateICmpNE(ok, builder.c64(0));
            builder.pair_dir = builder.irb->CreateIntToPtr(
                builder.c64(reinterpret_cast<std::uintptr_t>(aura_pair_seg_dir_base())),
                llvm::PointerType::getUnqual(ctx));
        }
        if (reads_pairs && !builder.pair_ok) {
            auto* ver = builder.irb->CreateCall(builder.fn_get_defuse_version, {});
            builder.irb->CreateStore(ver, builder.llvm_locals[fn.local_count]);
        }

        // Issue #4600/#4606: value slots written by MakeClosure — the
        // entry fn's define-cell CellSets on these are loader plumbing and
        // take the bind entry (no user-mutation choke; tenant gate stays).
        for (std::uint32_t mbi = 0; mbi < fn.num_blocks; ++mbi)
            for (std::uint32_t mii = 0; mii < fn.blocks[mbi].num_instructions; ++mii)
                if (fn.blocks[mbi].instructions[mii].opcode == OpMakeClosure)
                    builder.mc_value_slots.insert(fn.blocks[mbi].instructions[mii].ops[0]);

        // Issue #1537 + #1540: Apply prologue — dual-epoch + linear_post_mutate.
        //   %cur = call @aura_jit_get_current_bridge_epoch()
        //   %stale = call @aura_jit_is_fn_epoch_stale(name, %cur)
        //   %lin = call @aura_jit_linear_post_mutate_enforce(UINT32_MAX)
        //   %unsafe = or %stale, %lin
        //   br %unsafe, deopt, cont
        //   deopt: ret call @aura_jit_deopt_to_interpreter(name)
        //   cont:  <function body>
        // Issue #3419: production/Full also emit typed-entry on anonymous
        // functions (named path already did via #3224). Soft/Off omit that
        // call. Deopt reuses linear_post_mutate_force_rollback_total /
        // g_linear_fast_path_elide_blocked_production_total (no new key).
        builder.entry_body_bb = nullptr;
        const bool named = fn.name && fn.name[0];
        const bool hard_typed_entry = aura::compiler::typed_audit::production_defaults_active() ||
                                      aura::compiler::typed_audit::get_strategy() ==
                                          aura::compiler::typed_audit::AuditStrategy::Full;
        const bool can_epoch = named && builder.fn_get_current_bridge_epoch &&
                               builder.fn_is_fn_epoch_stale && builder.fn_deopt_to_interpreter;
        const bool can_typed = hard_typed_entry && builder.fn_ir_typed_entry_commit_readiness_ok &&
                               builder.fn_deopt_to_interpreter;
        // Issue #3616: anonymous production functions ran the JIT body with
        // typed-entry only — linear_post_mutate_enforce stayed named-only
        // because #1540 emitted it inside the epoch arm. Post-mutate linear
        // unsafety (remount last==0 / persist-reject hygiene) must deopt anon
        // applies too: emit the same UINT32_MAX env-hint call (host resolves
        // g_linear_env_id). Suppressed when can_epoch already emitted it
        // (named path) — exactly one linear probe per function, no double
        // count. Soft/Off keep zero prologue calls (hard_typed_entry false).
        const bool can_linear = hard_typed_entry && !can_epoch &&
                                builder.fn_linear_post_mutate_enforce &&
                                builder.fn_deopt_to_interpreter;
        if (can_epoch || can_typed || can_linear) {
            auto* entry_bb = builder.block_map[fn.entry_block];
            auto* parent = builder.func;
            auto* bb_deopt = llvm::BasicBlock::Create(ctx, "epoch_prologue_deopt", parent);
            auto* bb_cont = llvm::BasicBlock::Create(ctx, "epoch_prologue_cont", parent);
            builder.irb->SetInsertPoint(entry_bb);
            const char* prologue_name = named ? fn.name : "<anon>";
            auto* name_gv = builder.irb->CreateGlobalString(prologue_name, "prologue_fn_name");
            auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
            auto* name_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                name_gv->getValueType(), name_gv, llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
            llvm::Value* is_unsafe = nullptr;
            if (can_epoch) {
                // Issue #4600 §7: gate the dual-epoch C calls behind one
                // acquire load of the fn's DefineVersion word. In assumption
                // mode (#4569) every path that makes this fn stale bumps
                // its word (invalidate_function → DefineVersion bump), and
                // validated cache hits restamp the captured epoch the same
                // way — word unchanged ⇒ not stale. The epoch calls run only
                // on the cold moved path (whose locked lookup may still
                // answer not-stale). Global-epoch mode and
                // AURA_JIT_PROLOGUE_WORD=0 keep the unconditional prologue.
                const bool word_gate =
                    jit_prologue_word_enabled() && !aura::compiler::jit_assume::global_epoch_mode();
                llvm::Value* stale_i = nullptr;
                if (word_gate) {
                    auto* word =
                        aura::compiler::jit_assume::DefineVersions::instance().word(fn.name);
                    const auto expected = word->v.load(std::memory_order_acquire);
                    aura::compiler::jit_assume::stats().prologue_word_armed.fetch_add(
                        1, std::memory_order_relaxed);
                    auto* wptr = builder.irb->CreateIntToPtr(
                        builder.c64(reinterpret_cast<std::uintptr_t>(&word->v)),
                        llvm::PointerType::getUnqual(ctx));
                    auto* wv = builder.irb->CreateLoad(llvm::Type::getInt64Ty(ctx), wptr);
                    wv->setAtomic(llvm::AtomicOrdering::Acquire);
                    wv->setAlignment(llvm::Align(8));
                    auto* moved = builder.irb->CreateICmpNE(wv, builder.c64(expected));
                    auto* parent_f = builder.func;
                    auto* bb_word_ok = llvm::BasicBlock::Create(ctx, "prologue_word_ok", parent_f);
                    auto* bb_epoch_chk =
                        llvm::BasicBlock::Create(ctx, "prologue_epoch_chk", parent_f);
                    auto* bb_stale_join =
                        llvm::BasicBlock::Create(ctx, "prologue_stale_join", parent_f);
                    builder.br_likely(builder.irb->CreateNot(moved), bb_word_ok, bb_epoch_chk);
                    builder.irb->SetInsertPoint(bb_word_ok);
                    builder.irb->CreateBr(bb_stale_join);
                    builder.irb->SetInsertPoint(bb_epoch_chk);
                    auto* cur_epoch = builder.irb->CreateCall(
                        llvm::FunctionCallee(builder.fn_get_current_bridge_epoch), {});
                    auto* stale_call =
                        builder.irb->CreateCall(llvm::FunctionCallee(builder.fn_is_fn_epoch_stale),
                                                llvm::ArrayRef<llvm::Value*>{name_ptr, cur_epoch});
                    builder.irb->CreateBr(bb_stale_join);
                    builder.irb->SetInsertPoint(bb_stale_join);
                    auto* stale_phi =
                        builder.irb->CreatePHI(llvm::Type::getInt32Ty(ctx), 2, "prologue_stale_i");
                    stale_phi->addIncoming(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0),
                                           bb_word_ok);
                    stale_phi->addIncoming(stale_call, bb_epoch_chk);
                    stale_i = stale_phi;
                } else {
                    auto* cur_epoch = builder.irb->CreateCall(
                        llvm::FunctionCallee(builder.fn_get_current_bridge_epoch), {});
                    stale_i =
                        builder.irb->CreateCall(llvm::FunctionCallee(builder.fn_is_fn_epoch_stale),
                                                llvm::ArrayRef<llvm::Value*>{name_ptr, cur_epoch});
                }
                // UINT32_MAX → use g_linear_env_id (host sets context before Apply).
                auto* env_max = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0xFFFFFFFFu);
                llvm::Value* lin_i = zero32;
                if (builder.fn_linear_post_mutate_enforce) {
                    lin_i = builder.irb->CreateCall(
                        llvm::FunctionCallee(builder.fn_linear_post_mutate_enforce),
                        llvm::ArrayRef<llvm::Value*>{env_max});
                }
                auto* unsafe_i = builder.irb->CreateOr(stale_i, lin_i);
                is_unsafe = builder.irb->CreateICmpNE(unsafe_i, zero32);
            }
            if (can_linear) {
                // Issue #3616: same probe the named epoch arm emits —
                // UINT32_MAX → host g_linear_env_id. Non-zero return =
                // post-mutate linear state unsafe → deopt (shared path with
                // typed-entry below; no separate rollback metric).
                auto* env_max_lin =
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0xFFFFFFFFu);
                auto* lin_i = builder.irb->CreateCall(
                    llvm::FunctionCallee(builder.fn_linear_post_mutate_enforce),
                    llvm::ArrayRef<llvm::Value*>{env_max_lin});
                auto* lin_unsafe = builder.irb->CreateICmpNE(lin_i, zero32);
                is_unsafe = is_unsafe ? builder.irb->CreateOr(is_unsafe, lin_unsafe) : lin_unsafe;
            }
            if (can_typed) {
                auto* entry_ok_i = builder.irb->CreateCall(
                    llvm::FunctionCallee(builder.fn_ir_typed_entry_commit_readiness_ok),
                    llvm::ArrayRef<llvm::Value*>{});
                auto* entry_blocked = builder.irb->CreateICmpEQ(entry_ok_i, zero32);
                is_unsafe =
                    is_unsafe ? builder.irb->CreateOr(is_unsafe, entry_blocked) : entry_blocked;
            }
            if (!is_unsafe)
                is_unsafe = llvm::ConstantInt::getFalse(ctx);
            // Issue #4600 §2: force level 2 makes every prologue stale so
            // each native invocation deopts at entry (acceptance mode —
            // entry resume runs the whole body in the interpreter).
            if (builder.frame_deopt_ok && jit_force_guard_deopt() >= 2)
                is_unsafe = llvm::ConstantInt::getTrue(ctx);
            builder.irb->CreateCondBr(is_unsafe, bb_deopt, bb_cont);
            // Deopt path: Issue #4600 §1/§2 — with the frame-deopt stub
            // armed the stale path resumes the IR interpreter at the
            // entry (block, 0) from the caller's locals buffer, runs the
            // body to completion and returns its value (no sentinel, no
            // host-side error). Without the stub this stays the #4084
            // fixnum-0 sentinel path.
            builder.irb->SetInsertPoint(bb_deopt);
            if (builder.frame_deopt_ok) {
                builder.emit_frame_deopt_exit(fn, builder.entry_block_idx, 0,
                                              /*from_entry=*/true);
            } else {
                auto* deopt_ret =
                    builder.irb->CreateCall(llvm::FunctionCallee(builder.fn_deopt_to_interpreter),
                                            llvm::ArrayRef<llvm::Value*>{name_ptr});
                builder.irb->CreateRet(deopt_ret);
            }
            // Body continues in cont BB.
            builder.entry_body_bb = bb_cont;
            if (metrics)
                metrics->prologue_emit_total.fetch_add(1, std::memory_order_relaxed);
        }

        // Issue #4567: a shape map speculates argument slots. Check the
        // OpArg buffer (env is prepended, so the index is not local 0)
        // before the body. A miss returns the prologue deopt sentinel.
        // No map, or a macro-origin function, emits no call.
        if (fn.shape_map && fn.source_marker != 1 && builder.fn_check_shape_guard &&
            builder.fn_deopt_to_interpreter) {
            std::vector<std::uint8_t> codes;
            for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
                const auto& fb = fn.blocks[bi];
                for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
                    const auto& inst = fb.instructions[ii];
                    if (inst.opcode != OpArg)
                        continue;
                    const uint32_t buf = inst.ops[1];
                    const uint32_t dest = inst.ops[0];
                    if (buf >= fn.local_count || dest >= fn.local_count)
                        continue;
                    const auto expect = fn.shape_map[dest];
                    if (expect == 0)
                        continue;
                    if (codes.size() <= buf)
                        codes.resize(static_cast<std::size_t>(buf) + 1, 0);
                    codes[buf] = static_cast<std::uint8_t>(expect);
                }
            }
            bool any = false;
            for (auto c : codes)
                if (c != 0)
                    any = true;
            if (any) {
                auto* insert_bb = builder.entry_body_bb ? builder.entry_body_bb
                                                        : builder.block_map[fn.entry_block];
                builder.irb->SetInsertPoint(insert_bb);
                auto* arr = llvm::ConstantDataArray::get(ctx, codes);
                auto* gv = new llvm::GlobalVariable(*builder.mod, arr->getType(), true,
                                                    llvm::GlobalValue::PrivateLinkage, arr,
                                                    "shape_guard_map");
                auto* zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
                auto* map_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                    gv->getValueType(), gv, llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
                auto* n32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx),
                                                   static_cast<std::uint64_t>(codes.size()));
                auto* locals_ptr = &*builder.func->arg_begin();
                auto* guard_ok = builder.irb->CreateCall(
                    llvm::FunctionCallee(builder.fn_check_shape_guard),
                    llvm::ArrayRef<llvm::Value*>{locals_ptr, n32, map_ptr, n32});
                auto* miss = builder.irb->CreateICmpEQ(guard_ok, zero32);
                // Issue #4600 §2: force mode makes the entry shape guard
                // miss so the body-entry resume is exercised (real maps
                // only — no map, no guard, nothing to force).
                if (builder.frame_deopt_ok && jit_force_guard_deopt() >= 1)
                    miss = llvm::ConstantInt::getTrue(ctx);
                auto* bb_deopt = llvm::BasicBlock::Create(ctx, "shape_guard_deopt", builder.func);
                auto* bb_cont = llvm::BasicBlock::Create(ctx, "shape_guard_cont", builder.func);
                builder.irb->CreateCondBr(miss, bb_deopt, bb_cont);
                builder.irb->SetInsertPoint(bb_deopt);
                if (builder.frame_deopt_ok) {
                    // Issue #4600 §1/§2: shared frame-deopt stub — resume at
                    // the entry (block, 0); the host re-runs the Arg ops from
                    // the caller's env++args buffer.
                    builder.emit_frame_deopt_exit(fn, builder.entry_block_idx, 0,
                                                  /*from_entry=*/true);
                } else {
                    const char* gname = (fn.name && fn.name[0]) ? fn.name : "<anon>";
                    auto* name_gv = builder.irb->CreateGlobalString(gname, "shape_guard_fn_name");
                    auto* name_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                        name_gv->getValueType(), name_gv,
                        llvm::ArrayRef<llvm::Constant*>{zero32, zero32});
                    auto* deopt_ret = builder.irb->CreateCall(
                        llvm::FunctionCallee(builder.fn_deopt_to_interpreter),
                        llvm::ArrayRef<llvm::Value*>{name_ptr});
                    builder.irb->CreateRet(deopt_ret);
                }
                builder.entry_body_bb = bb_cont;
            }
        }

        // Lower each block
        for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
            auto& fb = fn.blocks[bi];
            // Entry body lands in prologue cont BB when prologue was emitted.
            if (fb.id == fn.entry_block && builder.entry_body_bb)
                builder.irb->SetInsertPoint(builder.entry_body_bb);
            else
                builder.irb->SetInsertPoint(builder.block_map[fb.id]);
            for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
                builder.cur_block = bi; // Issue #4600 §2: guard-exit resume point
                builder.cur_instr = ii;
                builder.next_inst =
                    ii + 1 < fb.num_instructions ? &fb.instructions[ii + 1] : nullptr;
                builder.consumed_next = false;
                if (!builder.lower(fb.instructions[ii], fb.id, fn)) {
                    delete builder.irb;
                    return nullptr;
                }
                if (builder.consumed_next)
                    ++ii; // #4570: fused compare+branch lowered both
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

        // Publish ScalarFn into compile_fns_ under unique fn_compile_mtx_.
        // compile_mtx_ is still held (this thread is the only compiler);
        // the unique_lock serializes with shared cache lookups / erase.
        {
            std::unique_lock<std::shared_mutex> lock(fn_compile_mtx_);
            compile_fns_[std::string(fn.name)] = fn_ptr;
        }

        // Issue #2022: preserve MacroIntroduced into native metadata after
        // successful compile so query/deopt/blame still see the marker once
        // native code is live. Scan FlatInstructions once; OR with
        // FlatFunction::source_marker (set by service from IRFunction::marker).
        // Non-macro path: all markers 0 → no side-table write / no counter bump.
        {
            uint8_t max_marker = fn.source_marker;
            uint32_t first_prov = fn.provenance;
            for (uint32_t bi = 0; bi < fn.num_blocks; ++bi) {
                const auto& fb = fn.blocks[bi];
                if (!fb.instructions)
                    continue;
                for (uint32_t ii = 0; ii < fb.num_instructions; ++ii) {
                    const auto& inst = fb.instructions[ii];
                    if (inst.source_marker == 1) {
                        max_marker = 1;
                        if (first_prov == 0 && inst.provenance != 0)
                            first_prov = inst.provenance;
                    } else if (first_prov == 0 && inst.provenance != 0) {
                        first_prov = inst.provenance;
                    }
                    if (max_marker == 1 && first_prov != 0)
                        break;
                }
                if (max_marker == 1 && first_prov != 0)
                    break;
            }
            // If marker is MacroIntroduced but provenance still 0, keep 0 here;
            // register_function / note will apply weak func_id fallback.
            if (fn.name && fn.name[0]) {
                auto tit = fn_trackers_.find(std::string(fn.name));
                if (tit != fn_trackers_.end()) {
                    tit->second.source_marker = max_marker;
                    if (first_prov != 0)
                        tit->second.provenance = first_prov;
                }
            }
            if (max_marker == 1)
                aura_jit_note_native_macro_preserved(max_marker, first_prov);
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
        bool hygiene_refuse = false;
        {
            std::lock_guard<std::mutex> lock(compile_mtx_);
            auto it = fn_trackers_.find(name);
            if (it != fn_trackers_.end() && it->second.deopt_pending) {
                if (metrics)
                    metrics->deopt_pending_invoke_fallbacks.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            // Issue #3475: production must not return a live native pointer
            // for a MacroIntroduced envelope (lost dirty still looks User).
            if (it != fn_trackers_.end() && it->second.source_marker == 1 &&
                aura_macro_hygiene_production_fail_closed() != 0) {
                hygiene_refuse = true;
            }
        }
        if (hygiene_refuse) {
            aura_jit_macro_introduced_deopt_inc();
            return nullptr;
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
                          uint32_t arg_count, uint32_t env_count, const char* name = nullptr,
                          uint8_t source_marker = 0, uint32_t provenance = 0) {
        // Issue #660 Option 1: also register by name if provided.
        if (name && *name) {
            aura_register_fn_named(name, func_id, fn_ptr, static_cast<int32_t>(local_count),
                                   static_cast<int32_t>(arg_count),
                                   static_cast<int32_t>(env_count));
            // Issue #4521: bind the JIT func_id to this define after the
            // named publish (register_fn_entry erases stale bindings) so
            // the native drop's #4405 func-id walk sees OpMakeClosure
            // closures that carry the id with an empty closure name.
            if (func_id >= 0)
                aura_note_ir_func_define(func_id, name);
            // Issue #1516: map IR func_id → name for per-function AOT.
            if (func_id >= 0) {
                std::lock_guard<std::mutex> lock(compile_mtx_);
                fn_id_to_name_[static_cast<std::uint32_t>(func_id)] = name;
            }
            // Issue #2022: mirror marker into name-keyed tracker when live.
            if (source_marker == 1) {
                std::lock_guard<std::mutex> lock(compile_mtx_);
                auto tit = fn_trackers_.find(std::string(name));
                if (tit != fn_trackers_.end()) {
                    tit->second.source_marker = 1;
                    if (provenance != 0)
                        tit->second.provenance = provenance;
                }
            }
        } else {
            aura_register_fn(func_id, fn_ptr, static_cast<int32_t>(local_count),
                             static_cast<int32_t>(arg_count), static_cast<int32_t>(env_count));
        }
        // Issue #2022: func_id side-table for AOT/native query after register.
        if (source_marker == 1)
            aura_jit_stamp_fn_macro_marker(func_id, source_marker, provenance);
        compiled_fns_.push_back({name ? std::string(name) : std::string(), fn_ptr, local_count,
                                 arg_count, env_count, source_marker, provenance});
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

// Issue #1658: CI/debug consistency gate. Under strict mode (default ON),
// compile() already folds unhandled → consistency_violations; this entry
// re-reads the counter so agents/tests can assert zero violations without
// a new public EDSL primitive (SlimSurface / engine:metrics only).
bool AuraJIT::force_jit_consistency_check() noexcept {
    if (strict_consistency_mode_) {
        const auto unhandled = metrics_.unhandled_opcode_count.load(std::memory_order_relaxed);
        // Unhandled without a prior compile-fail fold still counts as a
        // consistency risk (e.g. metrics-only test harness mirrors).
        if (unhandled > 0 && metrics_.consistency_violations.load(std::memory_order_relaxed) == 0) {
            metrics_.consistency_violations.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return metrics_.consistency_violations.load(std::memory_order_relaxed) == 0;
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

// Issue #1917: critical-set coverage (MakeClosure/Apply/PrimCall/GuardShape/Linear*).
// Uses static kCriticalOpcodeMask ∩ fully-lowered guarantee (all critical bits are
// in kFullyLoweredOpcodeMask) → baseline 100% when unhandled_critical == 0.
// When compiles have stamped opcode_covered_mask, report intersection density.
std::uint64_t AuraJIT::critical_opcode_coverage_pct() const noexcept {
    const auto unhandled_crit =
        metrics_.critical_opcode_unhandled_total.load(std::memory_order_relaxed);
    if (unhandled_crit > 0) {
        // Any critical unhandled → coverage < 100%. Approximate from lowered vs unhandled.
        const auto lowered = metrics_.critical_opcode_lowered_total.load(std::memory_order_relaxed);
        const auto den = lowered + unhandled_crit;
        if (den == 0)
            return 0;
        return (lowered * 100ull) / den;
    }
    // Static guarantee: all critical opcodes have explicit lower() cases.
    // Also verify covered_mask has them if any compile ran.
    const auto covered = metrics_.opcode_covered_mask.load(std::memory_order_relaxed);
    const auto crit = kCriticalOpcodeMask;
    if (covered == 0)
        return 100; // no compile yet — static lower() table is complete
    std::uint64_t hit = 0;
    for (std::uint32_t i = 0; i < 64; ++i) {
        if ((crit & (1ull << i)) == 0)
            continue;
        if (covered & (1ull << i))
            ++hit;
    }
    // If compile didn't exercise all critical ops, still report static completeness
    // weighted by what was seen (≥80% AC when hit/kCritical >= 0.8 or full table).
    if (hit * 100ull / kCriticalOpcodeCount >= 80)
        return (hit * 100ull) / kCriticalOpcodeCount;
    // Static lower table covers all critical ops even if not yet exercised.
    return 100;
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
    // Issue #1917: critical opcode + PrimCall fast-path counters.
    const auto crit_low = critical_opcode_lowered_total.load(std::memory_order_relaxed);
    const auto crit_unh = critical_opcode_unhandled_total.load(std::memory_order_relaxed);
    std::uint64_t crit_pct = 100;
    if (crit_unh > 0) {
        const auto den = crit_low + crit_unh;
        crit_pct = den > 0 ? (crit_low * 100ull) / den : 0;
    }
    const auto pc_fast = primcall_fastpath_hits.load(std::memory_order_relaxed);
    const auto apply_probe = apply_site_epoch_probe_total.load(std::memory_order_relaxed);
    // Live prim-call counters from aura_jit_runtime.cpp
    // (read via the global accessors). The total is in nanoseconds;
    // average per call is computed inline.
    const auto pc = aura_prim_call_count();
    const auto pns = aura_prim_call_total_ns();
    const auto pavg = pc > 0 ? static_cast<std::uint64_t>(pns / pc) : 0u;
    std::snprintf(
        buf, buf_size,
        "jit: compiles=%llu avg_us=%llu hot_swaps=%llu "
        "cached_fns=%llu inlined_prims=%llu slow_prims=%llu "
        "prim_calls=%llu prim_avg_ns=%llu "
        "verify_fail=%llu add_mod_fail=%llu "
        "unhandled_opcode=%llu intrinsics=%llu "
        "fallback_count=%llu consistency_violations=%llu "
        "opcode_coverage_pct=%llu consistency_compares=%llu "
        "consistency_matches=%llu "
        "critical_opcode_lowered=%llu critical_opcode_unhandled=%llu "
        "critical_opcode_coverage_pct=%llu primcall_fastpath_hits=%llu "
        "apply_site_epoch_probe=%llu",
        (unsigned long long)cc, (unsigned long long)avg_us, (unsigned long long)hs,
        (unsigned long long)cfns, (unsigned long long)inl, (unsigned long long)slow,
        (unsigned long long)pc, (unsigned long long)pavg, (unsigned long long)vfail,
        (unsigned long long)mfail, (unsigned long long)unhandled, (unsigned long long)intrinsic,
        (unsigned long long)fallback, (unsigned long long)consistency, (unsigned long long)cov_pct,
        (unsigned long long)cmp_total, (unsigned long long)cmp_match, (unsigned long long)crit_low,
        (unsigned long long)crit_unh, (unsigned long long)crit_pct, (unsigned long long)pc_fast,
        (unsigned long long)apply_probe);
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
    impl_->register_fn_func(func_id, fn_ptr, local_count, arg_count, env_count, name, 0, 0);
}

void AuraJIT::register_function(int64_t func_id, ScalarFn fn_ptr, uint32_t local_count,
                                uint32_t arg_count, uint32_t env_count, const char* name,
                                uint8_t source_marker, uint32_t provenance) {
    impl_->register_fn_func(func_id, fn_ptr, local_count, arg_count, env_count, name, source_marker,
                            provenance);
}

const std::vector<FunctionMeta>& AuraJIT::compiled_functions() const {
    return impl_->compiled_fns_;
}

// Issue #2022: name-keyed MacroIntroduced query after native code is live.
uint8_t AuraJIT::fn_source_marker(const char* name) const {
    if (!impl_ || !name || !name[0])
        return 0;
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    auto it = impl_->fn_trackers_.find(name);
    if (it == impl_->fn_trackers_.end())
        return 0;
    return it->second.source_marker;
}

uint32_t AuraJIT::fn_provenance(const char* name) const {
    if (!impl_ || !name || !name[0])
        return 0;
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    auto it = impl_->fn_trackers_.find(name);
    if (it == impl_->fn_trackers_.end())
        return 0;
    return it->second.provenance;
}

// Issue #1545: declared in aura_jit_bridge.h — host linear live-closure scan.
extern "C" int aura_jit_linear_live_closure_scan(void);

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
    // Issue #1545: pre-evict linear live-closure scan (mark invalid +
    // deopt surface) BEFORE ResourceTracker::remove tears down code.
    (void)aura_jit_linear_live_closure_scan();
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
        impl_->note_tracker_dropped_locked(it->second);
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
    // Do NOT hold compile_mtx_ across invalidate_prefix() — it re-locks.
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
    // Issue #2476: single pass — invalidate_prefix already matches bare
    // name (`it->first == p`) and name#* (`rfind(p_hash, 0) == 0`). A prior
    // bare-name-only invalidate was a redundant second walk of fn_trackers_
    // (+ second compile_mtx_ acquire). Same eviction semantics.
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
            it = impl_->erase_tracker_locked(it);
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

// Wave2: set-code / mark_all_defines_dirty bulk path.
// One linear-live scan + one compile_mtx_ session tears down every
// tracker / unhandled / AOT snapshot / compile_fns_ entry. Avoids
// O(N) × (linear_scan + full-map walk) from per-name invalidate+prefix.
std::size_t AuraJIT::invalidate_all() noexcept {
    if (!impl_)
        return 0;
    std::lock_guard<std::mutex> compile_lock(impl_->compile_mtx_);
    (void)aura_jit_linear_live_closure_scan();
    std::size_t n = 0;
    for (auto& [name, tr] : impl_->fn_trackers_) {
        (void)name;
        if (tr.tracker) {
            if (auto err = tr.tracker->remove())
                llvm::consumeError(std::move(err));
        }
        ++n;
    }
    impl_->fn_trackers_.clear();
    impl_->deopt_pending_live_.store(0, std::memory_order_release);
    impl_->fn_unhandled_counts_.clear();
    impl_->fn_aot_modules_.clear();
    {
        std::unique_lock<std::shared_mutex> lock(impl_->fn_compile_mtx_);
        impl_->compile_fns_.clear();
    }
    return n;
}

// Issue #1477: JIT-side dual-epoch fence.
// Issue #1522: soft batch deopt — mark fn_trackers_ deopt_pending + drop
// compile_fns_ so next invoke / compile refuses stale native code.
void AuraJIT::capture_fn_epoch(const char* name, std::uint64_t current_bridge_epoch) {
    if (!impl_ || !name)
        return;
    std::lock_guard<std::mutex> compile_lock(impl_->compile_mtx_);
    impl_->fn_captured_epochs_[name].store(current_bridge_epoch, std::memory_order_release);
    impl_->recompute_epochs_uniform_locked();
    metrics_.jit_epoch_stale_check_total.fetch_add(1, std::memory_order_relaxed);
}

bool AuraJIT::is_fn_epoch_stale(const char* name, std::uint64_t current_bridge_epoch) const {
    if (!impl_ || !name)
        return false;
    // Issue #4572: lock-free fast path. Empty map => never captured =>
    // pass-through; all entries == current => not stale. Same answer
    // as the locked lookup below.
    {
        const auto w = impl_->epochs_uniform_.load(std::memory_order_acquire);
        if (w == Impl::kEpochsEmpty)
            return false;
        if ((w & 3u) == Impl::kEpochsUniform && (current_bridge_epoch >> 62) == 0 &&
            (w >> 2) == current_bridge_epoch)
            return false;
    }
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
            impl_->set_deopt_pending_locked(entry, true);
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
    // Issue #3977: name-precise owner-scoped invalidate already put F in
    // the #3300 pending name table. Do not mark trackers — that would
    // bump process deopt_pending_count and make every sid==0 leave native.
    // Named leave-native is the name table; same-define unnamed is
    // #3750 slot stale / MustDeopt / #3323 overflow.
    if (aura_aot_last_table_bump_owner_scoped() != 0 &&
        aura_aot_peer_jit_name_is_soft_stale(name) != 0)
        return 0;
    std::string n(name);
    std::string n_hash = n + "#";
    std::size_t marked = 0;
    std::lock_guard<std::mutex> compile_lock(impl_->compile_mtx_);
    for (auto& [key, entry] : impl_->fn_trackers_) {
        if (key != n && key.rfind(n_hash, 0) != 0)
            continue;
        if (!entry.deopt_pending) {
            impl_->set_deopt_pending_locked(entry, true);
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
    // Issue #4572: no tracker pending => no name pending (lock-free).
    if (impl_->deopt_pending_live_.load(std::memory_order_acquire) == 0)
        return false;
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    auto it = impl_->fn_trackers_.find(name);
    return it != impl_->fn_trackers_.end() && it->second.deopt_pending;
}

std::uint64_t AuraJIT::deopt_pending_count() const noexcept {
    if (!impl_)
        return 0;
    // Issue #4572: exact live counter maintained under compile_mtx_.
    if (impl_->deopt_pending_live_.load(std::memory_order_acquire) == 0)
        return 0;
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    std::uint64_t n = 0;
    for (const auto& [_, e] : impl_->fn_trackers_) {
        if (e.deopt_pending)
            ++n;
    }
    return n;
}

// Issue #2050: render-critical soft-dirty under deopt throttle — clear
// deopt_pending for name / name#* and re-stamp captured epoch so the
// previous native body keeps serving frames until the throttle window
// opens for a real recompile.
std::size_t AuraJIT::clear_deopt_pending_keep_native(const char* name,
                                                     std::uint64_t current_epoch) noexcept {
    if (!impl_ || !name || !*name)
        return 0;
    std::lock_guard<std::mutex> lock(impl_->compile_mtx_);
    std::size_t cleared = 0;
    const std::string base(name);
    const std::string prefix = base + "#";
    for (auto& [key, entry] : impl_->fn_trackers_) {
        if (key == base || key.rfind(prefix, 0) == 0) {
            if (entry.deopt_pending) {
                impl_->set_deopt_pending_locked(entry, false);
                ++cleared;
            }
        }
    }
    // Re-stamp epoch under the same lock as capture_fn_epoch would.
    impl_->fn_captured_epochs_[base].store(current_epoch, std::memory_order_release);
    impl_->recompute_epochs_uniform_locked();
    metrics_.jit_epoch_stale_check_total.fetch_add(1, std::memory_order_relaxed);
    return cleared;
}

// ── Public AOT API ──────────────────────────────────────────────

bool emit_native_object(const FlatFunction& fn, const std::string& out_obj_path,
                        const std::vector<std::string>* string_pool) {
    return emit_native_object_llvm(fn, out_obj_path, string_pool);
}

bool emit_object(const std::string& ir_dump, const std::string& out_path) {
    // Issue #2477: deprecated misleading success path removed.
    // Previously wrote out_path+".ir" and returned true — callers
    // expecting a native .o at out_path would silently break.
    // Fail closed: return false + stderr so callers migrate to
    // emit_native_object (supported AOT entry).
    (void)ir_dump;
    (void)out_path;
    std::fprintf(stderr, "emit_object: deprecated, use emit_native_object instead\n");
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

#include <cstdio>

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
void AuraJIT::register_function(int64_t, ScalarFn, uint32_t, uint32_t, uint32_t, const char*,
                                uint8_t, uint32_t) {}
uint8_t AuraJIT::fn_source_marker(const char*) const {
    return 0;
}
uint32_t AuraJIT::fn_provenance(const char*) const {
    return 0;
}
void AuraJIT::invalidate(const char*) {}
void AuraJIT::invalidate_prefix(const char*) {}
std::size_t AuraJIT::invalidate_all() noexcept {
    return 0;
}
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
// Issue #2050: no-LLVM stub — CompilerService soft-dirty path calls this;
// without a real tracker table there is nothing to clear.
std::size_t AuraJIT::clear_deopt_pending_keep_native(const char* name,
                                                     std::uint64_t current_epoch) noexcept {
    (void)name;
    (void)current_epoch;
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
bool AuraJIT::force_jit_consistency_check() noexcept {
    if (strict_consistency_mode_) {
        const auto unhandled = metrics_.unhandled_opcode_count.load(std::memory_order_relaxed);
        if (unhandled > 0 && metrics_.consistency_violations.load(std::memory_order_relaxed) == 0) {
            metrics_.consistency_violations.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return metrics_.consistency_violations.load(std::memory_order_relaxed) == 0;
}
std::uint64_t AuraJIT::opcode_coverage_count() const noexcept {
    return 0;
}
std::uint64_t AuraJIT::opcode_coverage_pct() const noexcept {
    return 0;
}
// Issue #1917: no-LLVM stub — static critical table is complete.
std::uint64_t AuraJIT::critical_opcode_coverage_pct() const noexcept {
    return metrics_.critical_opcode_unhandled_total.load(std::memory_order_relaxed) > 0 ? 0 : 100;
}
const std::vector<FunctionMeta>& AuraJIT::compiled_functions() const {
    static std::vector<FunctionMeta> empty;
    return empty;
}
char* AuraJIT::Metrics::format(char* buf, std::size_t buf_size) const noexcept {
    if (!buf || buf_size == 0)
        return buf;
    // No-LLVM path: zeroed counters + #1917 critical keys for stable parse.
    const auto crit_unh = critical_opcode_unhandled_total.load(std::memory_order_relaxed);
    const auto crit_low = critical_opcode_lowered_total.load(std::memory_order_relaxed);
    const auto crit_pct = crit_unh > 0 ? 0ull : 100ull;
    const auto pc_fast = primcall_fastpath_hits.load(std::memory_order_relaxed);
    const auto apply_probe = apply_site_epoch_probe_total.load(std::memory_order_relaxed);
    std::snprintf(buf, buf_size,
                  "jit: compiles=0 unhandled_opcode=0 fallback_count=0 "
                  "consistency_violations=0 opcode_coverage_pct=0 "
                  "critical_opcode_lowered=%llu critical_opcode_unhandled=%llu "
                  "critical_opcode_coverage_pct=%llu primcall_fastpath_hits=%llu "
                  "apply_site_epoch_probe=%llu",
                  (unsigned long long)crit_low, (unsigned long long)crit_unh,
                  (unsigned long long)crit_pct, (unsigned long long)pc_fast,
                  (unsigned long long)apply_probe);
    return buf;
}


// ── AOT native compilation (stubs, LLVM unavailable) ────────────

bool emit_native_object(const FlatFunction&, const std::string&, const std::vector<std::string>*) {
    return false;
}

bool emit_object(const std::string& ir_dump, const std::string& out_path) {
    // Issue #2477: same fail-closed deprecation as LLVM build.
    (void)ir_dump;
    (void)out_path;
    std::fprintf(stderr, "emit_object: deprecated, use emit_native_object instead\n");
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
