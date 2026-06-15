// test_issue_217.cpp — Issue #217 Cycle 1 (pilot):
// AST/IR migration to reflect_members + auto_serialize.
//
// This is the SMALLEST viable cycle of the 1-week scope.
// It does NOT modify any production code. It verifies the
// reflection infrastructure (reflect_members<>, auto_serialize,
// auto_deserialize) works for IR-shaped types using local
// copies of the IR type definitions.
//
// The actual migration of the real IR types in
// src/compiler/ir.ixx is the bigger Cycle 2+ work.
//
// Test scenarios:
//   1. reflect_members<OpcodeInfo>() returns the 3 expected
//      fields (name, operand_count, has_result_slot)
//   2. auto_serialize/auto_deserialize roundtrip for OpcodeInfo
//   3. reflect_members<IRInstruction>() returns the 8 expected
//      fields (opcode, operands, source_ast_node_id, type_id,
//      shape_id, linear_ownership_state, adt_variant_id,
//      narrow_evidence)
//   4. auto_serialize/auto_deserialize roundtrip for IRInstruction
//   5. kOpcodeInfo[0] ("nop") roundtrips correctly
//   6. kOpcodeInfo[6] ("add") roundtrips correctly (with
//      operand_count=3, has_result_slot=true)

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <string_view>
#include <cstdint>
#include <print>
#include <variant>

#include "reflect/reflect.hh"

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL: %s (line %d)\n", (msg), __LINE__); \
        ++g_failed; \
    } else { \
        std::fprintf(stdout, "  PASS: %s\n", (msg)); \
        ++g_passed; \
    } \
} while(0)

#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

// ── Local copies of IR types (Issue #217 Cycle 1 pilot) ──────
//
// Cycle 3: These local copies are kept for the CYCLE 1
// tests (which don't need the full module). A new Test 8
// uses the REAL IR types imported from aura.compiler.ir.

// From src/compiler/ir.ixx (subset — only the fields the
// reflect_members / auto_serialize path needs).
enum class IROpcode : std::uint8_t {
    Nop = 0,
    ConstI64 = 1,
    ConstF64 = 2,
    Local = 3,
    Arg = 4,
    Add = 5,
    Sub = 6,
    Mul = 7,
    Div = 8,
};

struct IRInstruction {
    IROpcode opcode;
    std::array<std::uint32_t, 4> operands = {};
    std::uint32_t source_ast_node_id = 0;
    std::uint32_t type_id = 0;
    std::uint32_t shape_id = 0;
    std::uint8_t linear_ownership_state = 0;
    std::uint32_t adt_variant_id = 0;
    std::uint32_t narrow_evidence = 0;
};

struct OpcodeInfo {
    std::string_view name;
    std::uint8_t operand_count;
    bool has_result_slot;
};

// Minimal kOpcodeInfo table (subset of the 53-entry table in
// src/compiler/ir.ixx:154). Used to verify the roundtrip
// works on the real metadata table.
constexpr OpcodeInfo kOpcodeInfo[] = {
    {"nop", 0, false},
    {"const-i64", 1, true},
    {"add", 3, true},
};

// ── Test 7: OpcodeInfo with std::string name (the fix path) ──
//
// The current OpcodeInfo uses std::string_view for `name`
// (for constexpr support). The migration could change it
// to std::string to make it reflection-friendly. This
// test verifies that std::string roundtrips correctly.
struct OpcodeInfoStr {
    std::string name;
    std::uint8_t operand_count;
    bool has_result_slot;
};
bool test_opcode_info_str_roundtrip() {
    PRINTLN("\n--- Test 7: OpcodeInfo with std::string name ---");
    OpcodeInfoStr original;
    original.name = "const-i64";
    original.operand_count = 1;
    original.has_result_slot = true;
    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("buf size: {}", buf.size());
    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<OpcodeInfoStr>(buf, pos);
    CHECK(rt.name == "const-i64", "std::string name roundtrips");
    CHECK(rt.operand_count == 1, "operand_count roundtrips");
    CHECK(rt.has_result_slot == true, "has_result_slot roundtrips");
    return true;
}
bool test_reflect_opcode_info() {
    PRINTLN("\n--- Test 1: reflect_members<OpcodeInfo>() ---");
    constexpr auto members = aura::reflect::reflect_members<OpcodeInfo>();
    std::println("members.size = {}", members.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        std::println("  [{}] name={} kind={} offset={}",
                     i, members[i].name, (int)members[i].kind, members[i].offset);
    }
    CHECK(members.size() == 3,
          "OpcodeInfo has 3 non-static data members");
    // Find each field by name (order may vary by GCC version)
    bool found_name = false, found_count = false, found_result = false;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (members[i].name == "name") found_name = true;
        if (members[i].name == "operand_count") found_count = true;
        if (members[i].name == "has_result_slot") found_result = true;
    }
    CHECK(found_name, "field 'name' found");
    CHECK(found_count, "field 'operand_count' found");
    CHECK(found_result, "field 'has_result_slot' found");
    return true;
}

