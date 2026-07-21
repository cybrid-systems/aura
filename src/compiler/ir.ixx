module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module aura.compiler.ir;
import std;
import aura.core;

namespace aura::ir {

export enum class IROpcode : std::uint8_t {
    Nop,
    // Data
    ConstI64, // load int64 constant
    ConstF64, // load double constant
    Local,    // load local variable (slot index)
    Arg,      // load function argument (slot index)
    // Arithmetic
    Add,
    Sub,
    Mul,
    Div,
    // Comparison (return 0 or 1)
    Eq,
    Lt,
    Gt,
    Le,
    Ge,
    // Logic
    And,
    Or,
    Not,
    // Control flow
    Branch, // conditional branch: cond, true_target, false_target
    Jump,   // unconditional branch: target
    Call,   // function call: callee_expr, arg_count, result_slot
    Return, // return: value_slot
    // Closures
    MakeClosure, // create closure: func_slot, env_size
    Capture,     // capture variable: closure_slot, env_idx, var_slot
    CaptureRef,  // capture by reference: closure_slot, env_idx, cell_slot
    Apply,       // apply closure: closure_slot, arg_count, result_slot
    // Mutable cells (for letrec)
    NewCell, // allocate mutable cell: result_slot
    CellSet, // write to cell: cell_id, value_slot
    CellGet, // read from cell: result_slot, cell_id
    // Type coercion (L6.6b)
    CastOp, // runtime type check: result_slot, value_slot, type_tag
    // String support
    ConstString, // load string constant: result_slot, string_index
    // Primitive call (for non-arithmetic primitives like string ops)
    PrimCall, // call prim by id: prim_id, packed_args(arg_begin, arg_count), result_slot
    // Primitive value (load a primitive function value)
    Primitive, // load primitive value: result_slot, prim_slot_index
    // Boolean constant
    ConstBool, // load boolean constant: result_slot, value(0 or 1)
    // Void constant (empty list)
    ConstVoid, // load void: result_slot
    // Pair operations (native, avoids PrimCall dispatch)
    MakePair, // create pair: result_slot, car_slot, cdr_slot
    Car,      // car: result_slot, pair_slot
    Cdr,      // cdr: result_slot, pair_slot
    // Error handling
    Raise,   // raise error: result_slot, cause_slot
    IsError, // check if error: result_slot, value_slot
    // Issue #124: exception-handling opcodes for try/catch blocks.
    // TryBegin pushes an exception frame onto the runtime
    // exception stack; Raise unwinds to the nearest matching
    // TryBegin; TryEnd pops the frame. The interpreter (and
    // future JIT backend) maintains a stack of active try
    // regions to make Raise O(1) to find the handler.
    TryBegin, // start try: handler_block (branch target on Raise)
    TryEnd,   // end try (pop frame): result_slot
    // Catch is implicit: the TryBegin's handler_block is the
    // catch body. We don't have a separate Catch opcode because
    // the catch is structurally a regular block that starts
    // with a bound error variable. The handler_block's first
    // instruction is typically a Local that reads the exception
    // payload from a runtime slot.
    // Hash operations (inline, avoids PrimCall dispatch)
    HashRef,    // hash-ref: result, hash, key
    HashSet,    // hash-set!: result, hash, key, val
    HashRemove, // hash-remove!: result, hash, key
    // M4 Linear ownership
    LinearWrap,  // wrap linear value: result_slot, inner_slot
    MoveOp,      // move ownership: result_slot, inner_slot
    BorrowOp,    // immutable borrow: result_slot, inner_slot
    MutBorrowOp, // mutable borrow: result_slot, inner_slot
    DropOp,      // explicit destruct: inner_slot
    RefCountOp,  // runtime refcount: result_slot, inner_slot, inc(1)/dec(0)
    ArenaPush,   // push TL arena frame: result_slot (saved offset), size
    ArenaPop,    // pop TL arena frame: saved_offset_slot
    // Issue #61 Iter 2: lazy-deopt guard. Inserted at the entry
    // of a function specialized for a particular shape. The
    // expected shape id is in operands[2]; operands[3] is the
    // generic-trampoline block id (branch target on mismatch).
    // The guard writes a bool to operands[0]: 1 = matches, 0 = deopt.
    GuardShape, // deopt guard: result, arg_slot, expected_shape, generic_block
    // Issue #272 Cycle 3: load top-level value binding from evaluator cell heap.
    TopCellLoad, // TopCellLoad: result_slot, cell_index
};

// Issue #910 Phase 1: opcode count for dispatch-table migration.
export inline constexpr std::size_t kIROpcodeCount = 54;

// Issue #910 Phase 2: dense opcode class table for table-driven
// dispatch (hot path can branch on class then dense secondary
// switch, instead of one 54-way switch). Index = IROpcode ordinal.
export enum class IROpcodeClass : std::uint8_t {
    Nop = 0,
    Const,
    Load,    // Local / Arg / TopCellLoad
    Arith,   // Add Sub Mul Div
    Compare, // Eq Lt Gt Le Ge
    Logic,   // And Or Not
    Control, // Branch Jump Call Return Apply
    Closure, // MakeClosure Capture CaptureRef
    Cell,    // NewCell CellSet CellGet
    Cast,
    Pair,      // MakePair Car Cdr
    Exception, // Raise IsError TryBegin TryEnd
    Hash,      // HashRef HashSet HashRemove
    Linear,    // LinearWrap MoveOp BorrowOp MutBorrowOp DropOp RefCountOp
    Arena,     // ArenaPush ArenaPop
    Guard,     // GuardShape
    Prim,      // PrimCall Primitive
};

// Must stay in enum order of IROpcode (see above).
export inline constexpr IROpcodeClass kIROpcodeClass[kIROpcodeCount] = {
    IROpcodeClass::Nop,       // Nop
    IROpcodeClass::Const,     // ConstI64
    IROpcodeClass::Const,     // ConstF64
    IROpcodeClass::Load,      // Local
    IROpcodeClass::Load,      // Arg
    IROpcodeClass::Arith,     // Add
    IROpcodeClass::Arith,     // Sub
    IROpcodeClass::Arith,     // Mul
    IROpcodeClass::Arith,     // Div
    IROpcodeClass::Compare,   // Eq
    IROpcodeClass::Compare,   // Lt
    IROpcodeClass::Compare,   // Gt
    IROpcodeClass::Compare,   // Le
    IROpcodeClass::Compare,   // Ge
    IROpcodeClass::Logic,     // And
    IROpcodeClass::Logic,     // Or
    IROpcodeClass::Logic,     // Not
    IROpcodeClass::Control,   // Branch
    IROpcodeClass::Control,   // Jump
    IROpcodeClass::Control,   // Call
    IROpcodeClass::Control,   // Return
    IROpcodeClass::Closure,   // MakeClosure
    IROpcodeClass::Closure,   // Capture
    IROpcodeClass::Closure,   // CaptureRef
    IROpcodeClass::Control,   // Apply
    IROpcodeClass::Cell,      // NewCell
    IROpcodeClass::Cell,      // CellSet
    IROpcodeClass::Cell,      // CellGet
    IROpcodeClass::Cast,      // CastOp
    IROpcodeClass::Const,     // ConstString
    IROpcodeClass::Prim,      // PrimCall
    IROpcodeClass::Prim,      // Primitive
    IROpcodeClass::Const,     // ConstBool
    IROpcodeClass::Const,     // ConstVoid
    IROpcodeClass::Pair,      // MakePair
    IROpcodeClass::Pair,      // Car
    IROpcodeClass::Pair,      // Cdr
    IROpcodeClass::Exception, // Raise
    IROpcodeClass::Exception, // IsError
    IROpcodeClass::Exception, // TryBegin
    IROpcodeClass::Exception, // TryEnd
    IROpcodeClass::Hash,      // HashRef
    IROpcodeClass::Hash,      // HashSet
    IROpcodeClass::Hash,      // HashRemove
    IROpcodeClass::Linear,    // LinearWrap
    IROpcodeClass::Linear,    // MoveOp
    IROpcodeClass::Linear,    // BorrowOp
    IROpcodeClass::Linear,    // MutBorrowOp
    IROpcodeClass::Linear,    // DropOp
    IROpcodeClass::Linear,    // RefCountOp
    IROpcodeClass::Arena,     // ArenaPush
    IROpcodeClass::Arena,     // ArenaPop
    IROpcodeClass::Guard,     // GuardShape
    IROpcodeClass::Load,      // TopCellLoad
};

export inline constexpr IROpcodeClass opcode_class(IROpcode op) noexcept {
    const auto i = static_cast<std::size_t>(op);
    return i < kIROpcodeCount ? kIROpcodeClass[i] : IROpcodeClass::Nop;
}


// ── Issue #217 Cycle 3: IR types are reflection-ready ────────
//
// The IR types below (IRInstruction, OpcodeInfo, BasicBlock,
// IRFunction, IRModule) are plain POD structs that the
// C++26 P2996 reflection (reflect_members<T>()) sees
// automatically — no explicit REFLECT_MEMBERS annotation
// is required. The reflection-driven serialization
// (auto_serialize<T>(buf, obj) / auto_deserialize<T>(buf,
// pos)) works for these types out of the box.
//
// The migration scope per the issue body is:
//   1. Add a serialization path that uses auto_serialize
//      (currently the IR types are in-memory only; no
//      hand-written serializers exist in this file).
//   2. Replace the kOpcodeInfo[] table with a reflection-
//      driven dispatch table (Cycle 4 of #217).
//   3. Replace any AST type hand-written serializers
//      in src/core/ast.ixx with auto_serialize
//      (separate issue).
//
// The test in tests/test_issue_217.cpp verifies the
// reflection infrastructure works for IR-shaped types
// using local copies of these definitions (Cycle 1
// pilot) + a Roundtrip test on the real types via
// import (Test 8 — deferred to a future cycle because
// the test target's module imports currently conflict
// with arena.ixx's gc_hooks.h).

export struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
    std::uint32_t type_id = 0; // 0 = unknown/dynamic, from FlatAST type_id
    // Issue #60 Iter 1: shape_id from the per-function shape_map
    // (populated by FlatFnBuilder at IR-build time). 0 = unknown
    // or Dynamic. The JIT consumes this for L1 fast paths
    // (OpAdd etc.) and for L2 layout specialization
    // (OpPairAlloc, OpVectorRef, etc.).
    std::uint32_t shape_id = 0;

