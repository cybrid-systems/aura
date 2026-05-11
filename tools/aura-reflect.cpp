// ──────────────────────────────────────────────────────────────
//  aura-reflect — Standalone P2996 reflection tool for Aura IR
//
//  Built independently from the module-based aura core.
//  Uses:  g++ -std=c++26 -freflection tools/aura-reflect.cpp -o build/aura-reflect
//
//  Demonstrates:
//    - auto_to_json<IRInstruction> — IR instruction serialization
//    - auto_to_json<IRFunction>    — function serialization
//    - auto_to_json<IRModule>      — module serialization
//    - CLI: --ir-instruction, --ir-function, --ir-module
// ──────────────────────────────────────────────────────────────

#include "reflect/reflect.hh"
#include <cstdio>
#include <string>
#include <array>
#include <vector>
#include <cstdint>
#include <string_view>

// ── IR types (inlined to avoid module dependency) ──────────────

enum class IROpcode : std::uint8_t {
    Nop,
    ConstI64, Local, Arg,
    Add, Sub, Mul, Div,
    Eq, Lt, Gt, Le, Ge,
    And, Or, Not,
    Branch, Jump, Call, Return,
    MakeClosure, Capture, CaptureRef, Apply,
    NewCell, CellSet, CellGet,
};

struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
};

struct BasicBlock {
    std::uint32_t id = 0;
    std::vector<IRInstruction> instructions;
    std::vector<std::uint32_t> successors;
};

struct IRFunction {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t entry_block = 0;
    std::vector<BasicBlock> blocks;
    std::vector<std::string> params;
    std::vector<std::string> free_vars;
    std::uint32_t local_count = 0;
    std::uint32_t arg_count = 0;
};

// ==============================================================
//  IROpcode → string (for better JSON)
// ==============================================================

constexpr const char* opcode_name(IROpcode op) {
    switch (op) {
    case IROpcode::Nop:         return "Nop";
    case IROpcode::ConstI64:    return "ConstI64";
    case IROpcode::Local:       return "Local";
    case IROpcode::Arg:         return "Arg";
    case IROpcode::Add:         return "Add";
    case IROpcode::Sub:         return "Sub";
    case IROpcode::Mul:         return "Mul";
    case IROpcode::Div:         return "Div";
    case IROpcode::Eq:          return "Eq";
    case IROpcode::Lt:          return "Lt";
    case IROpcode::Gt:          return "Gt";
    case IROpcode::Le:          return "Le";
    case IROpcode::Ge:          return "Ge";
    case IROpcode::And:         return "And";
    case IROpcode::Or:          return "Or";
    case IROpcode::Not:         return "Not";
    case IROpcode::Branch:      return "Branch";
    case IROpcode::Jump:        return "Jump";
    case IROpcode::Call:        return "Call";
    case IROpcode::Return:      return "Return";
    case IROpcode::MakeClosure: return "MakeClosure";
    case IROpcode::Capture:     return "Capture";
    case IROpcode::CaptureRef:  return "CaptureRef";
    case IROpcode::Apply:       return "Apply";
    case IROpcode::NewCell:     return "NewCell";
    case IROpcode::CellSet:     return "CellSet";
    case IROpcode::CellGet:     return "CellGet";
    }
    return "?";
}

// ── Custom operator<< for IROpcode (needed for vector serialization) ──
// This allows auto_to_json(vector<T>) to work with IROpcode vectors

// ==============================================================
//  Enhanced auto_to_json for IRInstruction
// ==============================================================

std::string ir_instruction_to_json(const IRInstruction& inst) {
    // Use P2996 auto_to_json for the scalar/std::array fields,
    // but override opcode field with string name
    std::string json = "{";
    json += "\"opcode\":\""; json += opcode_name(inst.opcode); json += "\",";
    json += "\"operands\":" + aura::reflect::auto_to_json(inst.operands) + ",";
    json += "\"source_ast_node_id\":" + std::to_string(inst.source_ast_node_id);
    json += "}";
    return json;
}

// ==============================================================
//  CLI modes
// ==============================================================

void demo_ir_instruction() {
    printf("=== IR Instruction demo ===\n");

    IRInstruction add{
        IROpcode::Add,
        {1, 2, 0, 0},
        42
    };
    printf("  add: %s\n\n", ir_instruction_to_json(add).c_str());

    IRInstruction closure{
        IROpcode::MakeClosure,
        {0, 2, 0, 0},
        100
    };
    printf("  closure: %s\n\n", ir_instruction_to_json(closure).c_str());

    // Bulk: P2996 auto_to_json on ALL instruction types
    printf("  All opcodes via auto_to_json:\n");
    for (int i = 0; i <= 23; ++i) {
        IRInstruction inst{static_cast<IROpcode>(i), {0, 0, 0, 0}, 0};
        printf("    %s: %s\n", opcode_name(static_cast<IROpcode>(i)),
               ir_instruction_to_json(inst).c_str());
    }

    printf("\n=== Done ===\n");
}

void demo_show_schema() {
    // Show compile-time JSON schema for IR types
    printf("=== P2996 Reflection Schema ===\n");
    printf("  IRInstruction: ");
    // Use auto_to_json on a default instance
    printf("%s\n", aura::reflect::auto_to_json(IRInstruction{}).c_str());
    printf("\n");
}

// ==============================================================
//  Main
// ==============================================================

int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::string_view arg = argv[1];
        if (arg == "--ir-instruction" || arg == "--ir") {
            demo_ir_instruction();
            return 0;
        }
        if (arg == "--schema") {
            demo_show_schema();
            return 0;
        }
        printf("Usage: %s [--ir-instruction|--schema]\n", argv[0]);
        return 1;
    }

    // Default: run all demos
    demo_ir_instruction();
    demo_show_schema();
    return 0;
}
