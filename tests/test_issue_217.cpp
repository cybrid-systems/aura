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
bool test_span_uint32_roundtrip() {
    PRINTLN("\n--- Test 12: std::span<const uint32_t> full roundtrip ---");
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

    // Cycle 8 fix: full deserialize roundtrip for non-char
    // element types. The Span case now divides byte_count
    // by elem_size before storing in the span's size field,
    // so non-char element types work correctly.
    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<NodeViewLikeU32>(buf, pos);
    CHECK(rt.id == 42, "id roundtrips");
    CHECK(rt.tag == 7, "tag roundtrips");
    CHECK(rt.int_value == 0xDEADBEEF, "int_value roundtrips");
    // The deserialized span: byte_count / elem_size = 20 / 4 = 5 elements
    CHECK(rt.children.size() == 5,
          "children size == 5 (byte_count / elem_size = 20 / 4)");
    if (rt.children.size() == 5) {
        CHECK(rt.children[0] == 100, "children[0] == 100");
        CHECK(rt.children[1] == 200, "children[1] == 200");
        CHECK(rt.children[2] == 300, "children[2] == 300");
        CHECK(rt.children[3] == 400, "children[3] == 400");
        CHECK(rt.children[4] == 500, "children[4] == 500");
    }
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 13: MutationRecord-like (5 std::string fields) ────
//
// Issue #217 Cycle 9: MutationRecord in src/core/ast.ixx
// has 5 std::string fields (operator_name, old_type_str,
// new_type_str, summary, old_subtree_source), 2 enum
// fields (MutationStatus, InvariantStatus), and several
// POD fields. This test verifies a struct with the same
// shape roundtrips correctly.
enum class MutationStatus : std::uint8_t {
    Committed = 0,
    RolledBack = 1,
};
enum class InvariantStatus : std::uint8_t {
    NotChecked = 0,
    Ok = 1,
    Warnings = 2,
    Violations = 3,
};
struct MutationRecordLike {
    std::uint64_t mutation_id = 0;
    std::uint64_t timestamp_ms = 0;
    std::uint32_t target_node = 0;
    std::string operator_name;
    std::string old_type_str;
    std::string new_type_str;
    std::string summary;
    MutationStatus status = MutationStatus::Committed;
    std::uint32_t field_offset = 0;
    std::uint64_t old_value = 0;
    std::uint64_t new_value = 0;
    bool has_rollback_data = false;
    std::uint32_t parent_id = 0;
    std::uint32_t child_idx = 0;
    std::string old_subtree_source;
    bool has_subtree_rollback = false;
    InvariantStatus invariant_status = InvariantStatus::NotChecked;
};
bool test_mutation_record_roundtrip() {
    PRINTLN("\n--- Test 13: MutationRecord-like (5 std::strings) ---");
    constexpr auto members = aura::reflect::reflect_members<MutationRecordLike>();
    std::println("MutationRecordLike has {} members", members.size());
    // 17 fields total: 2 u64 + 4 u32 + 5 string + 2 enums + 3 bool-ish (has_rollback_data, has_subtree_rollback, ...)
    CHECK(members.size() == 17, "MutationRecordLike has 17 members");
    // Verify all 5 string fields are found
    const char* expected_strings[] = {
        "operator_name", "old_type_str", "new_type_str",
        "summary", "old_subtree_source"
    };
    for (auto& e : expected_strings) {
        bool found = false;
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i].name == e) { found = true; break; }
        }
        CHECK(found, e);
    }

    // Build a MutationRecord with all 5 strings populated
    MutationRecordLike original;
    original.mutation_id = 0xDEADBEEFCAFE0001;
    original.timestamp_ms = 1700000000000;
    original.target_node = 42;
    original.operator_name = "replace-type";
    original.old_type_str = "Int";
    original.new_type_str = "Bool";
    original.summary = "type changed from Int to Bool";
    original.status = MutationStatus::Committed;
    original.field_offset = 16;
    original.old_value = 100;
    original.new_value = 200;
    original.has_rollback_data = true;
    original.parent_id = 7;
    original.child_idx = 2;
    original.old_subtree_source = "(+ 1 2)";
    original.has_subtree_rollback = true;
    original.invariant_status = InvariantStatus::Ok;

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("MutationRecordLike serialized: {} bytes", buf.size());
    CHECK(!buf.empty(), "buf is non-empty");

    // Full roundtrip
    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<MutationRecordLike>(buf, pos);
    CHECK(rt.mutation_id == 0xDEADBEEFCAFE0001, "mutation_id roundtrips");
    CHECK(rt.timestamp_ms == 1700000000000, "timestamp_ms roundtrips");
    CHECK(rt.target_node == 42, "target_node roundtrips");
    CHECK(rt.operator_name == "replace-type", "operator_name roundtrips");
    CHECK(rt.old_type_str == "Int", "old_type_str roundtrips");
    CHECK(rt.new_type_str == "Bool", "new_type_str roundtrips");
    CHECK(rt.summary == "type changed from Int to Bool", "summary roundtrips");
    CHECK(rt.status == MutationStatus::Committed, "status roundtrips");
    CHECK(rt.field_offset == 16, "field_offset roundtrips");
    CHECK(rt.old_value == 100, "old_value roundtrips");
    CHECK(rt.new_value == 200, "new_value roundtrips");
    CHECK(rt.has_rollback_data == true, "has_rollback_data roundtrips");
    CHECK(rt.parent_id == 7, "parent_id roundtrips");
    CHECK(rt.child_idx == 2, "child_idx roundtrips");
    CHECK(rt.old_subtree_source == "(+ 1 2)", "old_subtree_source roundtrips");
    CHECK(rt.has_subtree_rollback == true, "has_subtree_rollback roundtrips");
    CHECK(rt.invariant_status == InvariantStatus::Ok, "invariant_status roundtrips");
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 14: MatchClauseInfo-like (2 std::vector<SymId> + bool) ─
//
// Issue #217 Cycle 10: MatchClauseInfo in src/core/ast.ixx
// has 2 std::vector<SymId> fields (used_constructors,
// candidate_constructors) and 1 bool (has_wildcard).
// SymId is std::uint32_t. The vector overload is already
// supported (tested in #215 Cycle 2). This test verifies
// the full struct roundtrips correctly.
struct MatchClauseInfoLike {
    std::vector<std::uint32_t> used_constructors;
    std::vector<std::uint32_t> candidate_constructors;
    bool has_wildcard = false;
};
bool test_match_clause_info_roundtrip() {
    PRINTLN("\n--- Test 14: MatchClauseInfo-like (vectors) ---");
    constexpr auto members = aura::reflect::reflect_members<MatchClauseInfoLike>();
    std::println("MatchClauseInfoLike has {} members", members.size());
    CHECK(members.size() == 3, "MatchClauseInfoLike has 3 members");
    const char* expected_fields[] = {
        "used_constructors", "candidate_constructors", "has_wildcard"
    };
    for (auto& e : expected_fields) {
        bool found = false;
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i].name == e) { found = true; break; }
        }
        CHECK(found, e);
    }

    // Build a MatchClauseInfo with all 3 fields populated
    MatchClauseInfoLike original;
    original.used_constructors = {101, 102, 103};
    original.candidate_constructors = {201, 202};
    original.has_wildcard = true;

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("MatchClauseInfoLike serialized: {} bytes", buf.size());
    // Expected: 4 (used_constructors count) + 12 (3 * 4) +
    //           4 (candidate_constructors count) + 8 (2 * 4) +
    //           1 (has_wildcard) = 29 bytes
    CHECK(buf.size() == 29, "buf size == 29 bytes (4+12+4+8+1)");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<MatchClauseInfoLike>(buf, pos);
    CHECK(rt.used_constructors.size() == 3,
          "used_constructors size == 3");
    if (rt.used_constructors.size() == 3) {
        CHECK(rt.used_constructors[0] == 101, "used[0] == 101");
        CHECK(rt.used_constructors[1] == 102, "used[1] == 102");
        CHECK(rt.used_constructors[2] == 103, "used[2] == 103");
    }
    CHECK(rt.candidate_constructors.size() == 2,
          "candidate_constructors size == 2");
    if (rt.candidate_constructors.size() == 2) {
        CHECK(rt.candidate_constructors[0] == 201, "cand[0] == 201");
        CHECK(rt.candidate_constructors[1] == 202, "cand[1] == 202");
    }
    CHECK(rt.has_wildcard == true, "has_wildcard roundtrips");
    CHECK(pos == buf.size(), "all bytes consumed");

    // Test empty vectors too
    MatchClauseInfoLike empty;
    empty.used_constructors = {};
    empty.candidate_constructors = {};
    empty.has_wildcard = false;
    std::vector<char> buf2;
    aura::reflect::auto_serialize(buf2, empty);
    std::println("Empty MatchClauseInfoLike serialized: {} bytes", buf2.size());
    // Expected: 4 (count 0) + 4 (count 0) + 1 (bool) = 9 bytes
    CHECK(buf2.size() == 9, "empty buf size == 9 bytes (4+4+1)");
    std::size_t pos2 = 0;
    auto rt2 = aura::reflect::auto_deserialize<MatchClauseInfoLike>(buf2, pos2);
    CHECK(rt2.used_constructors.size() == 0,
          "empty used_constructors roundtrips");
    CHECK(rt2.candidate_constructors.size() == 0,
          "empty candidate_constructors roundtrips");
    CHECK(rt2.has_wildcard == false,
          "empty has_wildcard roundtrips");
    return true;
}