// ── Test 2: auto_serialize/auto_deserialize roundtrip ───────
bool test_opcode_info_roundtrip() {
    PRINTLN("\n--- Test 2: OpcodeInfo roundtrip ---");
    OpcodeInfo original{"const-i64", 1, true};
    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("buf size: {}", buf.size());
    // Cycle 2 fix: std::string_view is now a first-class
    // MemberKind (StringView). The name field roundtrips
    // because both serialize and deserialize use the
    // length-prefixed raw-bytes layout.
    CHECK(!buf.empty(), "buf is non-empty");

    std::size_t pos = 0;
    auto roundtrip = aura::reflect::auto_deserialize<OpcodeInfo>(buf, pos);
    CHECK(roundtrip.name == "const-i64",
          "string_view name roundtrips (Issue #217 Cycle 2 fix)");
    CHECK(roundtrip.operand_count == 1,
          "operand_count roundtrips");
    CHECK(roundtrip.has_result_slot == true,
          "has_result_slot roundtrips");
    return true;
}

// ── Test 3: reflect_members<IRInstruction>() returns 8 fields ─
bool test_reflect_ir_instruction() {
    PRINTLN("\n--- Test 3: reflect_members<IRInstruction>() ---");
    constexpr auto members = aura::reflect::reflect_members<IRInstruction>();
    std::println("members.size = {}", members.size());
    CHECK(members.size() == 8,
          "IRInstruction has 8 non-static data members");
    const char* expected[] = {
        "opcode", "operands", "source_ast_node_id", "type_id",
        "shape_id", "linear_ownership_state", "adt_variant_id",
        "narrow_evidence"
    };
    for (auto& e : expected) {
        bool found = false;
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i].name == e) { found = true; break; }
        }
        CHECK(found, e);
    }
    return true;
}

// ── Test 4: IRInstruction roundtrip ────────────────────────
bool test_ir_instruction_roundtrip() {
    PRINTLN("\n--- Test 4: IRInstruction roundtrip ---");
    IRInstruction original;
    original.opcode = IROpcode::Add;
    original.operands = {0, 1, 2, 3};
    original.source_ast_node_id = 42;
    original.type_id = 1;
    original.shape_id = 7;
    original.linear_ownership_state = 2;
    original.adt_variant_id = 0;
    original.narrow_evidence = 0;
    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("buf size: {}", buf.size());

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<IRInstruction>(buf, pos);
    CHECK(rt.opcode == IROpcode::Add, "opcode preserved");
    CHECK(rt.operands[0] == 0, "operands[0] preserved");
    CHECK(rt.operands[1] == 1, "operands[1] preserved");
    CHECK(rt.operands[2] == 2, "operands[2] preserved");
    CHECK(rt.operands[3] == 3, "operands[3] preserved");
    CHECK(rt.source_ast_node_id == 42, "source_ast_node_id preserved");
    CHECK(rt.type_id == 1, "type_id preserved");
    CHECK(rt.shape_id == 7, "shape_id preserved");
    CHECK(rt.linear_ownership_state == 2,
          "linear_ownership_state preserved");
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 5: kOpcodeInfo[0] roundtrips (nop) ────────────────
bool test_kopcode_info_nop() {
    PRINTLN("\n--- Test 5: kOpcodeInfo[0] (nop) roundtrip ---");
    OpcodeInfo original = kOpcodeInfo[0];
    CHECK(original.name == "nop", "kOpcodeInfo[0].name == \"nop\"");
    CHECK(original.operand_count == 0, "kOpcodeInfo[0].operand_count == 0");
    CHECK(original.has_result_slot == false,
          "kOpcodeInfo[0].has_result_slot == false");

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<OpcodeInfo>(buf, pos);
    // Cycle 2 fix: string_view name roundtrips
    CHECK(rt.name == "nop", "nop name roundtrips (string_view)");
    CHECK(rt.operand_count == 0, "nop operand_count roundtrips");
    CHECK(rt.has_result_slot == false,
          "nop has_result_slot roundtrips");
    return true;
}

// ── Test 6: kOpcodeInfo[2] roundtrips (add) ────────────────
bool test_kopcode_info_add() {
    PRINTLN("\n--- Test 6: kOpcodeInfo[2] (add) roundtrip ---");
    OpcodeInfo original = kOpcodeInfo[2];
    CHECK(original.name == "add", "kOpcodeInfo[2].name == \"add\"");
    CHECK(original.operand_count == 3, "kOpcodeInfo[2].operand_count == 3");
    CHECK(original.has_result_slot == true,
          "kOpcodeInfo[2].has_result_slot == true");

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<OpcodeInfo>(buf, pos);
    CHECK(rt.name == "add", "add name roundtrips (string_view)");
    CHECK(rt.operand_count == 3, "add operand_count roundtrips");
    CHECK(rt.has_result_slot == true,
          "add has_result_slot roundtrips");
    return true;
}

int main() {
    std::fprintf(stdout, "═══ Issue #217 — IR reflection pilot (Cycle 1) ═══\n");
    std::fprintf(stdout, "  Verifies the reflection infrastructure works for\n");
    std::fprintf(stdout, "  IR-shaped types. The full migration ships in\n");
    std::fprintf(stdout, "  Cycle 2+.\n\n");

    test_reflect_opcode_info();
    test_opcode_info_roundtrip();
    test_reflect_ir_instruction();
    test_ir_instruction_roundtrip();
    test_kopcode_info_nop();
    test_kopcode_info_add();
    test_opcode_info_str_roundtrip();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