    // Issue #149 Phase 1: rich type metadata for specialization,
    // monomorphization, and GuardShape precision. All three
    // fields are 0/unknown by default — populated by the
    // lowering pass (Phase 2) when the inferred type carries
    // the corresponding information. Consumers (the
    // TypeSpecializationWrap pass, GuardShape, JIT) check
    // the field before using it; a 0 value is always "no
    // info, fall back to existing behavior".
    //
    // linear_ownership_state: 0=untracked, 1=Owned, 2=Borrowed,
    // 3=MutBorrowed, 4=Moved. Set on values that are linear-
    // typed (per the M4 Linear typing from OwnershipEnv).
    // Consumers: TypeSpecializationWrap elides MoveOp on
    // owned values; GuardShape treats a value known to be
    // linear-typed as non-Dynamic for the purposes of the
    // dynamic-check fast path.
    std::uint8_t linear_ownership_state = 0;
    // adt_variant_id: 0 = not an ADT or unknown variant.
    // Non-zero: the discriminant of the specific ADT variant
    // this instruction produces (OpCons) or expects (OpCar/
    // OpCdr matching). Set by the ADT-specialization pass
    // (Phase 3) when the variant is statically known.
    // Consumers: TypeSpecializationWrap can replace a generic
    // ADT call with a variant-specific block when this is set.
    std::uint32_t adt_variant_id = 0;
    // narrow_evidence: 0 = no narrowing evidence (use default
    // type). Non-zero: a bitmask of narrowing predicates that
    // have been applied to produce this value's type
    // (1 = number?, 2 = string?, 4 = list?, etc.). Set when
    // occurrence-narrowing has been applied in the if-then
    // branch. Consumers: TypeSpecializationWrap can skip
    // generic type-checks for predicates in the evidence
    // mask; the JIT can use precise unboxed representations.
    std::uint32_t narrow_evidence = 0;
    // Issue #455 / #1610: per-instruction SyntaxMarker snapshot
    // (0=User, 1=MacroIntroduced, 2=BoolLiteral). Populated
    // by the lowering pass from the source AST node's marker
    // (aura::ast::FlatAST::marker). The inliner consults this
    // to apply macro-hygiene policy at the call-site level
    // (in addition to the existing per-function check on
    // IRFunction::marker). A call site marked MacroIntroduced
    // is not inlined into user code by default. JIT FlatInstruction
    // mirrors this for conservative deopt / specialization policy.
    std::uint8_t source_marker = 0;
    // Issue #1610: AST provenance id (0 = none). Stamped from
    // FlatAST::provenance(source node) so IR/JIT/AOT can correlate
    // MacroIntroduced instructions back to macro expansion origin.
    std::uint32_t provenance = 0;
};

