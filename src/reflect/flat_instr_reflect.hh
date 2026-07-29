// ──────────────────────────────────────────────────────────────
//  flat_instr_reflect.hh — Wave B2 FlatInstruction wire helpers
//
//  Uses production aura::jit::FlatInstruction (aura_jit.h) with
//  auto_serialize / auto_deserialize / to_json. Overlap with
//  ir_pod::IRInstruction is documented for field mapping tests.
//
//  Mapping (IR → Flat):
//    opcode (enum u8)     → opcode (u32 ordinal)
//    operands[4]          → ops[4]
//    type_id, shape_id, linear_ownership_state, narrow_evidence,
//    source_marker, provenance — same names / roles
//    source_ast_node_id, adt_variant_id — IR-only
//    dirty                — Flat-only (SoA dirty bit)
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_FLAT_INSTR_REFLECT_HH
#define AURA_REFLECT_FLAT_INSTR_REFLECT_HH

#include "compiler/aura_jit.h"
#include "reflect/ir_pod_reflect.hh"
#include "reflect/reflect.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace aura::reflect::flat_instr {

using FlatInstruction = aura::jit::FlatInstruction;
using IRInstruction = aura::ir_pod::IRInstruction;

inline std::vector<char> serialize(const FlatInstruction& inst) {
    return auto_serialize(inst);
}

inline FlatInstruction deserialize(const std::vector<char>& bytes) {
    return auto_deserialize<FlatInstruction>(bytes);
}

inline std::string json(const FlatInstruction& inst) {
    return to_json(inst);
}

inline bool validate(const FlatInstruction& inst, std::string* error = nullptr) {
    if (!auto_validate(inst, error))
        return false;
    // IROpcode count is 54 (Nop..TopCellLoad)
    if (inst.opcode >= 54) {
        if (error)
            *error = "FlatInstruction.opcode out of range";
        return false;
    }
    if (inst.linear_ownership_state > 4) {
        if (error)
            *error = "FlatInstruction.linear_ownership_state out of range";
        return false;
    }
    if (inst.source_marker > 2) {
        if (error)
            *error = "FlatInstruction.source_marker out of range";
        return false;
    }
    return true;
}

// Project overlapping IR fields into a FlatInstruction (debug/tests).
inline FlatInstruction from_ir(const IRInstruction& ir, std::uint8_t dirty = 0) {
    FlatInstruction f{};
    f.opcode = static_cast<std::uint32_t>(ir.opcode);
    for (std::size_t i = 0; i < 4; ++i)
        f.ops[i] = ir.operands[i];
    f.shape_id = ir.shape_id;
    f.narrow_evidence = ir.narrow_evidence;
    f.type_id = ir.type_id;
    f.linear_ownership_state = ir.linear_ownership_state;
    f.dirty = dirty;
    f.source_marker = ir.source_marker;
    f.provenance = ir.provenance;
    return f;
}

// Copy overlapping fields back into an IRInstruction (loses IR-only fields).
inline IRInstruction to_ir_overlap(const FlatInstruction& f) {
    IRInstruction ir{};
    if (f.opcode < 54)
        ir.opcode = static_cast<aura::ir_pod::IROpcode>(f.opcode);
    for (std::size_t i = 0; i < 4; ++i)
        ir.operands[i] = f.ops[i];
    ir.shape_id = f.shape_id;
    ir.narrow_evidence = f.narrow_evidence;
    ir.type_id = f.type_id;
    ir.linear_ownership_state = f.linear_ownership_state;
    ir.source_marker = f.source_marker;
    ir.provenance = f.provenance;
    return ir;
}

// Compile-time member anchors
static_assert(member_count<FlatInstruction>() == 9);
static_assert(member_count<IRInstruction>() == 10);

} // namespace aura::reflect::flat_instr

#endif // AURA_REFLECT_FLAT_INSTR_REFLECT_HH
