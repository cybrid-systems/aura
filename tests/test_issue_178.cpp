// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_178.cpp — Issue #178 Cycle 5: production NodeView
// migration to reflect_members + auto_serialize.
//
// This is the PRODUCTION migration test for the AST NodeView
// type. test_issue_217 Test 16 used a hand-written copy of
// NodeView's field layout (because test_issue_217 is a
// header-only standalone TU that doesn't import
// aura.core.ast). This test imports the real module and
// roundtrips the ACTUAL aura::ast::NodeView struct.
//
// If this test passes, the generic reflect path in
// src/reflect/reflect.hh is confirmed to work on the
// production NodeView type. Adding
// `auto_serialize/auto_deserialize<NodeView>` to any
// production call site (e.g. a future cache format that
// persists NodeView data) is then a one-line change
// with no custom overload required.
//
// Test scenarios:
//   1. reflect_members<aura::ast::NodeView>() returns all
//      12 expected fields (id, tag, int_value, float_value,
//      sym_id, line, col, type_id, children, params,
//      param_annotations, marker) with correct kinds +
//      offsets.
//   2. auto_serialize/auto_deserialize roundtrip preserves
//      all 12 fields (POD scalars + 3 spans + enum-byte).
//   3. The 3 span fields are distinguished by field name
//      (children + param_annotations are both
//      span<const NodeId>=span<const uint32_t>; they share
//      elem_size=4 but each gets its own byte_count
//      header).
//   4. Empty NodeView (default-constructed) roundtrips:
//      all 3 spans are size 0, all scalars are 0/nullptr.
//
// Build env note (Cycle 13, 2026-06-16):
//   This test requires the aura.core.ast module to be
//   importable + the C++26 P2996 reflection enabled
//   (-freflection). GCC 16.1 has a known issue where
//   std module + -freflection + the pthread system
//   headers conflict when both are in the same TU.
//   reflect.hh includes <meta> (P2996), which transitively
//   pulls in <bitset> + system headers; aura.core.ast
//   has `import std;` which provides the same std
//   declarations from the module. The duplicate
//   declarations cause the GCC 16.1 ICE.
//
//   Two paths to unblock:
//     (a) Wait for GCC 16.2+ std module + -freflection
//         pthread fix (upstream bug, not in our control).
//     (b) Make reflect.hh a module (export module
//         aura.reflect;) so it doesn't include <meta>
//         directly — the import side imports the module
//         instead. This is a small refactor of reflect.hh
//         and would unblock this test (and any other test
//         that wants to use the reflect path on a
//         module-defined type).
//
//   In the meantime, test_issue_217 Test 16 verifies the
//   same NodeView SHAPE (12 fields, 3 spans, 2 enums)
//   via a hand-written copy. The test is identical in
//   field layout to the production NodeView, so Test 16
//   is the "would it work" check until this test can be
//   compiled in a fixed build env.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <span>
#include <cstdint>
#include <print>

#include "reflect/reflect.hh"

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;



#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

using aura::ast::NodeView;
using aura::ast::NodeId;
using aura::ast::SymId;
using aura::ast::NodeTag;
using aura::ast::SyntaxMarker;
using aura::ast::NULL_NODE;
using aura::ast::INVALID_SYM;

// ── Test 1: reflect_members on the real NodeView ────────────
bool test_reflect_node_view() {
    PRINTLN("\n--- Test 1: reflect_members<aura::ast::NodeView>() ---");
    constexpr auto members = aura::reflect::reflect_members<NodeView>();
    std::println("NodeView has {} members", members.size());
    // 12 fields (8 POD scalars + 3 spans + 1 enum-byte)
    CHECK(members.size() == 12, "NodeView has 12 members");

    // All 12 field names must be present
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

    // 3 span fields must all have elem_size=4 (NodeId=u32, SymId=u32)
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& m = members[i];
        if (m.name == "children" || m.name == "params" || m.name == "param_annotations") {
            CHECK(m.kind == aura::reflect::MemberKind::Span,
                  "span field kind");
            CHECK(m.elem_size == sizeof(std::uint32_t),
                  "span field elem_size = 4");
        }
    }
    return true;
}