// ── Opcode metadata table ─────────────────────────────────────
// Describes properties of each IROpcode. Indexed by IROpcode enum value.
// Order MUST match the IROpcode enum exactly.
export struct OpcodeInfo {
    std::string_view name;
    std::uint8_t operand_count; // 0-4, how many operands are meaningful
    bool has_result_slot;       // true if operands[0] is the result slot
};

export constexpr OpcodeInfo kOpcodeInfo[] = {
    // 0  Nop
    {"nop", 0, false},
    // 1-4  Data
    {"const-i64", 1, true}, // ConstI64
    {"const-f64", 1, true}, // ConstF64
    {"local", 2, true},     // Local: result, src
    {"arg", 2, true},       // Arg: result, arg_slot
    // 5-8  Arithmetic
    {"add", 3, true}, // Add: result, a, b
    {"sub", 3, true},
    {"mul", 3, true},
    {"div", 3, true},
    // 9-13  Comparison
    {"eq", 3, true}, // Eq: result, a, b
    {"lt", 3, true},
    {"gt", 3, true},
    {"le", 3, true},
    {"ge", 3, true},
    // 14-16  Logic
    {"and", 3, true}, // And: result, a, b
    {"or", 3, true},
    {"not", 2, true}, // Not: result, a
    // 17-18  Control flow
    {"branch", 3, false}, // Branch: cond, true_block, false_block
    {"jump", 1, false},   // Jump: target_block
    // 19-20
    {"call", 4, false},   // Call: callee, arg_base, arg_count, result
    {"return", 1, false}, // Return: value
    // 21-24  Closures
    {"make-closure", 3, true}, // MakeClosure: result, func_id, env_size
    {"capture", 3, false},     // Capture: closure, env_idx, var
    {"capture-ref", 3, false}, // CaptureRef: closure, env_idx, cell
    {"apply", 4, false},       // Apply: closure, arg_base, arg_count, result
    // 25-27  Mutable cells
    {"new-cell", 1, true},  // NewCell: result
    {"cell-set", 2, false}, // CellSet: cell, value
    {"cell-get", 2, true},  // CellGet: result, cell
    // 28  Type coercion
    {"cast", 3, true}, // CastOp: result, value, type_tag
    // 29  String
    {"const-string", 2, true}, // ConstString: result, string_index
    // 30-31  Primitive
    {"prim-call", 3, true}, // PrimCall: prim_id, packed_args, result
    {"primitive", 2, true}, // Primitive: result, slot_index
    // 32-33  Constants
    {"const-bool", 2, true}, // ConstBool: result, value
    {"const-void", 1, true}, // ConstVoid: result
    // 34-36  Pair
    {"make-pair", 3, true}, // MakePair: result, car, cdr
    {"car", 2, true},       // Car: result, pair
    {"cdr", 2, true},       // Cdr: result, pair
    // 37-38  Error handling
    {"raise", 2, true},    // Raise: result, cause
    {"is-error", 2, true}, // IsError: result, value
    // Issue #124: try/catch exception support
    {"try-begin", 1, false}, // TryBegin: handler_block (no result)
    {"try-end", 1, true},    // TryEnd: result_slot (pop frame, value)
    // 39-41  Hash operations (inline, avoids PrimCall dispatch)
    {"hash-ref", 3, true},    // HashRef: result, hash, key
    {"hash-set", 3, true},    // HashSet: void, hash, keyval
    {"hash-remove", 3, true}, // HashRemove: void, hash, key
    // 42-47  M4 Linear ownership
    {"linear-wrap", 2, true},   // LinearWrap: result, inner
    {"move-op", 2, true},       // MoveOp: result, inner
    {"borrow-op", 2, true},     // BorrowOp: result, inner
    {"mut-borrow-op", 2, true}, // MutBorrowOp: result, inner
    {"drop-op", 1, false},      // DropOp: inner (no result)
    {"ref-count-op", 3, true},  // RefCountOp: result, inner, inc/dec
    {"arena-push", 2, true},    // ArenaPush: result, size
    {"arena-pop", 1, false},    // ArenaPop: saved_offset (no result)
    // Issue #61 Iter 2: GuardShape. result + arg + expected + generic_block.
    {"guard-shape", 4, true}, // GuardShape: result, arg, expected, generic
    // Issue #272 Cycle 3
    {"top-cell-load", 2, true}, // TopCellLoad: result, cell_index
};

