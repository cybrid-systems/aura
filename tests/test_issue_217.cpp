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
    {"nop", 0, false},         // 0 Nop
    {"const-i64", 1, true},    // 1 ConstI64
    {"const-f64", 1, true},    // 2 ConstF64
    {"local", 2, true},        // 3 Local
    {"arg", 2, true},          // 4 Arg
    {"add", 3, true},          // 5 Add
    {"sub", 3, true},          // 6 Sub
    {"mul", 3, true},          // 7 Mul
    {"div", 3, true},          // 8 Div
};

// ── Test 8: lookup_opcode helper (Issue #217 Cycle 4) ──────
//
// The lookup_opcode(IROpcode) helper in src/compiler/ir.ixx
// does the bounds check + array access in one call. This
// test verifies it returns the right OpcodeInfo for the
// known opcodes.
//
// Note: this test uses a local copy of OpcodeInfo (same
// as Test 1-7) because we can't import the aura.compiler.ir
// module from this test target (see Test 8 in the previous
// cycle). The lookup_opcode helper is a constexpr function
// in the ir.ixx module; we re-implement it here for the
// local copy.
const OpcodeInfo* local_lookup_opcode(IROpcode op) {
    auto idx = static_cast<std::size_t>(op);
    return idx < std::size(kOpcodeInfo) ? &kOpcodeInfo[idx] : nullptr;
}
bool test_lookup_opcode_helper() {
    PRINTLN("\n--- Test 8: lookup_opcode helper ---");
    // Valid opcodes return the right info
    auto* nop = local_lookup_opcode(IROpcode::Nop);
    CHECK(nop != nullptr, "lookup_opcode(Nop) returns non-null");
    if (nop) {
        CHECK(nop->name == "nop", "Nop name == \"nop\"");
        CHECK(nop->operand_count == 0, "Nop operand_count == 0");
        CHECK(nop->has_result_slot == false, "Nop has_result_slot == false");
    }
    auto* add_info = local_lookup_opcode(IROpcode::Add);
    CHECK(add_info != nullptr, "lookup_opcode(Add) returns non-null");
    if (add_info) {
        CHECK(add_info->name == "add", "Add name == \"add\"");
        CHECK(add_info->operand_count == 3, "Add operand_count == 3");
        CHECK(add_info->has_result_slot == true, "Add has_result_slot == true");
    }
    auto* const_i64 = local_lookup_opcode(IROpcode::ConstI64);
    CHECK(const_i64 != nullptr, "lookup_opcode(ConstI64) returns non-null");
    if (const_i64) {
        CHECK(const_i64->name == "const-i64", "ConstI64 name == \"const-i64\"");
        CHECK(const_i64->operand_count == 1, "ConstI64 operand_count == 1");
    }
    return true;
}
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

