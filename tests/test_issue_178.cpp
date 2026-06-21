// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_178.cpp — Issue #178 / #268: production NodeView
// roundtrip via reflect_members + auto_serialize.
//
// GCC 16.1 ICEs when one TU imports aura.core.ast and includes
// reflect.hh (<meta>). This module TU uses the REAL
// aura::ast::NodeView; reflection runs in
// test_issue_178_reflect.cpp (bridge API declared below).

import std;
import aura.core.ast;

void issue178_reset_counters();
int issue178_failed_count();
void issue178_run_reflect_member_tests();
void issue178_run_ir_roundtrip_tests();
int issue178_roundtrip_populated(
    std::uint32_t id, std::uint32_t tag, std::int64_t int_value, double float_value,
    std::uint32_t sym_id, std::uint32_t line, std::uint32_t col, std::uint32_t type_id,
    const std::uint32_t* children, std::size_t children_count,
    const std::uint32_t* params, std::size_t params_count,
    const std::uint32_t* annot, std::size_t annot_count, std::uint8_t marker,
    std::size_t* out_bytes);
int issue178_roundtrip_empty(std::size_t* out_bytes);
int issue178_roundtrip_verify_marker(std::uint8_t marker_out);
int issue178_run_stress_iterations(int iterations);

using aura::ast::NodeView;
using aura::ast::NodeId;
using aura::ast::SymId;
using aura::ast::NodeTag;
using aura::ast::SyntaxMarker;

int main() {
    issue178_reset_counters();
    issue178_run_reflect_member_tests();
    issue178_run_ir_roundtrip_tests();

    {
        NodeId children_data[] = {10, 20, 30};
        SymId params_data[] = {100, 200, 300, 400};
        NodeId annot_data[] = {5, 6};
        NodeView original;
        original.id = 42;
        original.tag = NodeTag::Call;
        original.int_value = 0xDEADBEEFCAFE0001LL;
        original.float_value = 3.14159;
        original.sym_id = 0xABCD;
        original.line = 100;
        original.col = 50;
        original.type_id = 0x12345;
        original.children = std::span<const NodeId>(children_data, 3);
        original.params = std::span<const SymId>(params_data, 4);
        original.param_annotations = std::span<const NodeId>(annot_data, 2);
        original.marker = SyntaxMarker::MacroIntroduced;

        std::size_t bytes = 0;
        issue178_roundtrip_populated(
            original.id, static_cast<std::uint32_t>(original.tag), original.int_value,
            original.float_value, original.sym_id, original.line, original.col, original.type_id,
            children_data, 3, params_data, 4, annot_data, 2,
            static_cast<std::uint8_t>(original.marker), &bytes);
        (void)bytes;
    }

    {
        std::size_t bytes = 0;
        issue178_roundtrip_empty(&bytes);
        (void)bytes;
    }

    {
        std::uint8_t marker = 0;
        issue178_roundtrip_verify_marker(marker);
    }

    // Issue #218: 1000+ serialize/deserialize iterations.
    // Skip when AURA_SKIP_STRESS=1 (fast CI path).
    if (const char* skip = std::getenv("AURA_SKIP_STRESS"); !skip || skip[0] == '\0') {
        constexpr int kStressIterations = 1000;
        issue178_run_stress_iterations(kStressIterations);
    }

    return issue178_failed_count() > 0 ? 1 : 0;
}