static_assert(std::size(kOpcodeInfo) == 54, "kOpcodeInfo must have exactly one entry per IROpcode");

// Helper: look up opcode info by IROpcode enum value
export inline const OpcodeInfo* lookup_opcode(IROpcode op) {
    auto idx = static_cast<std::size_t>(op);
    return idx < std::size(kOpcodeInfo) ? &kOpcodeInfo[idx] : nullptr;
}

// Primitive IDs for PrimCall opcode
export enum class PrimId : std::uint8_t {
    Hash,
    HashLength,
    HashHasKey,
    HashKeys,
    HashValues,
    StringAppend,
    StringLength,
    StringRef,
    Substring,
    StringEq,
    StringLt,
    NumberToString,
    StringToNumber,
    Display,
    Write,
    Newline,
    Error,
    Assert,
    Read,
    ReadFile,
    WriteFile,
    FileExists,
    Gensym,
    // Apply: (apply fn list) — call fn with list elements as args
    Apply,
    // Vector primitives
    Vector,
    VectorRef,
    VectorSet,
    VectorLength,
    VectorP,
    MakeVector,
    // Module primitives
    Import,
    // Character operations (chars are integers in Aura)
    CharEq,
    CharLt,
    CharToInteger,
    IntegerToChar,
    // Arithmetic
    Quotient,
    Remainder,
    // List primitives
    ListLength,
    ListRef,
    ListReverse,
    // Error handling
    Raise,
    ErrorP,
    // Type predicates
    PairP,
    NullP,
};

// Names for each PrimId, indexed by enum value.
// Must match PrimId enum order exactly.
export constexpr std::string_view kPrimNames[] = {
    "hash",          "hash-length",    "hash-has-key?",  "hash-keys",     "hash-values",
    "string-append", "string-length",  "string-ref",     "substring",     "string=?",
    "string<?",      "number->string", "string->number", "display",       "write",
    "newline",       "error",          "assert",         "read",          "read-file",
    "write-file",    "file-exists?",   "gensym",         "apply",         "vector",
    "vector-ref",    "vector-set!",    "vector-length",  "vector?",       "make-vector",
    "import",        "char=?",         "char<?",         "char->integer", "integer->char",
    "quotient",      "remainder",      "length",         "list-ref",      "reverse",
    "raise",         "error?",         "pair?",          "null?",
};

static_assert(std::size(kPrimNames) == 44, "kPrimNames must have exactly one entry per PrimId");

// Helper: pack two uint32 into one (for Call: args_begin << 16 | arg_count)
export constexpr std::uint32_t pack_pair(std::uint32_t hi, std::uint32_t lo) {
    return (hi << 16) | (lo & 0xFFFF);
}
export constexpr std::uint32_t unpack_hi(std::uint32_t p) {
    return p >> 16;
}
export constexpr std::uint32_t unpack_lo(std::uint32_t p) {
    return p & 0xFFFF;
}