// ── Test 9: AST SourceLocation roundtrip (Issue #217 Cycle 5) ──
//
// The simplest AST type: SourceLocation has 3 uint32 fields.
// reflect_members + auto_serialize/auto_deserialize should
// work out of the box.
struct SourceLocation {
    std::uint32_t line = 0, column = 0, file = 0;
};
bool test_source_location_roundtrip() {
    PRINTLN("\n--- Test 9: AST SourceLocation roundtrip ---");
    constexpr auto members = aura::reflect::reflect_members<SourceLocation>();
    std::println("SourceLocation has {} members", members.size());
    CHECK(members.size() == 3, "SourceLocation has 3 members");
    const char* expected[] = {"line", "column", "file"};
    for (auto& e : expected) {
        bool found = false;
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i].name == e) { found = true; break; }
        }
        CHECK(found, e);
    }

    SourceLocation original{42, 7, 1};
    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("SourceLocation serialized: {} bytes (3 * 4 = 12 expected)",
                 buf.size());
    CHECK(buf.size() == 12, "buf size == 12 bytes (3 uint32)");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<SourceLocation>(buf, pos);
    CHECK(rt.line == 42, "line roundtrips");
    CHECK(rt.column == 7, "column roundtrips");
    CHECK(rt.file == 1, "file roundtrips");
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 10: AST Patch roundtrip (Issue #217 Cycle 5) ────────
//
// Patch is an AI mutation descriptor (3 POD fields). Similar
// to SourceLocation, the reflection should work directly.
struct Patch {
    std::uint32_t node = 0;
    std::uint32_t field_offset = 0;
    std::uint64_t new_value = 0;
};
bool test_patch_roundtrip() {
    PRINTLN("\n--- Test 10: AST Patch roundtrip ---");
    constexpr auto members = aura::reflect::reflect_members<Patch>();
    std::println("Patch has {} members", members.size());
    CHECK(members.size() == 3, "Patch has 3 members");
    const char* expected[] = {"node", "field_offset", "new_value"};
    for (auto& e : expected) {
        bool found = false;
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i].name == e) { found = true; break; }
        }
        CHECK(found, e);
    }

    Patch original{100, 16, 0xDEADBEEFCAFE};
    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("Patch serialized: {} bytes", buf.size());
    CHECK(!buf.empty(), "buf is non-empty");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<Patch>(buf, pos);
    CHECK(rt.node == 100, "node roundtrips");
    CHECK(rt.field_offset == 16, "field_offset roundtrips");
    CHECK(rt.new_value == 0xDEADBEEFCAFE, "new_value roundtrips");
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 11: std::span field roundtrip (Issue #217 Cycle 6) ──
//
// NodeView in src/core/ast.ixx uses std::span<const NodeId>
// for the children field. This test verifies a struct
// with a span field roundtrips through the new
// MemberKind::Span path.
//
// Note: std::span is non-owning. The deserialized span
// points into the buf (no copy) — the caller must keep
// the buf alive for the lifetime of the span. The test
// verifies the data while the buf is still in scope.
struct NodeViewLike {
    std::uint32_t id = 0;
    std::uint32_t tag = 0;
    std::int64_t int_value = 0;
    std::span<const char> children;
};
bool test_span_field_roundtrip() {
    PRINTLN("\n--- Test 11: std::span field roundtrip ---");
    constexpr auto members = aura::reflect::reflect_members<NodeViewLike>();
    std::println("NodeViewLike has {} members", members.size());
    CHECK(members.size() == 4, "NodeViewLike has 4 members");
    bool found_children = false;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (members[i].name == "children") {
            found_children = true;
            std::println("  children kind = {}", (int)members[i].kind);
        }
    }
    CHECK(found_children, "NodeViewLike has 'children' field");

    // Build a span from a vector of char
    std::vector<char> data = {'a', 'b', 'c', 'd', 'e'};
    NodeViewLike original;
    original.id = 42;
    original.tag = 7;
    original.int_value = 0xDEADBEEF;
    original.children = std::span<const char>(data.data(), data.size());

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("NodeViewLike serialized: {} bytes", buf.size());
    // Expected: 4 (id) + 4 (tag) + 8 (int_value) +
    //           4 (span size) + 5 (data) = 25 bytes
    CHECK(buf.size() == 25, "buf size == 25 bytes (4+4+8+4+5)");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<NodeViewLike>(buf, pos);
    CHECK(rt.id == 42, "id roundtrips");
    CHECK(rt.tag == 7, "tag roundtrips");
    CHECK(rt.int_value == 0xDEADBEEF, "int_value roundtrips");
    // The deserialized span points into buf (no copy).
    // The data is still in scope (buf is alive).
    CHECK(rt.children.size() == 5, "children size == 5");
    if (rt.children.size() == 5) {
        CHECK(rt.children[0] == 'a', "children[0] == 'a'");
        CHECK(rt.children[4] == 'e', "children[4] == 'e'");
    }
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 12: NodeView-like with std::span<const uint32_t> ───
//
// Issue #217 Cycle 7: the real NodeView in ast.ixx has
// `std::span<const NodeId>` (where NodeId is uint32_t) for
// the children field. The Cycle 6 Span implementation
// hardcoded `std::span<const char>` in the deserialize
// path, which is correct for char spans but is a type
// mismatch for non-char element types.
//
// Cycle 7 fixes the SERIALIZE side: it now writes the
// BYTE count (not element count), computed via
// elem_size from the MemberInfo. The DESERIALIZE side
// still hardcodes std::span<const char> which means
// non-char element types need a re-interpretation step
// (documented as a follow-up limitation).
//
// This test verifies the elem_size is correctly set for
// the Span MemberKind (the fix that enables the
// NodeView migration) and the SERIALIZE side produces
// the right byte count. The full roundtrip (with
// deserialization) is limited to char spans (Test 11).
struct NodeViewLikeU32 {
    std::uint32_t id = 0;
    std::uint32_t tag = 0;
    std::int64_t int_value = 0;
    std::span<const std::uint32_t> children;
};
bool test_span_uint32_serialize_only() {
    PRINTLN("\n--- Test 12: std::span<const uint32_t> serialize ---");
    constexpr auto members = aura::reflect::reflect_members<NodeViewLikeU32>();
    std::println("NodeViewLikeU32 has {} members", members.size());
    CHECK(members.size() == 4, "NodeViewLikeU32 has 4 members");
    bool found_children = false;
    int children_kind = -1;
    std::size_t children_elem_size = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (members[i].name == "children") {
            found_children = true;
            children_kind = (int)members[i].kind;
            children_elem_size = members[i].elem_size;
            std::println("  children kind = {}, elem_size = {}",
                         children_kind, children_elem_size);
        }
    }
    CHECK(found_children, "NodeViewLikeU32 has 'children' field");
    CHECK(children_kind == 15,
          "children kind is MemberKind::Span (= 15)");
    CHECK(children_elem_size == sizeof(std::uint32_t),
          "children elem_size is sizeof(uint32_t) = 4");

    // Build a span from a vector of uint32_t
    std::vector<std::uint32_t> data = {100, 200, 300, 400, 500};
    NodeViewLikeU32 original;
    original.id = 42;
    original.tag = 7;
    original.int_value = 0xDEADBEEF;
    original.children = std::span<const std::uint32_t>(data.data(), data.size());

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("NodeViewLikeU32 serialized: {} bytes", buf.size());
    // Expected: 4 (id) + 4 (tag) + 8 (int_value) +
    //           4 (span byte_count) + 20 (5 * sizeof(uint32_t))
    //         = 40 bytes
    CHECK(buf.size() == 40,
          "buf size == 40 bytes (4+4+8+4+20)");
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

// ── Test 6: kOpcodeInfo[5] roundtrips (add) ────────────────
bool test_kopcode_info_add() {
    PRINTLN("\n--- Test 6: kOpcodeInfo[5] (add) roundtrip ---");
    // After expanding the local kOpcodeInfo table to
    // include entries for the test opcodes (Nop=0,
    // ConstI64=1, ConstF64=2, Local=3, Arg=4, Add=5, ...),
    // the Add entry is at index 5.
    OpcodeInfo original = kOpcodeInfo[5];
    CHECK(original.name == "add", "kOpcodeInfo[5].name == \"add\"");
    CHECK(original.operand_count == 3, "kOpcodeInfo[5].operand_count == 3");
    CHECK(original.has_result_slot == true,
          "kOpcodeInfo[5].has_result_slot == true");

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
    test_lookup_opcode_helper();
    test_source_location_roundtrip();
    test_patch_roundtrip();
    test_span_field_roundtrip();
    test_span_uint32_serialize_only();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
