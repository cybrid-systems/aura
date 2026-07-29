// ──────────────────────────────────────────────────────────────
//  opcode_display_names.hh — IROpcode display names (meta-free)
//
//  Wave A3: single source for kOpcodeInfo[].name. Safe to include
//  from module GMF (no <meta>). Order MUST match IROpcode ordinals
//  in ir.ixx / ir_pod_reflect.hh (0..53).
//
//  Identifier (PascalCase) ↔ display (kebab) alignment is checked
//  under -freflection in tests/reflect/test_opcode_info_align_a3.cpp.
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_OPCODE_DISPLAY_NAMES_HH
#define AURA_REFLECT_OPCODE_DISPLAY_NAMES_HH

#include <cstddef>
#include <string_view>

namespace aura::ir {

inline constexpr std::size_t kIrOpcodeDisplayNameCount = 54;

// Indexed by IROpcode ordinal. Keep in lockstep with the enum.
inline constexpr std::string_view kIrOpcodeDisplayNames[kIrOpcodeDisplayNameCount] = {
    "nop",           // 0  Nop
    "const-i64",     // 1  ConstI64
    "const-f64",     // 2  ConstF64
    "local",         // 3  Local
    "arg",           // 4  Arg
    "add",           // 5  Add
    "sub",           // 6  Sub
    "mul",           // 7  Mul
    "div",           // 8  Div
    "eq",            // 9  Eq
    "lt",            // 10 Lt
    "gt",            // 11 Gt
    "le",            // 12 Le
    "ge",            // 13 Ge
    "and",           // 14 And
    "or",            // 15 Or
    "not",           // 16 Not
    "branch",        // 17 Branch
    "jump",          // 18 Jump
    "call",          // 19 Call
    "return",        // 20 Return
    "make-closure",  // 21 MakeClosure
    "capture",       // 22 Capture
    "capture-ref",   // 23 CaptureRef
    "apply",         // 24 Apply
    "new-cell",      // 25 NewCell
    "cell-set",      // 26 CellSet
    "cell-get",      // 27 CellGet
    "cast",          // 28 CastOp (display drops trailing Op)
    "const-string",  // 29 ConstString
    "prim-call",     // 30 PrimCall
    "primitive",     // 31 Primitive
    "const-bool",    // 32 ConstBool
    "const-void",    // 33 ConstVoid
    "make-pair",     // 34 MakePair
    "car",           // 35 Car
    "cdr",           // 36 Cdr
    "raise",         // 37 Raise
    "is-error",      // 38 IsError
    "try-begin",     // 39 TryBegin
    "try-end",       // 40 TryEnd
    "hash-ref",      // 41 HashRef
    "hash-set",      // 42 HashSet
    "hash-remove",   // 43 HashRemove
    "linear-wrap",   // 44 LinearWrap
    "move-op",       // 45 MoveOp
    "borrow-op",     // 46 BorrowOp
    "mut-borrow-op", // 47 MutBorrowOp
    "drop-op",       // 48 DropOp
    "ref-count-op",  // 49 RefCountOp
    "arena-push",    // 50 ArenaPush
    "arena-pop",     // 51 ArenaPop
    "guard-shape",   // 52 GuardShape
    "top-cell-load", // 53 TopCellLoad
};

} // namespace aura::ir

#endif // AURA_REFLECT_OPCODE_DISPLAY_NAMES_HH