export struct BasicBlock {
    std::uint32_t id = 0;
    std::vector<IRInstruction> instructions;
    std::vector<std::uint32_t> successors;
};

export enum class Region : std::uint8_t {
    Default = 0,
    Performance = 1,
    Evolution = 2,
};

// Issue #151 P0 — Tiered Compilation Strategy. The tier
// chosen for an IRFunction determines the codegen path.
// Today's AOT / JIT / Interpreter all live in separate
// compilation units (aura_jit.cpp, aura_jit_bridge.cpp,
// ir_executor_impl.cpp); the dispatch decision is the
// follow-up. This enum + tier_for_region is the
// classification scheme that the dispatch will consult.
export enum class CompilationTier : std::uint8_t {
    AOT = 0,         // ahead-of-time: persistent cache, O3
    JIT = 1,         // just-in-time: ORC, full shape guards
    Interpreter = 2, // IR interpreter: dynamic fallback
};

// Issue #151: map Region -> CompilationTier. The mapping
// is the heart of the tiered strategy:
//   - Performance: AOT (stable, hot, benefits most from
//     O3 + persistent cache; GuardShape deopts minimized)
//   - Evolution:   JIT (full shape guards, easier
//     invalidation on mutation; the AOT path's persistent
//     cache would be invalidated too often for evolving code)
//   - Default:     JIT (balanced — the existing default path)
//
// Returns the tier for a given region. Callers consult
// this when deciding the codegen path for a function.
export constexpr CompilationTier tier_for_region(Region r) {
    switch (r) {
        case Region::Performance:
            return CompilationTier::AOT;
        case Region::Evolution:
            return CompilationTier::JIT;
        case Region::Default:
        default:
            return CompilationTier::JIT;
    }
}

// Issue #150 P0 — Static/Dynamic Region Analysis. The
// Region enum above is the data model; today the
// FlatFunction::region field (set by the parser / IR
// builder) is mostly 0 (Default) because no caller
// explicitly sets it. Issue #150 Phase 1 (lightweight
// annotations) would wire:
//   (performance-region (define (hot-compute ...) ...))
//   (evolution-region (define (self-modify ...) ...))
// Aura forms that the parser recognizes and sets the
// corresponding Region value on the FlatFunction / IRFunction.
//
// Full 3-phase roadmap (estimated 4-6 commits, like #148
// and #149):
//   Phase 1: explicit Aura forms (performance-region /
//     evolution-region) + parser / lowering support to
//     set FlatFunction::region accordingly.
//   Phase 2: automatic inference pass — walk the call
//     graph, detect mutate:* / eval-current / ast:* calls,
//     and set region on functions that escape mutation.
//     Functions only called from performance regions
//     inherit Performance.
//   Phase 3: mutation impact analysis — detect which
//     functions are affected by a specific mutate:rebind
//     and only invalidate/recompile those (not global cache).
//
// Today ships Phase 0 (prep): the enum + a 28-line design
// comment. The existing Region field is preserved (Default
// is the current behavior for everything not annotated).
// No behavior change.

export struct IRFunction {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t entry_block = 0;
    std::vector<BasicBlock> blocks;
    std::vector<std::string> params;
    std::vector<std::string> free_vars;
    std::uint32_t local_count = 0; // number of local slots needed
    std::uint32_t arg_count = 0;   // number of arguments
    bool variadic = false;         // dotted rest param
    Region region = Region::Default;
    // Issue #246: SyntaxMarker from the source AST node that
    // produced this IRFunction. 0 = User (default), 1 =
    // MacroIntroduced (lambda body inserted by a hygienic
    // macro via clone_macro_body), 2 = BoolLiteral.
    // Mirrors aura::ast::SyntaxMarker. The InlinePass consults
    // this to decide whether to be conservative with macro-
    // introduced code (respect_macro_hygiene_ flag, default on).
    std::uint8_t marker = 0;
    // Issue #61 Iter 1: shape this function was specialized for.
    // 0 = no specialization (generic version). When non-zero, the
    // IRFunction entry must contain a GuardShape (Iter 2) that
    // checks the actual arg shape against this and deopts to
    // generic_id on mismatch.
    std::uint32_t specialized_for = 0;
    // Index into IRModule.functions[] of the un-specialized
    // counterpart. 0xFFFFFFFF = "no generic version" (deopt
    // falls back to the interpreter). When the function is itself
    // generic (specialized_for == 0), generic_id is ignored.
    std::uint32_t generic_id = 0xFFFFFFFF;

    // Issue #160: escape analysis result. Filled by
    // EscapeAnalysisPass. Size = local_count. 0 = NON_ESCAPING
    // (slot is only used locally), 1 = ESCAPED (slot is returned,
    // passed to a call, stored in a cell/hash, captured in a
    // closure, etc.). Consumers (JIT, arena-aware allocation,
    // future stack promotion) check this map to enable
    // optimizations — e.g., a non-escaping pair can be
    // arena-allocated instead of heap-allocated.
    std::vector<std::uint8_t> escape_map;
};

