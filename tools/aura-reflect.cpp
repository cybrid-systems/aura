// ──────────────────────────────────────────────────────────────
//  aura-reflect — Standalone P2996 reflection tool for Aura IR
//
//  Built independently from the module-based aura core.
//  Uses:  ninja aura-reflect
//
//  CLI:
//    --ir-instruction   Demo IR instruction serialization
//    --schema           Show compile-time JSON Schema for IR types
//    --expansion        Demo template for (P1306) expansion statements
//    (no args)          Run all demos
// ──────────────────────────────────────────────────────────────

#include "reflect/reflect.hh"
#include "reflect/opcode_reflect.hh"
#include "reflect/reflect_schema.hh"
#include <cstdio>
#include <string>
#include <array>
#include <vector>
#include <cstdint>
#include <string_view>

// ── IR types (inlined to avoid module dependency) ──────────────

enum class IROpcode : std::uint8_t {
    Nop, ConstI64, Local, Arg,
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

// ==============================================================
//  IROpcode → string
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

// ==============================================================
//  IR instruction serialization (mixed: enum→string + auto_to_json)
// ==============================================================

std::string ir_instruction_to_json(const IRInstruction& inst) {
    std::string json = "{";
    json += "\"opcode\":\""; json += opcode_name(inst.opcode); json += "\",";
    json += "\"operands\":" + aura::reflect::auto_to_json(inst.operands) + ",";
    json += "\"source_ast_node_id\":" + std::to_string(inst.source_ast_node_id);
    json += "}";
    return json;
}

// ==============================================================
//  --expansion: P1306 expansion statements demo
// ==============================================================

struct ExpansionDemo {
    std::string name;
    int         version;
    double      score;
    bool        active;
};

template <typename T>
void demo_expansion_print(const T& obj, const char* label) {
    using namespace std::meta;
    
    constexpr size_t N = []() {
        return nonstatic_data_members_of(
            ^^T, access_context::unchecked()).size();
    }();
    
    static constexpr auto members = []() {
        std::array<info, N> arr{};
        auto vec = nonstatic_data_members_of(^^T, access_context::unchecked());
        for (size_t i = 0; i < N; ++i) arr[i] = vec[i];
        return arr;
    }();
    
    printf("%s:\n", label);
    
    // ── P1306 expansion statement ─────────────────────────────
    // Each iteration of template for generates real, separate code.
    // The [:m:] splice accesses obj.member with the correct type.
    // type_of(m) + is_same_type → compile-time type dispatch.
    template for (constexpr auto m : members) {
        constexpr auto type = type_of(m);
        auto name = identifier_of(m);
        
        if constexpr (is_same_type(type, ^^std::string)) {
            printf("    %s (string): \"%s\"\n",
                   std::string(name).c_str(),
                   obj.[:m:].c_str());
        } else if constexpr (is_same_type(type, ^^int) || 
                             is_same_type(type, ^^unsigned int)) {
            printf("    %s (int): %d\n",
                   std::string(name).c_str(),
                   static_cast<int>(obj.[:m:]));
        } else if constexpr (is_same_type(type, ^^double) || 
                             is_same_type(type, ^^float)) {
            printf("    %s (float): %g\n",
                   std::string(name).c_str(),
                   static_cast<double>(obj.[:m:]));
        } else if constexpr (is_same_type(type, ^^bool)) {
            printf("    %s (bool): %s\n",
                   std::string(name).c_str(),
                   obj.[:m:] ? "true" : "false");
        } else {
            printf("    %s (other)\n", std::string(name).c_str());
        }
    }
}

void demo_expansion() {
    printf("=== P1306 Expansion Statements Demo ===\n\n");
    printf("template for + [:m:] generates separate code paths\n");
    printf("for each member at compile time, with type dispatch\n");
    printf("via constexpr is_same_type().\n\n");
    
    ExpansionDemo d{"aura", 1, 99.5, true};
    demo_expansion_print(d, "ExpansionDemo");
    
    printf("\n");
    demo_expansion_print(IRInstruction{IROpcode::Add, {1,2,0,0}, 42}, "IRInstruction");
    
    printf("\n✅ template for works with mixed-type structs\n");
}

// ==============================================================
//  --schema: JSON Schema demo
// ==============================================================

void demo_schema() {
    printf("=== Compile-time JSON Schema Generation ===\n\n");
    printf("Generated using P2996 reflection + P1306 expansion\n");
    printf("statements. Stored as static constexpr char array\n");
    printf("at compile time.\n\n");
    
    printf("--- IRInstruction Schema ---\n%s\n\n",
           aura::reflect::get_json_schema<IRInstruction>().data());
    
    printf("Describes 3 fields: opcode (enum→integer),\n");
    printf("operands (std::array<uint32_t,4>→array),\n");
    printf("source_ast_node_id (uint32_t→integer).\n");
}

// ==============================================================
//  --ir-instruction: IR serialization demo
// ==============================================================

void demo_ir_instruction() {
    printf("=== IR Instruction serialization ===\n\n");
    
    auto show = [](IROpcode op, auto ops, uint32_t src) {
        IRInstruction inst{op, ops, src};
        auto json = ir_instruction_to_json(inst);
        printf("  %s → %s\n", opcode_name(op), json.c_str());
    };
    
    show(IROpcode::Add,         std::array<std::uint32_t,4>{1,2,0,0},  42);
    show(IROpcode::MakeClosure, std::array<std::uint32_t,4>{0,2,0,0},  100);
    show(IROpcode::Call,        std::array<std::uint32_t,4>{3,2,5,0},  7);
    show(IROpcode::Return,      std::array<std::uint32_t,4>{0,0,0,0},  0);
    show(IROpcode::Branch,      std::array<std::uint32_t,4>{0,1,2,0},  15);
    
    printf("\n  Uses P2996 auto_to_json for operands[] and node_id.\n");
    printf("  opcode handled via hand-written switch with enum names.\n");
}

// ==============================================================
//  --opcodes: P2996 reflection demo
// ==============================================================

void demo_opcodes() {
    printf("=== IROpcode Reflection Table (P2996-generated) ===\n\n");
    constexpr auto N = aura::reflect::enum_count<IROpcode>();
    printf("  %zu opcodes, names unique: %d\n\n", N,
           aura::reflect::validate_enum<IROpcode>());
    printf("  #   name\n");
    printf("  ──  ──────────────────────────────\n");
    for (int i = 0; i < (int)N; ++i) {
        auto name = aura::reflect::opcode_name<IROpcode>(i);
        printf("  %2d  %.*s\n", i, (int)name.size(), name.data());
    }
    printf("\n  (no hand-written switch — P2996 enumerators_of)\n");
}

// ==============================================================
//  Main
// ==============================================================

int main(int argc, char* argv[]) {
    auto has = [&](const char* name) {
        for (int i = 1; i < argc; ++i)
            if (std::string_view(argv[i]) == name) return true;
        return false;
    };
    
    bool all = (argc == 1);
    
    if (all || has("--ir-instruction")) { demo_ir_instruction(); printf("\n"); }
    if (all || has("--schema"))         { demo_schema();         printf("\n"); }
    if (all || has("--expansion"))      { demo_expansion();      printf("\n"); }
    if (all || has("--opcodes"))        { demo_opcodes();        printf("\n"); }
    
    if (!all && !has("--ir-instruction") && !has("--schema") && !has("--expansion")) {
        printf("Usage: %s [--ir-instruction|--schema|--expansion|--opcodes]\n", argv[0]);
        return 1;
    }
    
    return 0;
}
