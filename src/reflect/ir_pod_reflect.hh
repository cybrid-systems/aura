// ──────────────────────────────────────────────────────────────
//  ir_pod_reflect.hh — Phase 4 kickoff (#2291)
//
//  Pure-POD IR types serialized / validated only via P2996 helpers
//  (auto_serialize / auto_deserialize / auto_validate / to_json).
//  No field-by-field write loops.
//
//  Migration pattern for the next batch:
//    1. Flat POD (arithmetic / enum / std::array; no virtuals,
//       no private SoA, prefer std::array over C arrays).
//    2. Keep layout in sync with ir.ixx (mirror until a type can
//       live in a non-import-std -freflection TU without std module).
//    3. Serialize/validate/inspect only via auto_serialize /
//       auto_deserialize / auto_validate / to_json.
//    4. Round-trip under -freflection (test_ir_pod_phase4.cpp).
//
//  Non-goals: nested containers as primary types, full AST SoA,
//  or dropping aura-reflect while business TUs still import std.
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_IR_POD_REFLECT_HH
#define AURA_REFLECT_IR_POD_REFLECT_HH

#include "reflect/reflect.hh"
#include "reflect/opcode_reflect.hh"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aura::ir_pod {

// Mirror of aura::ir::IROpcode / Region — sequential enums only.
// Keep ordinals in sync with ir.ixx (kIROpcodeCount == 54).
enum class IROpcode : std::uint8_t {
    Nop,
    ConstI64,
    ConstF64,
    Local,
    Arg,
    Add,
    Sub,
    Mul,
    Div,
    Eq,
    Lt,
    Gt,
    Le,
    Ge,
    And,
    Or,
    Not,
    Branch,
    Jump,
    Call,
    Return,
    MakeClosure,
    Capture,
    CaptureRef,
    Apply,
    NewCell,
    CellSet,
    CellGet,
    CastOp,
    ConstString,
    PrimCall,
    Primitive,
    ConstBool,
    ConstVoid,
    MakePair,
    Car,
    Cdr,
    Raise,
    IsError,
    TryBegin,
    TryEnd,
    HashRef,
    HashSet,
    HashRemove,
    LinearWrap,
    MoveOp,
    BorrowOp,
    MutBorrowOp,
    DropOp,
    RefCountOp,
    ArenaPush,
    ArenaPop,
    GuardShape,
    TopCellLoad,
};

enum class Region : std::uint8_t {
    Default = 0,
    Performance = 1,
    Evolution = 2,
};

// ── POD 1: IRInstruction (matches ir.ixx field set) ───────────
struct IRInstruction {
    IROpcode opcode = IROpcode::Nop;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
    std::uint32_t type_id = 0;
    std::uint32_t shape_id = 0;
    std::uint8_t linear_ownership_state = 0;
    std::uint32_t adt_variant_id = 0;
    std::uint32_t narrow_evidence = 0;
    std::uint8_t source_marker = 0;
    std::uint32_t provenance = 0;
};

// ── POD 2: flat function header (no vectors / strings) ────────
// Slice of IRFunction scalars for cache keys / light wire format.
struct IRFunctionHeader {
    std::uint32_t id = 0;
    std::uint32_t entry_block = 0;
    std::uint32_t local_count = 0;
    std::uint32_t arg_count = 0;
    bool variadic = false;
    Region region = Region::Default;
    std::uint8_t marker = 0;
    std::uint32_t specialized_for = 0;
    std::uint32_t generic_id = 0xFFFFFFFFu;
};

// ── POD 3: opcode arity descriptor (OpcodeInfo without name) ──
// Names come from opcode_reflect; this carries only numeric meta.
struct OpcodeArity {
    std::uint8_t operand_count = 0;
    bool has_result_slot = false;
};

// ── Reflection-only helpers (no field loops) ──────────────────

template <typename T> inline std::vector<char> pod_serialize(const T& obj) {
    return aura::reflect::auto_serialize(obj);
}

template <typename T> inline T pod_deserialize(const std::vector<char>& bytes) {
    return aura::reflect::auto_deserialize<T>(bytes);
}

template <typename T> inline bool pod_validate(const T& obj, std::string* error = nullptr) {
    return aura::reflect::auto_validate(obj, error);
}

template <typename T> inline std::string pod_json(const T& obj) {
    return aura::reflect::to_json(obj);
}

// Semantic validate on top of structural auto_validate.
inline bool validate_instruction(const IRInstruction& ins, std::string* error = nullptr) {
    if (!pod_validate(ins, error))
        return false;
    const auto n = aura::reflect::enum_count<IROpcode>();
    if (static_cast<std::size_t>(ins.opcode) >= n) {
        if (error)
            *error = "IRInstruction.opcode out of range";
        return false;
    }
    if (ins.linear_ownership_state > 4) {
        if (error)
            *error = "IRInstruction.linear_ownership_state out of range";
        return false;
    }
    if (ins.source_marker > 2) {
        if (error)
            *error = "IRInstruction.source_marker out of range";
        return false;
    }
    return true;
}

inline bool validate_function_header(const IRFunctionHeader& h, std::string* error = nullptr) {
    if (!pod_validate(h, error))
        return false;
    if (static_cast<std::uint8_t>(h.region) > 2) {
        if (error)
            *error = "IRFunctionHeader.region out of range";
        return false;
    }
    if (h.marker > 2) {
        if (error)
            *error = "IRFunctionHeader.marker out of range";
        return false;
    }
    if (h.local_count > 10'000'000u) {
        if (error)
            *error = "IRFunctionHeader.local_count too large";
        return false;
    }
    return true;
}

inline bool validate_opcode_arity(const OpcodeArity& a, std::string* error = nullptr) {
    if (!pod_validate(a, error))
        return false;
    if (a.operand_count > 4) {
        if (error)
            *error = "OpcodeArity.operand_count > 4";
        return false;
    }
    return true;
}

// Compile-time layout anchors (fail if mirrors drift).
static_assert(aura::reflect::enum_count<IROpcode>() == 54);
static_assert(aura::reflect::member_count<IRInstruction>() == 10);
static_assert(aura::reflect::member_count<IRFunctionHeader>() == 9);
static_assert(aura::reflect::member_count<OpcodeArity>() == 2);

} // namespace aura::ir_pod

#endif // AURA_REFLECT_IR_POD_REFLECT_HH