// Closure bridge data: original tree-walker info for IR closures.
// Only populated when the lowered closure has corresponding FlatAST data
// (i.e. for lambda forms, not for cached defines).
//
// Issue #224 Cycle 2: shared_ptr-based ownership. The shared_ptr
// keeps the FlatAST/StringPool alive as long as any bridge holds
// a reference — even after `arena_.reset()`. The arena is the
// actual owner; the shared_ptr is a non-owning view (constructed
// with a no-op deleter). This makes the bridge's lifetime
// independent of the lowering arena, avoiding use-after-free
// when a long-lived closure outlives the arena that produced it.
export struct ClosureBridgeData {
    std::shared_ptr<const ast::FlatAST> flat;    // shared view of arena-owned FlatAST
    std::shared_ptr<const ast::StringPool> pool; // same
    ast::NodeId body_id = ast::NULL_NODE;
    std::string body_source; // serialized source for bridge fallback re-parse
    // Issue #223: epoch captured at bridge construction. If the
    // service's bridge_epoch() returns a different value, the
    // bridge's flat*/pool* are stale (arena was reset, or a major
    // mutation invalidated the captured pointers). The bridge
    // callback / apply_closure compares this against the current
    // epoch and either re-parses from body_source or invalidates
    // the closure. Default 0 = "unset" (legacy bridge); Cycle 1
    // captures the current epoch explicitly via set_closure_bridge.
    std::uint64_t bridge_epoch = 0;
    // Issue #1616: SyntaxMarker + provenance from lambda/macro body AST
    // (0=User, 1=MacroIntroduced). Propagated into IRClosure at MakeClosure.
    std::uint8_t source_marker = 0;
    std::uint32_t provenance = 0;
};

export struct IRModule {
    std::vector<IRFunction> functions;
    std::vector<ClosureBridgeData> closure_bridge; // indexed by func_id
    std::uint32_t entry_function_id = 0;

    std::uint32_t add_function(IRFunction func) {
        func.id = static_cast<std::uint32_t>(functions.size());
        functions.push_back(std::move(func));
        // Ensure closure_bridge is in sync
        if (closure_bridge.size() < functions.size())
            closure_bridge.resize(functions.size());
        return func.id;
    }

    // Set bridge data for a function (for cross-evaluator lambda calls)
    // Issue #223: epoch is the bridge_epoch_ captured at this point;
    // a future mismatch with the service's bridge_epoch() signals
    // the bridge's flat*/pool* are stale.
    //
    // Issue #224 Cycle 2: takes shared_ptr by value. The shared_ptr
    // is a non-owning view (constructed with a no-op deleter at
    // the call site) that keeps the FlatAST alive as long as the
    // bridge exists.
    void set_closure_bridge(std::uint32_t func_id, std::shared_ptr<const ast::FlatAST> flat,
                            std::shared_ptr<const ast::StringPool> pool, ast::NodeId body_id,
                            std::uint64_t epoch = 0) {
        if (func_id < functions.size()) {
            ClosureBridgeData bd;
            bd.flat = std::move(flat);
            bd.pool = std::move(pool);
            bd.body_id = body_id;
            bd.bridge_epoch = epoch;
            // Issue #1616: stamp SyntaxMarker + provenance from body AST.
            if (bd.flat && body_id != ast::NULL_NODE && body_id < bd.flat->size()) {
                bd.source_marker = static_cast<std::uint8_t>(bd.flat->marker(body_id));
                bd.provenance = bd.flat->provenance(body_id);
            } else if (func_id < functions.size()) {
                bd.source_marker = functions[func_id].marker;
            }
            closure_bridge[func_id] = std::move(bd);
        }
    }

    // Set bridge data by pointer (for cached function injection)
    // Issue #224 Cycle 2: takes shared_ptr by value. The shared_ptr
    // is a non-owning view (constructed with a no-op deleter at
    // the call site).
    void set_closure_bridge_ptr(std::uint32_t func_id, std::shared_ptr<const ast::FlatAST> flat,
                                std::shared_ptr<const ast::StringPool> pool, ast::NodeId body_id,
                                std::uint64_t epoch = 0) {
        if (func_id < closure_bridge.size()) {
            ClosureBridgeData bd;
            bd.flat = std::move(flat);
            bd.pool = std::move(pool);
            bd.body_id = body_id;
            bd.bridge_epoch = epoch;
            // Issue #1616: stamp SyntaxMarker + provenance from body AST.
            if (bd.flat && body_id != ast::NULL_NODE && body_id < bd.flat->size()) {
                bd.source_marker = static_cast<std::uint8_t>(bd.flat->marker(body_id));
                bd.provenance = bd.flat->provenance(body_id);
            } else if (func_id < functions.size()) {
                bd.source_marker = functions[func_id].marker;
            }
            closure_bridge[func_id] = std::move(bd);
        }
    }

    void set_closure_body_source(std::uint32_t func_id, const std::string& src) {
        if (func_id < closure_bridge.size()) {
            closure_bridge[func_id].body_source = src;
        }
    }

    std::vector<std::string> string_pool; // string constants (for ConstString opcode)

    // Deduplicated string pool (Phase 4e: JIT hash string keys)
    std::unordered_map<std::string, std::uint32_t, aura::core::TransparentStringHash,
                       std::equal_to<>>
        string_map_;

