// Wave B2: FlatInstruction auto_serialize round-trip + IR field overlap.

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "reflect/flat_instr_reflect.hh"
#include "reflect/reflect.hh"

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

bool has_member(std::string_view name) {
    constexpr auto m = aura::reflect::reflect_members<aura::jit::FlatInstruction>();
    for (auto& x : m)
        if (x.name == name)
            return true;
    return false;
}

bool ir_has(std::string_view name) {
    constexpr auto m = aura::reflect::reflect_members<aura::ir_pod::IRInstruction>();
    for (auto& x : m)
        if (x.name == name)
            return true;
    return false;
}

} // namespace

int main() {
    using namespace aura::reflect::flat_instr;

    constexpr auto fm = aura::reflect::reflect_members<FlatInstruction>();
    static_assert(fm.size() == 9);
    check(fm[0].name == "opcode", "flat opcode");
    check(fm[1].name == "ops", "flat ops");
    check(fm[1].kind == aura::reflect::MemberKind::Array, "ops is C array");
    check(fm[1].array_len == 4 && fm[1].elem_size == 4, "ops 4x u32");

    // Shared conceptual fields with IRInstruction
    for (const char* n : {"type_id", "shape_id", "linear_ownership_state", "narrow_evidence",
                          "source_marker", "provenance"}) {
        check(has_member(n) && ir_has(n), n);
    }
    check(has_member("ops") && ir_has("operands"), "ops/operands pair");
    check(has_member("dirty") && !ir_has("dirty"), "dirty flat-only");
    check(ir_has("source_ast_node_id") && !has_member("source_ast_node_id"), "ast id ir-only");
    check(ir_has("adt_variant_id") && !has_member("adt_variant_id"), "adt ir-only");

    FlatInstruction in{};
    in.opcode = 5; // Add
    in.ops[0] = 10;
    in.ops[1] = 20;
    in.ops[2] = 30;
    in.ops[3] = 0;
    in.shape_id = 7;
    in.narrow_evidence = 4;
    in.type_id = 9;
    in.linear_ownership_state = 1;
    in.dirty = 0;
    in.source_marker = 1;
    in.provenance = 42;

    auto bytes = serialize(in);
    check(!bytes.empty(), "serialize non-empty");
    // Field-packed wire (no struct padding); sizeof may be larger.
    check(bytes.size() == 39, "wire 39 bytes field-packed");
    check(bytes.size() <= sizeof(FlatInstruction), "wire <= sizeof");

    auto out = deserialize(bytes);
    check(out.opcode == 5, "rt opcode");
    check(out.ops[0] == 10 && out.ops[1] == 20 && out.ops[2] == 30, "rt ops");
    check(out.shape_id == 7 && out.type_id == 9, "rt shape/type");
    check(out.narrow_evidence == 4 && out.provenance == 42, "rt evidence/prov");
    check(out.linear_ownership_state == 1 && out.source_marker == 1, "rt meta bytes");
    check(out.dirty == 0, "rt dirty");

    std::string err;
    check(validate(in, &err), "validate good");
    FlatInstruction bad = in;
    bad.opcode = 200;
    check(!validate(bad, &err), "validate bad opcode");

    const std::string j = json(in);
    check(j.find("\"opcode\":5") != std::string::npos, "json opcode");
    check(j.find("\"ops\":[10,20,30,0]") != std::string::npos, "json ops array");
    check(j.find("\"provenance\":42") != std::string::npos, "json provenance");

    // IR ↔ Flat overlap projection
    IRInstruction ir{};
    ir.opcode = aura::ir_pod::IROpcode::Add;
    ir.operands = {10, 20, 30, 0};
    ir.shape_id = 7;
    ir.type_id = 9;
    ir.narrow_evidence = 4;
    ir.linear_ownership_state = 1;
    ir.source_marker = 1;
    ir.provenance = 42;
    ir.source_ast_node_id = 99;
    ir.adt_variant_id = 3;

    auto flat = from_ir(ir, /*dirty=*/0);
    check(flat.opcode == 5 && flat.ops[0] == 10, "from_ir opcode/ops");
    check(flat.type_id == 9 && flat.provenance == 42, "from_ir meta");

    auto ir2 = to_ir_overlap(flat);
    check(ir2.opcode == aura::ir_pod::IROpcode::Add, "to_ir opcode");
    check(ir2.operands[1] == 20 && ir2.shape_id == 7, "to_ir fields");
    check(ir2.source_ast_node_id == 0 && ir2.adt_variant_id == 0, "ir-only cleared");

    // Round-trip through wire preserves from_ir projection
    auto flat2 = deserialize(serialize(flat));
    check(flat2.opcode == flat.opcode && flat2.ops[2] == 30, "from_ir wire rt");

    std::printf("test_flat_instr_reflect_b2: %s (failed=%d)\n", g_failed ? "FAIL" : "PASS",
                g_failed);
    return g_failed == 0 ? 0 : 1;
}
