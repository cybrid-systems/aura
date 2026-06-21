// Non-module TU: P2996 reflection (Issue #268).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "reflect/reflect.hh"
#include "nodeview_wire.hh"
#include "test_issue_178_bridge.h"

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond) {
    if (cond)
        ++g_passed;
    else
        ++g_failed;
}

NodeViewWire make_wire(
    std::uint32_t id, std::uint32_t tag, std::int64_t int_value, double float_value,
    std::uint32_t sym_id, std::uint32_t line, std::uint32_t col, std::uint32_t type_id,
    const std::uint32_t* children, std::size_t children_count,
    const std::uint32_t* params, std::size_t params_count,
    const std::uint32_t* annot, std::size_t annot_count, std::uint8_t marker) {
    NodeViewWire w;
    w.id = id;
    w.tag = static_cast<WireNodeTag>(tag);
    w.int_value = int_value;
    w.float_value = float_value;
    w.sym_id = sym_id;
    w.line = line;
    w.col = col;
    w.type_id = type_id;
    w.children = std::span<const std::uint32_t>(children, children_count);
    w.params = std::span<const std::uint32_t>(params, params_count);
    w.param_annotations = std::span<const std::uint32_t>(annot, annot_count);
    w.marker = static_cast<WireSyntaxMarker>(marker);
    return w;
}

bool roundtrip_wire(const NodeViewWire& original, std::size_t& out_bytes,
                    NodeViewWire* out = nullptr) {
    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    out_bytes = buf.size();
    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<NodeViewWire>(buf, pos);
    if (pos != buf.size())
        return false;
    if (out)
        *out = rt;
    if (rt.id != original.id || static_cast<std::uint32_t>(rt.tag) != static_cast<std::uint32_t>(original.tag))
        return false;
    if (rt.int_value != original.int_value || rt.float_value != original.float_value)
        return false;
    if (rt.sym_id != original.sym_id || rt.line != original.line || rt.col != original.col)
        return false;
    if (rt.type_id != original.type_id || rt.marker != original.marker)
        return false;
    if (rt.children.size() != original.children.size() || rt.params.size() != original.params.size())
        return false;
    if (rt.param_annotations.size() != original.param_annotations.size())
        return false;
    for (std::size_t i = 0; i < rt.children.size(); ++i)
        if (rt.children[i] != original.children[i]) return false;
    for (std::size_t i = 0; i < rt.params.size(); ++i)
        if (rt.params[i] != original.params[i]) return false;
    for (std::size_t i = 0; i < rt.param_annotations.size(); ++i)
        if (rt.param_annotations[i] != original.param_annotations[i]) return false;
    return true;
}

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

} // namespace

void issue178_reset_counters() {
    g_passed = 0;
    g_failed = 0;
}

int issue178_failed_count() { return g_failed; }

void issue178_run_reflect_member_tests() {
    constexpr auto members = aura::reflect::reflect_members<NodeViewWire>();
    check(members.size() == 12);
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& m = members[i];
        if (m.name == "children" || m.name == "params" || m.name == "param_annotations") {
            check(m.kind == aura::reflect::MemberKind::Span);
            check(m.elem_size == sizeof(std::uint32_t));
        }
    }
}

void issue178_run_ir_roundtrip_tests() {
    constexpr auto members = aura::reflect::reflect_members<IRInstructionLike>();
    check(members.size() == 12);

    std::array<std::uint32_t, 3> ops_data = {1, 2, 3};
    std::array<std::uint32_t, 2> results_data = {4, 5};
    std::string source_text = "((+ x y))";
    IRInstructionLike original;
    original.opcode = 0x05;
    original.result_slot = 7;
    original.operands = std::span<const std::uint32_t>(ops_data.data(), ops_data.size());
    original.results = std::span<const std::uint32_t>(results_data.data(), results_data.size());
    original.source_view = std::string_view(source_text);

    std::vector<char> buf;
    aura::reflect::auto_serialize(buf, original);
    check(buf.size() > 30);
    std::size_t pos = 0;
    auto rt = aura::reflect::auto_deserialize<IRInstructionLike>(buf, pos);
    check(rt.opcode == 0x05);
    check(rt.operands.size() == 3);
    check(rt.results.size() == 2);
    check(rt.source_view == "((+ x y))");
    check(pos == buf.size());
}

int issue178_roundtrip_populated(
    std::uint32_t id, std::uint32_t tag, std::int64_t int_value, double float_value,
    std::uint32_t sym_id, std::uint32_t line, std::uint32_t col, std::uint32_t type_id,
    const std::uint32_t* children, std::size_t children_count,
    const std::uint32_t* params, std::size_t params_count,
    const std::uint32_t* annot, std::size_t annot_count, std::uint8_t marker,
    std::size_t* out_bytes) {
    auto wire = make_wire(id, tag, int_value, float_value, sym_id, line, col, type_id, children,
                          children_count, params, params_count, annot, annot_count, marker);
    const bool ok = roundtrip_wire(wire, *out_bytes);
    check(ok);
    if (*out_bytes == 89)
        check(true);
    else
        check(false);
    return ok ? 1 : 0;
}

int issue178_roundtrip_empty(std::size_t* out_bytes) {
    NodeViewWire wire;
    const bool ok = roundtrip_wire(wire, *out_bytes);
    check(ok);
    check(*out_bytes == 53);
    return ok ? 1 : 0;
}

int issue178_roundtrip_verify_marker(std::uint8_t marker_out) {
    std::array<std::uint32_t, 2> children = {99, 100};
    std::array<std::uint32_t, 3> params = {0xAA, 0xBB, 0xCC};
    std::array<std::uint32_t, 1> annot = {200};
    auto wire = make_wire(42, static_cast<std::uint32_t>(WireNodeTag::Let), 0xDEADBEEF, 0.0, 0xABCD,
                          10, 1, 0, children.data(), children.size(), params.data(), params.size(),
                          annot.data(), annot.size(), 1);
    std::size_t bytes = 0;
    NodeViewWire rt{};
    const bool ok = roundtrip_wire(wire, bytes, &rt);
    check(ok);
    // 41 bytes POD + 12+16+8 span payload (2+3+1 elems) = 77 bytes
    check(bytes == 77);
    check(static_cast<std::uint8_t>(rt.marker) == 1);
    marker_out = static_cast<std::uint8_t>(rt.marker);
    (void)marker_out;
    return ok && static_cast<std::uint8_t>(rt.marker) == 1 ? 1 : 0;
}