    std::uint32_t add_string(std::string s) {
        auto it = string_map_.find(s);
        if (it != string_map_.end())
            return it->second;
        auto id = static_cast<std::uint32_t>(string_pool.size());
        string_pool.push_back(s);
        string_map_[std::move(s)] = id;
        return id;
    }

    void set_entry(std::uint32_t id) { entry_function_id = id; }
    IRFunction& entry() { return functions[entry_function_id]; }
    const IRFunction& entry() const { return functions[entry_function_id]; }

    // Hot-swap a function: replace the body while keeping the same id
    // All existing closures referencing func_id will use the new code
    // on their next invocation.
    bool hot_swap_function(std::uint32_t func_id, IRFunction new_func) {
        if (func_id >= functions.size())
            return false;
        new_func.id = func_id;
        functions[func_id] = std::move(new_func);
        return true;
    }

    // ── 类型驱动 IR 优化 ───────────────────────────────────
    // 基于 type_id 信息消除冗余 CastOp。当值类型已知且匹配目标
    // 类型时，CastOp 是多余的，直接替换引用为原始值。
    // 同时传播类型信息，减少运行时类型检查。
    void optimize_type_info() {
        for (auto& func : functions) {
            for (auto& block : func.blocks) {
                // First pass: collect known type for each slot
                struct SlotType {
                    std::uint32_t type_id = 0;
                    std::uint32_t source_slot = 0; // 0 = self
                    bool is_cast = false;
                };
                std::vector<SlotType> slot_types(func.local_count + func.arg_count + 4);
                std::vector<std::uint32_t> slot_remap; // result_slot -> source_slot
                slot_remap.resize(func.local_count + func.arg_count + 4,
                                  std::numeric_limits<std::uint32_t>::max());

                for (auto& instr : block.instructions) {
                    // Record known type for this instruction's result
                    if (auto* info = lookup_opcode(instr.opcode)) {
                        if (info->has_result_slot && instr.operands[0] < slot_types.size()) {
                            auto& st = slot_types[instr.operands[0]];
                            if (instr.type_id != 0) {
                                st.type_id = instr.type_id;
                                st.source_slot = instr.operands[0];
                            }
                        }
                    }

                    // Eliminate redundant CastOp
                    if (instr.opcode == IROpcode::CastOp) {
                        auto val_slot = instr.operands[1];
                        auto target_tag = instr.operands[2];
                        auto result_slot = instr.operands[0];

                        // Check if value already has the target type
                        bool redundant = false;
                        if (val_slot < slot_types.size() && slot_types[val_slot].type_id != 0) {
                            if (slot_types[val_slot].type_id == target_tag) {
                                redundant = true;
                            }
                        }

                        if (redundant) {
                            // Mark for replacement: result_slot -> val_slot
                            if (result_slot < slot_remap.size())
                                slot_remap[result_slot] = val_slot;
                            // Also propagate type
                            if (result_slot < slot_types.size() && val_slot < slot_types.size())
                                slot_types[result_slot] = slot_types[val_slot];
                        }
                    }
                }

                // Apply remapping: replace all uses of redundant cast results
                // with the original source slot
                for (auto& instr : block.instructions) {
                    if (instr.opcode == IROpcode::CastOp)
                        continue; // skip CastOps we already marked
                    // Don't skip op==0 — a valid slot can be 0
                    for (auto& op : instr.operands) {
                        if (op < slot_remap.size() &&
                            slot_remap[op] != std::numeric_limits<std::uint32_t>::max() &&
                            slot_remap[op] != op) {
                            // Follow the remap chain
                            auto src = slot_remap[op];
                            while (src < slot_remap.size() && slot_remap[src] != 0 &&
                                   slot_remap[src] != src)
                                src = slot_remap[src];
                            op = src;
                        }
                    }
                }

                // Second pass: remove redundant CastOps (no operands reference them)
                std::erase_if(block.instructions, [&](const IRInstruction& instr) {
                    if (instr.opcode == IROpcode::CastOp) {
                        auto result_slot = instr.operands[0];
                        return result_slot < slot_remap.size() &&
                               slot_remap[result_slot] != std::numeric_limits<std::uint32_t>::max();
                    }
                    return false;
                });
            }
        }
    }

    // Find all Call/MakeClosure instructions that reference a function
    std::vector<std::pair<std::uint32_t, std::uint32_t>>
    find_callers_of(std::uint32_t func_id) const {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> result;
        for (std::size_t fi = 0; fi < functions.size(); ++fi) {
            auto& f = functions[fi];
            for (std::size_t bi = 0; bi < f.blocks.size(); ++bi) {
                auto& b = f.blocks[bi];
                for (std::size_t ii = 0; ii < b.instructions.size(); ++ii) {
                    auto& instr = b.instructions[ii];
                    if (instr.opcode == IROpcode::MakeClosure && instr.operands.size() > 1 &&
                        instr.operands[1] == func_id) {
                        result.emplace_back(static_cast<std::uint32_t>(fi),
                                            static_cast<std::uint32_t>(ii));
                    }
                }
            }
        }
        return result;
    }
};