// ── Test 15: FlatAST-like SoA columns (Cycle 11) ───────────
//
// Issue #217 Cycle 11: FlatAST in src/core/ast.ixx stores
// data in SoA (Structure of Arrays) columns for cache
// locality (Issue #145). The SoA columns are PRIVATE
// members of the FlatAST class, so the generic
// reflect_members<T>() template can't see them. The
// proper fix is a CUSTOM auto_serialize overload for
// FlatAST that iterates the SoA columns explicitly
// (deferred to a future cycle — see follow-ups).
//
// This test verifies the conceptual approach with a
// FlatAST-like struct that has PUBLIC SoA columns.
// The struct has 4 SoA columns: int_val_ (vector<int64_t>),
// type_id_ (vector<uint32_t>), tag_ (vector<uint8_t>),
// and string_val_ (vector<string>). All 4 should be
// reflectable + roundtrippable.
//
// Cycle 11 also adds vector<string> support to the
// generic reflect path. The POD-vector path
// reinterpret_casts to vector<char>, which gives the
// correct byte count for POD element types but produces
// garbage for std::string (each string is separately
// heap-allocated, so the vector's contiguous data is
// just the string objects' internal memory, not the
// string contents). The new special path detects
// vector<string> via elem_size == sizeof(std::string)
// and uses the length-prefixed string format
// (u32 count + (u32 len + bytes) per string).
struct FlatASTLikeSoA {
    std::vector<std::int64_t> int_val_;
    std::vector<std::uint32_t> type_id_;
    std::vector<std::uint8_t> tag_;
    std::vector<std::string> string_val_;
};
bool test_flatast_soa_columns() {
    PRINTLN("\n--- Test 15: FlatAST-like SoA columns ---");
    constexpr auto members = aura::reflect::reflect_members<FlatASTLikeSoA>();
    std::println("FlatASTLikeSoA has {} members", members.size());
    // 4 SoA columns: int_val_, type_id_, tag_, string_val_
    CHECK(members.size() == 4, "FlatASTLikeSoA has 4 members");
    bool found_int = false, found_type = false, found_tag = false, found_str = false;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (members[i].name == "int_val_") found_int = true;
        if (members[i].name == "type_id_") found_type = true;
        if (members[i].name == "tag_") found_tag = true;
        if (members[i].name == "string_val_") found_str = true;
    }
    CHECK(found_int, "field 'int_val_' found");
    CHECK(found_type, "field 'type_id_' found");
    CHECK(found_tag, "field 'tag_' found");
    CHECK(found_str, "field 'string_val_' found");

    // Roundtrip test: all 4 columns populated
    FlatASTLikeSoA original;
    original.int_val_ = {0xCAFE0001, 0xCAFE0002, 0xCAFE0003};
    original.type_id_ = {101, 102};
    original.tag_ = {1, 2, 3, 4, 5};
    original.string_val_ = {"hello", "world"};

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("FlatASTLikeSoA serialized: {} bytes", buf.size());
    // Cycle 11 format:
    //   int_val_   (vector<int64_t>):  4 + 3*8 = 28 bytes
    //   type_id_   (vector<uint32_t>): 4 + 2*4 = 12 bytes
    //   tag_       (vector<uint8_t>):  4 + 5*1 =  9 bytes
    //   string_val_ (vector<string>):   4 + (4+5) + (4+5) = 22 bytes
    //                                 = 71 bytes total
    CHECK(buf.size() == 71, "buf size == 71 bytes");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<FlatASTLikeSoA>(buf, pos);
    CHECK(rt.int_val_.size() == 3, "int_val_ size == 3");
    if (rt.int_val_.size() == 3) {
        CHECK(rt.int_val_[0] == 0xCAFE0001, "int_val_[0]");
        CHECK(rt.int_val_[1] == 0xCAFE0002, "int_val_[1]");
        CHECK(rt.int_val_[2] == 0xCAFE0003, "int_val_[2]");
    }
    CHECK(rt.type_id_.size() == 2, "type_id_ size == 2");
    if (rt.type_id_.size() == 2) {
        CHECK(rt.type_id_[0] == 101, "type_id_[0]");
        CHECK(rt.type_id_[1] == 102, "type_id_[1]");
    }
    CHECK(rt.tag_.size() == 5, "tag_ size == 5");
    if (rt.tag_.size() == 5) {
        CHECK(rt.tag_[0] == 1, "tag_[0]");
        CHECK(rt.tag_[4] == 5, "tag_[4]");
    }
    CHECK(rt.string_val_.size() == 2, "string_val_ size == 2");
    if (rt.string_val_.size() == 2) {
        CHECK(rt.string_val_[0] == "hello", "string_val_[0]");
        CHECK(rt.string_val_[1] == "world", "string_val_[1]");
    }
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 16: NodeView production migration (Cycle 12) ───────
//
// Issue #217 Cycle 12: real NodeView in src/core/ast.ixx
// has 12 fields covering the union of all AST node data:
//   - POD scalars: id (NodeId=u32), tag (NodeTag=u32-enum),
//     int_value (i64), float_value (f64), sym_id (SymId=u32),
//     line (u32), col (u32), type_id (u32)
//   - 3 span fields: children (span<const NodeId>),
//     params (span<const SymId>), param_annotations
//     (span<const NodeId>). Two of the three have
//     identical element types (uint32_t) but different
//     semantic meaning — the reflect path must
//     distinguish them by FIELD, not by element type.
//   - marker (SyntaxMarker=u8-enum)
//
// This test verifies the generic reflect path handles
// the full NodeView shape. The struct below is a
// hand-written copy of the production NodeView (same
// field names + types, same field order) — the real
// NodeView can't be used directly here because
// tests/test_issue_217.cpp is a small standalone TU
// that doesn't import the aura.core.ast module.
//
// If this test passes, the production NodeView in
// src/core/ast.ixx can be auto_serialize'd /
// auto_deserialize'd without any custom overloads
// (the reflect path sees all 12 fields automatically).
// The actual production migration (adding a
// `NodeView astdoc::reflect::auto_deserialize<NodeView>(buf, pos)`
// call site in cache_reflect.cpp or similar) is a
// separate commit once the public API is confirmed.
struct NodeViewFullLike {
    std::uint32_t id = 0;
    std::uint32_t tag = 0;  // NodeTag enum (uint32-backed)
    std::int64_t int_value = 0;
    double float_value = 0.0;
    std::uint32_t sym_id = 0;  // SymId (uint32)
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::uint32_t type_id = 0;
    std::span<const std::uint32_t> children;     // span<const NodeId>
    std::span<const std::uint32_t> params;       // span<const SymId>
    std::span<const std::uint32_t> param_annotations;  // span<const NodeId>
    std::uint8_t marker = 0;  // SyntaxMarker enum (uint8-backed)
};
bool test_node_view_full() {
    PRINTLN("\n--- Test 16: NodeView full (production shape) ---");
    constexpr auto members = aura::reflect::reflect_members<NodeViewFullLike>();
    std::println("NodeViewFullLike has {} members", members.size());
    // 12 fields: 8 POD scalars + 3 spans + 1 enum-byte
    CHECK(members.size() == 12, "NodeViewFullLike has 12 members");
    // Verify all expected field names are found
    const char* expected[] = {
        "id", "tag", "int_value", "float_value", "sym_id",
        "line", "col", "type_id", "children", "params",
        "param_annotations", "marker"
    };
    for (auto& name : expected) {
        bool found = false;
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i].name == name) { found = true; break; }
        }
        CHECK(found, name);
    }
    // Verify the 3 span fields all have elem_size = 4
    // (uint32_t element type, regardless of semantic name)
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& m = members[i];
        if (m.name == "children" || m.name == "params" || m.name == "param_annotations") {
            CHECK(m.kind == aura::reflect::MemberKind::Span,
                  "span field kind");
            CHECK(m.elem_size == sizeof(std::uint32_t),
                  "span field elem_size = 4");
        }
    }

    // Roundtrip test: all 12 fields populated
    std::vector<std::uint32_t> children_data = {10, 20, 30};
    std::vector<std::uint32_t> params_data = {100, 200, 300, 400};
    std::vector<std::uint32_t> annot_data = {5, 6};

    NodeViewFullLike original;
    original.id = 42;
    original.tag = 0x03;  // Call
    original.int_value = 0xDEADBEEFCAFE0001LL;
    original.float_value = 3.14159;
    original.sym_id = 0xABCD;
    original.line = 100;
    original.col = 50;
    original.type_id = 0x12345;
    original.children = std::span<const std::uint32_t>(children_data.data(), children_data.size());
    original.params = std::span<const std::uint32_t>(params_data.data(), params_data.size());
    original.param_annotations = std::span<const std::uint32_t>(annot_data.data(), annot_data.size());
    original.marker = 1;  // MacroIntroduced

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("NodeViewFullLike serialized: {} bytes", buf.size());
    // Cycle 12 format:
    //   id (u32)            : 4
    //   tag (u32)           : 4
    //   int_value (i64)     : 8
    //   float_value (f64)   : 8
    //   sym_id (u32)        : 4
    //   line (u32)          : 4
    //   col (u32)           : 4
    //   type_id (u32)       : 4
    //   children span       : 4 + 3*4 = 16
    //   params span         : 4 + 4*4 = 20
    //   param_annotations   : 4 + 2*4 = 12
    //   marker (u8)         : 1
    //                       = 89 bytes
    CHECK(buf.size() == 89, "buf size == 89 bytes");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<NodeViewFullLike>(buf, pos);
    CHECK(rt.id == 42, "id");
    CHECK(rt.tag == 0x03, "tag");
    CHECK(rt.int_value == 0xDEADBEEFCAFE0001LL, "int_value");
    CHECK(rt.float_value == 3.14159, "float_value");
    CHECK(rt.sym_id == 0xABCD, "sym_id");
    CHECK(rt.line == 100, "line");
    CHECK(rt.col == 50, "col");
    CHECK(rt.type_id == 0x12345, "type_id");
    // The 3 span fields must be deserialized with the
    // CORRECT element counts (not just byte counts).
    // Cycle 8 fix already handles this: the deserialize
    // path divides byte_count by elem_size before storing
    // it in the span's size field.
    CHECK(rt.children.size() == 3, "children size == 3");
    if (rt.children.size() == 3) {
        CHECK(rt.children[0] == 10, "children[0]");
        CHECK(rt.children[1] == 20, "children[1]");
        CHECK(rt.children[2] == 30, "children[2]");
    }
    CHECK(rt.params.size() == 4, "params size == 4");
    if (rt.params.size() == 4) {
        CHECK(rt.params[0] == 100, "params[0]");
        CHECK(rt.params[3] == 400, "params[3]");
    }
    CHECK(rt.param_annotations.size() == 2, "param_annotations size == 2");
    if (rt.param_annotations.size() == 2) {
        CHECK(rt.param_annotations[0] == 5, "param_annotations[0]");
        CHECK(rt.param_annotations[1] == 6, "param_annotations[1]");
    }
    CHECK(rt.marker == 1, "marker");
    CHECK(pos == buf.size(), "all bytes consumed");
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
    test_span_uint32_roundtrip();
    test_mutation_record_roundtrip();
    test_match_clause_info_roundtrip();
    test_flatast_soa_columns();
    test_node_view_full();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
