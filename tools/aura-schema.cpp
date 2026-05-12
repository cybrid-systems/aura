// ──────────────────────────────────────────────────────────────
//  aura-schema — Compile-time JSON Schema generator for Aura IR
//
//  Uses P2996 reflection + P1306 expansion statements to generate
//  JSON Schema (draft 2020-12) at compile time.
//
//  Build: ninja aura-schema
//  Usage: ./aura-schema [--serve]
//    --serve: JSON-line output for AI agent consumption
//    (no args): pretty-print schemas
// ──────────────────────────────────────────────────────────────

#include "reflect/reflect.hh"
#include "reflect/reflect_schema.hh"
#include <cstdio>
#include <string>
#include <array>
#include <vector>
#include <cstdint>

// ── IR types ──────────────────────────────────────────────────

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

struct IRModule {
    std::vector<IRFunction> functions;
    std::uint32_t entry_function_id = 0;
};

// ── Schema types ──────────────────────────────────────────────
// Once the P2996 auto_to_json handles vectors/strings properly,
// these type registrations will be auto-generated.

// ==============================================================
//  Schema exporter
// ==============================================================

void print_schema(const char* name, std::string_view schema) {
    printf("// %s\n", name);
    printf("%.*s\n\n", (int)schema.size(), schema.data());
}

void print_serve_schema(const char* name, std::string_view schema) {
    // Compact JSON-line format for AI agent consumption
    printf("{\"type\":\"schema\",\"name\":\"%s\",\"schema\":", name);
    // The schema itself is JSON, so we need to export it carefully
    // For now, just re-pretty-print it
    printf("\n%.*s}\n", (int)schema.size(), schema.data());
}

// ==============================================================
//  Main
// ==============================================================

int main(int argc, char* argv[]) {
    bool serve_mode = (argc > 1 && std::string_view(argv[1]) == "--serve");
    
    if (serve_mode) {
        printf("{\"tool\":\"aura-schema\",\"version\":\"0.1\",\"mode\":\"serve\"}\n");
    } else {
        printf("=== Compile-time JSON Schema Generator ===\n");
        printf("P2996 reflection + P1306 expansion statements\n\n");
    }
    
    // Without the real module types, we use local replicas for demo.
    // When GCC fixes module + reflection compatibility, we'll
    // switch to the actual aura::ir:: types.
    
    auto show = [&](const char* name, auto schema_fn) {
        auto sv = schema_fn();
        if (serve_mode) {
            print_serve_schema(name, sv);
        } else {
            print_schema(name, sv);
        }
    };
    
    show("IRInstruction", []{ return aura::reflect::get_json_schema<IRInstruction>(); });
    show("IRFunction",    []{ return aura::reflect::get_json_schema<IRFunction>(); });
    show("IRModule",      []{ return aura::reflect::get_json_schema<IRModule>(); });
    
    if (!serve_mode) {
        printf("3 schemas generated at compile time.\n");
        printf("Usage: ./aura-schema [--serve] for JSON-line mode.\n");
    }
    
    return 0;
}