// ── Issue #375: IR encoding observability snapshot ──
//
// Free function + struct pair that captures the AoS-vs-compact
// encoding characteristics of an IRModule. Used by:
//   - CompilerService: snapshots the last compiled module's
//     stats whenever last_ir_mod_ is set, so the
//     (compile:ir-stats) Aura primitive + the C++ test API
//     both read consistent numbers (no "primitive sees its
//     own IR" clobber problem).
//   - test_issue_375: verifies the field values on known
//     workloads.
//
// The "compact projection" here is a *baseline estimate*
// (variable-length: 2-byte header + 4 bytes per used operand
// rounded to 4 bytes), not a shipped encoding. The follow-up
// #375 Slice B (compact dual view) will define the actual
// production encoding; this estimate is enough to validate
// the ≥30% size-reduction AC1.
export struct IRStatsSnapshot {
    std::uint64_t total_instructions = 0;
    std::uint64_t total_functions = 0;
    std::uint64_t total_blocks = 0;
    // Operand count distribution: index = operand count (0..4),
    // value = how many instructions use that many operands.
    std::array<std::uint64_t, 5> operand_count_distribution = {0, 0, 0, 0, 0};
    // Opcode histogram: index = IROpcode enum value, value = count.
    // 54 opcodes today (see kOpcodeInfo).
    std::array<std::uint64_t, 54> opcode_histogram = {};
    std::uint64_t operands_used_sum = 0;
    // AoS layout: 40 bytes per IRInstruction (1 opcode + 16 ops
    // + 4 src + 4 type + 4 shape + 1 linear + 3 pad + 4 adt + 4
    // narrow + 1 marker = 40). Verified by counting fields in
    // IRInstruction; matches sizeof on aarch64.
    static constexpr std::uint64_t AOS_BYTES_PER_INSTRUCTION = 40;
    static constexpr std::uint64_t PADDING_BYTES_PER_INSTRUCTION = 3;
    std::uint64_t aos_bytes_total = 0;
    std::uint64_t padding_bytes_total = 0;
    std::uint64_t unused_operand_bytes_total = 0;
    // Compact projection: variable-length (2-byte header +
    // 4 bytes per used operand, rounded to 4-byte alignment).
    // Metadata sidecar (type_id, shape_id, adt_variant_id,
    // narrow_evidence, source_marker, linear_state) lives in
    // a separate column that passes touch but the interpreter
    // switch dispatch doesn't.
    std::uint64_t compact_bytes_projection = 0;
    // compact / aos in basis points (0-10000). 3000 bp = compact
    // is 30% of aos (i.e. 70% reduction). The #375 AC is
    // "≥30% size reduction" so a ratio ≤ 7000 bp is a pass.
    std::uint64_t compact_ratio_bp = 0;
};

export inline IRStatsSnapshot compute_ir_stats(const IRModule& mod) {
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>
    IRStatsSnapshot s;
    s.total_functions = mod.functions.size();
    for (const auto& func : mod.functions) {
        for (const auto& block : func.blocks) {
            ++s.total_blocks;
            for (const auto& instr : block.instructions) {
                ++s.total_instructions;
                auto op_idx = static_cast<std::size_t>(instr.opcode);
                if (op_idx < s.opcode_histogram.size()) {
                    ++s.opcode_histogram[op_idx];
                }
                std::uint8_t used = 0;
                if (op_idx < 54) {
                    // Trust kOpcodeInfo for known opcodes (Branch etc.
                    // put target IDs in operands that can be 0 — the
                    // "count non-zero trailing operands" heuristic
                    // would miscount them).
                    used = kOpcodeInfo[op_idx].operand_count;
                } else {
                    // Out-of-range opcode: fall back to a trailing-
                    // non-zero heuristic.
                    if (instr.operands[0] != 0)
                        used = std::max<std::uint8_t>(used, 1);
                    if (instr.operands[1] != 0)
                        used = std::max<std::uint8_t>(used, 2);
                    if (instr.operands[2] != 0)
                        used = std::max<std::uint8_t>(used, 3);
                    if (instr.operands[3] != 0)
                        used = std::max<std::uint8_t>(used, 4);
                }
                if (used > 4)
                    used = 4;
                s.operand_count_distribution[used] += 1;
                s.operands_used_sum += used;
            }
        }
    }
    s.aos_bytes_total = s.total_instructions * IRStatsSnapshot::AOS_BYTES_PER_INSTRUCTION;
    s.padding_bytes_total = s.total_instructions * IRStatsSnapshot::PADDING_BYTES_PER_INSTRUCTION;
    s.unused_operand_bytes_total = (s.total_instructions * 4u - s.operands_used_sum) * 4u;
    for (std::size_t i = 0; i < 5; ++i) {
        std::uint64_t per_inst = 2 + static_cast<std::uint64_t>(i) * 4;
        per_inst = (per_inst + 3) & ~std::uint64_t(3);
        s.compact_bytes_projection += per_inst * s.operand_count_distribution[i];
    }
    s.compact_ratio_bp =
        s.aos_bytes_total ? (s.compact_bytes_projection * 10000u / s.aos_bytes_total) : 0;
    return s;
}

} // namespace aura::ir
