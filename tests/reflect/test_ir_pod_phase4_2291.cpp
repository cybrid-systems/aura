// Issue #2291 — Phase 4 kickoff: pure-POD IR types via reflection.
//
// Proves IRInstruction, IRFunctionHeader, OpcodeArity have no
// field-by-field serialize path: only auto_serialize / validate / to_json.
// Also checks CacheHeader member reflection + round-trip.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "reflect/cache_format.h"
#include "reflect/ir_pod_reflect.hh"
#include "reflect/opcode_reflect.hh"
#include "reflect/reflect.hh"

namespace {

int g_failed = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

// Manual byte packing for IRInstruction — only for parity against
// auto_serialize (documents the reflection wire layout).
std::vector<char> hand_pack_instruction(const aura::ir_pod::IRInstruction& ins) {
    std::vector<char> b;
    b.reserve(64);
    auto push_n = [&](const void* p, std::size_t n) {
        const char* c = static_cast<const char*>(p);
        b.insert(b.end(), c, c + n);
    };
    auto op = static_cast<std::uint8_t>(ins.opcode);
    push_n(&op, 1);
    push_n(ins.operands.data(), sizeof(ins.operands));
    push_n(&ins.source_ast_node_id, 4);
    push_n(&ins.type_id, 4);
    push_n(&ins.shape_id, 4);
    push_n(&ins.linear_ownership_state, 1);
    push_n(&ins.adt_variant_id, 4);
    push_n(&ins.narrow_evidence, 4);
    push_n(&ins.source_marker, 1);
    push_n(&ins.provenance, 4);
    return b;
}

} // namespace

int run_test_ir_pod_phase4_2291() {
    using namespace aura::ir_pod;

    // ── member anchors ───────────────────────────────────────
    constexpr auto im = aura::reflect::reflect_members<IRInstruction>();
    constexpr auto hm = aura::reflect::reflect_members<IRFunctionHeader>();
    constexpr auto am = aura::reflect::reflect_members<OpcodeArity>();
    static_assert(im.size() == 10);
    static_assert(hm.size() == 9);
    static_assert(am.size() == 2);
    check(im[0].name == "opcode", "instr member opcode");
    check(hm[0].name == "id", "header member id");
    check(am[0].name == "operand_count", "arity member");

    // ── IRInstruction round-trip + parity with hand pack ─────
    IRInstruction ins{};
    ins.opcode = IROpcode::Add;
    ins.operands = {10, 20, 30, 0};
    ins.source_ast_node_id = 99;
    ins.type_id = 7;
    ins.shape_id = 3;
    ins.linear_ownership_state = 1;
    ins.adt_variant_id = 0;
    ins.narrow_evidence = 4;
    ins.source_marker = 1;
    ins.provenance = 42;

    auto ser = pod_serialize(ins);
    auto hand = hand_pack_instruction(ins);
    check(ser == hand, "IRInstruction auto_serialize == hand pack");
    check(ser.size() == 43, "IRInstruction wire size 43");

    auto back = pod_deserialize<IRInstruction>(ser);
    check(back.opcode == IROpcode::Add, "rt opcode");
    check(back.operands[0] == 10 && back.operands[2] == 30, "rt operands");
    check(back.type_id == 7 && back.provenance == 42, "rt ids");
    check(back.source_marker == 1, "rt marker");

    std::string err;
    check(validate_instruction(ins, &err), "validate good instr");
    IRInstruction bad = ins;
    bad.opcode = static_cast<IROpcode>(200);
    check(!validate_instruction(bad, &err), "validate bad opcode");

    const std::string j = pod_json(ins);
    check(j.find("\"opcode\":5") != std::string::npos, "json opcode Add=5");
    check(j.find("\"operands\":[10,20,30,0]") != std::string::npos, "json operands");

    // ── IRFunctionHeader ─────────────────────────────────────
    IRFunctionHeader hdr{};
    hdr.id = 11;
    hdr.entry_block = 2;
    hdr.local_count = 8;
    hdr.arg_count = 3;
    hdr.variadic = true;
    hdr.region = Region::Evolution;
    hdr.marker = 1;
    hdr.specialized_for = 5;
    hdr.generic_id = 1;

    auto hs = pod_serialize(hdr);
    auto hb = pod_deserialize<IRFunctionHeader>(hs);
    check(hb.id == 11 && hb.local_count == 8 && hb.variadic, "hdr rt");
    check(hb.region == Region::Evolution && hb.generic_id == 1, "hdr region");
    check(validate_function_header(hdr), "hdr validate");
    IRFunctionHeader hbad = hdr;
    hbad.local_count = 50'000'000u;
    check(!validate_function_header(hbad), "hdr reject huge local_count");

    // ── OpcodeArity ──────────────────────────────────────────
    OpcodeArity ar{.operand_count = 3, .has_result_slot = true};
    auto as = pod_serialize(ar);
    check(as.size() == 2, "arity size");
    auto ab = pod_deserialize<OpcodeArity>(as);
    check(ab.operand_count == 3 && ab.has_result_slot, "arity rt");
    check(validate_opcode_arity(ar), "arity validate");
    OpcodeArity arbad{.operand_count = 9, .has_result_slot = false};
    check(!validate_opcode_arity(arbad), "arity reject >4");

    // Names from opcode_reflect stay consistent
    check(aura::reflect::opcode_name<IROpcode>(static_cast<int>(IROpcode::Add)) == "Add",
          "opcode_name Add");

    // ── CacheHeader (Wave A2: C array magic[8] as Array) ─────
    constexpr auto cm = aura::reflect::reflect_members<CacheHeader>();
    static_assert(cm.size() == 12);
    check(cm[5].name == "magic", "cache magic member name");
    check(cm[5].kind == aura::reflect::MemberKind::Array, "cache magic is Array");
    check(cm[5].elem_size == 1 && cm[5].array_len == 8, "cache magic 8x char");
    CacheHeader ch{};
    std::memcpy(ch.magic, "AURACACH", 8);
    ch.version = 4;
    ch.num_nodes = 1;
    ch.node_offset = 72;
    auto cs = aura::reflect::auto_serialize(ch);
    check(cs.size() == sizeof(CacheHeader), "cache ser size == 72");
    auto cb = aura::reflect::auto_deserialize<CacheHeader>(cs);
    check(cb.version == 4 && cb.num_nodes == 1, "cache header rt scalars");
    check(cb.node_offset == 72, "cache node_offset rt");
    check(std::memcmp(cb.magic, "AURACACH", 8) == 0, "cache magic rt");
    check(aura::reflect::auto_validate(ch), "cache auto_validate");
    check(cache_validate_header(&ch) == 0, "cache_validate_header ok");
    check(cache_validate_header(&cb) == 0, "validate after deserialize");
    CacheHeader ch_bad = ch;
    ch_bad.version = 99;
    check(cache_validate_header(&ch_bad) == -2, "cache reject bad version");

    std::printf("test_ir_pod_phase4_2291: %s (failed=%d)\n", g_failed == 0 ? "PASS" : "FAIL",
                g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_ir_pod_phase4_2291();
}
#endif