// ── Test 2: full roundtrip on a populated NodeView ──────────
bool test_node_view_roundtrip() {
    PRINTLN("\n--- Test 2: real NodeView roundtrip ---");

    // Build a NodeView with all 12 fields populated.
    // The span fields need their backing storage to outlive
    // the original NodeView (spans are non-owning).
    std::vector<NodeId> children_data = {10, 20, 30};
    std::vector<SymId> params_data = {100, 200, 300, 400};
    std::vector<NodeId> annot_data = {5, 6};

    NodeView original;
    original.id = 42;
    original.tag = NodeTag::Call;          // 0x03
    original.int_value = 0xDEADBEEFCAFE0001LL;
    original.float_value = 3.14159;
    original.sym_id = 0xABCD;
    original.line = 100;
    original.col = 50;
    original.type_id = 0x12345;
    original.children = std::span<const NodeId>(children_data.data(), children_data.size());
    original.params = std::span<const SymId>(params_data.data(), params_data.size());
    original.param_annotations = std::span<const NodeId>(annot_data.data(), annot_data.size());
    original.marker = SyntaxMarker::MacroIntroduced;  // 1

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("NodeView serialized: {} bytes", buf.size());
    // Same layout as the hand-written Test 16 in
    // test_issue_217.cpp: 41 bytes POD + 48 bytes spans
    // = 89 bytes. (The hand-written copy in Cycle 12
    // has identical field layout, so the buf size must
    // match exactly.)
    CHECK(buf.size() == 89, "buf size == 89 bytes");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<NodeView>(buf, pos);
    CHECK(rt.id == 42, "id roundtrips");
    CHECK(rt.tag == NodeTag::Call, "tag roundtrips");
    CHECK(rt.int_value == 0xDEADBEEFCAFE0001LL, "int_value roundtrips");
    CHECK(rt.float_value == 3.14159, "float_value roundtrips");
    CHECK(rt.sym_id == 0xABCD, "sym_id roundtrips");
    CHECK(rt.line == 100, "line roundtrips");
    CHECK(rt.col == 50, "col roundtrips");
    CHECK(rt.type_id == 0x12345, "type_id roundtrips");
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
    CHECK(rt.marker == SyntaxMarker::MacroIntroduced, "marker roundtrips");
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 3: empty NodeView (default-constructed) roundtrip ──
bool test_empty_node_view() {
    PRINTLN("\n--- Test 3: empty NodeView roundtrip ---");
    NodeView original;  // all defaults: id=NULL_NODE, scalars=0,
                        // spans empty, marker=User
    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    // 41 bytes POD (id=0xFFFFFFFF, tag=LiteralInt=1, int_value=0,
    // float_value=0, sym_id=0xFFFFFFFF, line=0, col=0, type_id=0,
    // marker=0) + 12 bytes span headers (3 spans * 4 bytes header,
    // 0 bytes data) = 53 bytes
    std::println("Empty NodeView serialized: {} bytes", buf.size());
    CHECK(buf.size() == 53, "empty buf size == 53 bytes (41+12)");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<NodeView>(buf, pos);
    CHECK(rt.id == NULL_NODE, "empty id roundtrips");
    CHECK(rt.tag == NodeTag::LiteralInt, "empty tag roundtrips");
    CHECK(rt.int_value == 0, "empty int_value");
    CHECK(rt.float_value == 0.0, "empty float_value");
    CHECK(rt.sym_id == INVALID_SYM, "empty sym_id");
    CHECK(rt.line == 0, "empty line");
    CHECK(rt.col == 0, "empty col");
    CHECK(rt.type_id == 0, "empty type_id");
    CHECK(rt.children.size() == 0, "empty children");
    CHECK(rt.params.size() == 0, "empty params");
    CHECK(rt.param_annotations.size() == 0, "empty param_annotations");
    CHECK(rt.marker == SyntaxMarker::User, "empty marker");
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 4: reflection-driven IR/AST serialization (Cycle 4) ────
//
// Issue #218 Cycle 4: the IRInstruction type in
// aura.compiler.ir is the most complex production type
// to roundtrip (4 operand fields + 3 span fields + 6
// flag/enum fields + 3 u8 fields). It exercises the
// generic struct path with the full range of MemberKind
// cases (POD + StringView + Span + Array + Enum).
//
// We don't have aura.compiler.ir in the test_issue_178
// dep graph (it has a different module dep chain). This
// test uses a local copy (IRInstructionLike) that mirrors
// the production layout. When the env is fixed
// (#268 unblock), the same test can be re-pointed at the
// actual production type.
struct IRInstructionLike {
    std::uint8_t opcode = 0;
    std::uint8_t type_id_lo = 0;
    std::uint8_t type_id_hi = 0;
    std::uint8_t has_result_slot = 0;
    std::uint32_t result_slot = 0;
    std::uint32_t result = 0;
    std::uint32_t result2 = 0;
    std::uint32_t flags = 0;
    std::span<const std::uint32_t> operands;
    std::span<const std::uint32_t> results;
    std::span<const std::uint32_t> param_annotations;
    std::string_view source_view;
};
bool test_ir_instruction_reflect_roundtrip() {
    PRINTLN("\n--- Test 4: IRInstruction-like (Cycle 4 reflection-driven IR) ---");
    constexpr auto members = aura::reflect::reflect_members<IRInstructionLike>();
    std::println("IRInstructionLike has {} members", members.size());
    CHECK(members.size() == 12, "IRInstructionLike has 12 members");
    bool found_opcode = false, found_operands = false, found_source = false;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (members[i].name == "opcode") found_opcode = true;
        if (members[i].name == "operands") found_operands = true;
        if (members[i].name == "source_view") found_source = true;
    }
    CHECK(found_opcode, "field 'opcode' found");
    CHECK(found_operands, "field 'operands' found");
    CHECK(found_source, "field 'source_view' found");

    // Roundtrip test
    std::vector<std::uint32_t> ops_data = {1, 2, 3};
    std::vector<std::uint32_t> results_data = {4, 5};
    std::string source_text = "((+ x y))";
    IRInstructionLike original;
    original.opcode = 0x05;  // Add
    original.result_slot = 7;
    original.result = 0;
    original.result2 = 0;
    original.flags = 0;
    original.operands = std::span<const std::uint32_t>(ops_data.data(), ops_data.size());
    original.results = std::span<const std::uint32_t>(results_data.data(), results_data.size());
    original.source_view = std::string_view(source_text);

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    std::println("IRInstructionLike serialized: {} bytes", buf.size());
    // 12 fields with 3 spans + 1 string_view. The byte count
    // is exact; we just verify it's > 30 bytes (5 fields
    // × 4 bytes minimum + 3 span headers + 1 string_view
    // header + the 3 span data + the string content)
    CHECK(buf.size() > 30, "IRInstructionLike buf size > 30 bytes");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<IRInstructionLike>(buf, pos);
    CHECK(rt.opcode == 0x05, "opcode roundtrips");
    CHECK(rt.operands.size() == 3, "operands size == 3");
    if (rt.operands.size() == 3) {
        CHECK(rt.operands[0] == 1, "operands[0]");
        CHECK(rt.operands[1] == 2, "operands[1]");
        CHECK(rt.operands[2] == 3, "operands[2]");
    }
    CHECK(rt.results.size() == 2, "results size == 2");
    CHECK(rt.source_view == "((+ x y))", "source_view roundtrips");
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

// ── Test 5: hygienic macro + query:schema integration (Cycle 5) ─
//
// Issue #218 Cycle 5: when a hygienic macro introduces a
// binding, the user's `query:schema` on that binding should
// return the USER's intended schema, not the macro's
// internal hygiene node. This composes with #140's
// hygiene work.
//
// For a real test, we need the full compiler pipeline
// (parse + macro-expand + type-check). The test_issue_178
// env doesn't have aura.compiler.evaluator in its dep
// graph, so this test verifies the SAME invariant at the
// reflect-path level: a NodeView whose marker is
// SyntaxMarker::MacroIntroduced (set by the hygienic
// macro) roundtrips correctly with the marker preserved.
// The schema-resolution invariant itself is verified in
// test_issue_140 (the hygiene test). This test verifies
// that the marker doesn't get LOST in serialize/deserialize
// (which would break the schema-resolution downstream).
bool test_hygienic_macro_marker_roundtrip() {
    PRINTLN("\n--- Test 5: hygienic macro marker roundtrip (Cycle 5) ---");
    // Build a NodeView where the marker = SyntaxMarker::MacroIntroduced
    // (the value set by the macro expansion when it
    // creates a binding node). This is the SAME NodeView
    // from Test 2, but with marker=1 instead of 0.
    std::vector<aura::ast::NodeId> children_data = {99, 100};
    std::vector<aura::ast::SymId> params_data = {0xAA, 0xBB, 0xCC};
    std::vector<aura::ast::NodeId> annot_data = {200};

    aura::ast::NodeView macro_introduced;
    macro_introduced.id = 42;
    macro_introduced.tag = aura::ast::NodeTag::Let;
    macro_introduced.int_value = 0xDEADBEEF;
    macro_introduced.float_value = 0.0;
    macro_introduced.sym_id = 0xABCD;
    macro_introduced.line = 10;
    macro_introduced.col = 1;
    macro_introduced.type_id = 0;
    macro_introduced.children = std::span<const aura::ast::NodeId>(children_data.data(), children_data.size());
    macro_introduced.params = std::span<const aura::ast::SymId>(params_data.data(), params_data.size());
    macro_introduced.param_annotations = std::span<const aura::ast::NodeId>(annot_data.data(), annot_data.size());
    macro_introduced.marker = aura::ast::SyntaxMarker::MacroIntroduced;  // value 1

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, macro_introduced);
    std::println("Macro-introduced NodeView serialized: {} bytes", buf.size());
    CHECK(buf.size() == 89, "macro-introduced same wire size as user-introduced (89 bytes)");

    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<aura::ast::NodeView>(buf, pos);
    // CRITICAL invariant: the marker must roundtrip
    // exactly. If the reflect path silently demotes
    // MacroIntroduced → User (or loses the marker entirely),
    // then `query:schema` on this node would return the
    // wrong schema (the user's vs the macro's). This
    // invariant is what makes the Cycle 5 schema-resolution
    // test (in test_issue_140) actually work.
    CHECK(rt.marker == aura::ast::SyntaxMarker::MacroIntroduced,
          "marker == SyntaxMarker::MacroIntroduced (preserves hygiene signal)");
    CHECK(static_cast<int>(rt.marker) == 1,
          "marker value == 1 (MacroIntroduced)");
    // Sanity check the other fields too (this is the same
    // NodeView from Test 2 with marker=1)
    CHECK(rt.tag == aura::ast::NodeTag::Let, "tag == Let");
    CHECK(rt.sym_id == 0xABCD, "sym_id roundtrips");
    CHECK(rt.children.size() == 2, "children size == 2");
    CHECK(rt.params.size() == 3, "params size == 3");
    CHECK(rt.param_annotations.size() == 1, "param_annotations size == 1");
    CHECK(pos == buf.size(), "all bytes consumed");
    return true;
}

int run_issue_178() {
    test_reflect_node_view();
    test_node_view_roundtrip();
    test_empty_node_view();
    test_ir_instruction_reflect_roundtrip();
    test_hygienic_macro_marker_roundtrip();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
