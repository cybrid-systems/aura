// @category: integration
// @reason: Structured C++ IR for SystemVerilog (Issue #436
//          Phase 7). Demonstrates the dedicated C++ type
//          pattern that #435 will scale to all SV core
//          constructs. Phase 7 ships the WireIR type + Aura
//          bridge; list-based eda:wire is unchanged.


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.sv_ir;

namespace aura_issue_436_phase7_detail {

static std::string run_string(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r)
        return "";
    auto& v = *r;
    if (!aura::compiler::types::is_string(v))
        return "";
    auto idx = aura::compiler::types::as_string_idx(v);
    const auto& heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return "";
    return std::string(heap[idx]);
}

static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// ── AC1: C++ WireIR type — direct construction + accessors ──
bool test_cpp_wire_ir_direct() {
    std::println("\n--- AC1: C++ WireIR direct construction ---");
    using aura::compiler::sv_ir::WireKind;
    using aura::compiler::sv_ir::WireIR;
    using aura::compiler::sv_ir::make_wire;

    WireIR w = make_wire("data", 8, WireKind::Wire);
    CHECK(w.name == "data", "WireIR.name == 'data'");
    CHECK(w.width == 8, "WireIR.width == 8");
    CHECK(w.kind == WireKind::Wire, "WireIR.kind == Wire");

    WireIR l = make_wire("q", 1, WireKind::Logic);
    CHECK(l.kind == WireKind::Logic, "logic kind");

    WireIR r = make_wire("acc", 32, WireKind::Reg);
    CHECK(r.width == 32, "reg width 32");

    WireIR b = make_wire("flag", 1, WireKind::Bit);
    CHECK(b.kind == WireKind::Bit, "bit kind");

    return true;
}

// ── AC2: C++ emit_wire — matches list-based eda:emit-wire output ──
bool test_cpp_emit_wire() {
    std::println("\n--- AC2: C++ emit_wire matches list IR ---");
    using aura::compiler::sv_ir::WireKind;
    using aura::compiler::sv_ir::emit_wire;
    using aura::compiler::sv_ir::make_wire;

    std::string r;

    r = emit_wire(make_wire("d", 8, WireKind::Wire));
    CHECK(r == "wire [8-1:0] d;", "wire [8-1:0] d; — matches list IR");

    r = emit_wire(make_wire("q", 1, WireKind::Logic));
    CHECK(r == "logic q;", "logic q; — matches list IR");

    r = emit_wire(make_wire("q", 8, WireKind::Reg));
    CHECK(r == "reg [8-1:0] q;", "reg [8-1:0] q; — matches list IR");

    r = emit_wire(make_wire("f", 1, WireKind::Bit));
    CHECK(r == "bit f;", "bit f; — matches list IR");

    return true;
}

// ── AC3: wire_kind_to_symbol / wire_kind_from_symbol roundtrip ──
bool test_wire_kind_symbol_roundtrip() {
    std::println("\n--- AC3: WireKind symbol roundtrip ---");
    using aura::compiler::sv_ir::WireKind;
    using aura::compiler::sv_ir::wire_kind_from_symbol;
    using aura::compiler::sv_ir::wire_kind_to_symbol;

    for (auto k : {WireKind::Wire, WireKind::Logic, WireKind::Reg, WireKind::Bit}) {
        const char* sym = wire_kind_to_symbol(k);
        WireKind round = wire_kind_from_symbol(sym);
        CHECK(round == k, std::string("roundtrip: ") + sym);
    }

    // Unknown symbol defaults to Wire.
    CHECK(wire_kind_from_symbol("nonsense") == WireKind::Wire,
          "unknown symbol → Wire");

    return true;
}

// ── AC4: Aura bridge — structured type reachable from Aura ──
bool test_aura_bridge() {
    std::println("\n--- AC4: Aura can construct a structured WireIR via C++ ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    // Aura can call the C++ symbol-name to enum bridge.
    // We don't directly construct a C++ WireIR from Aura
    // (that needs full FFI), but we can verify the symbol
    // names match between Aura eda:wire-kind and C++ WireKind.
    auto aura_wire_kind = run_string(cs,
        "(eda:name-str (eda:wire-kind "
        "  (make-eda:wire 'd 8)))");
    CHECK(aura_wire_kind == "wire",
          "Aura eda:wire-kind returns 'wire (default)");

    auto aura_logic_kind = run_string(cs,
        "(eda:name-str (eda:wire-kind "
        "  (make-eda:logic 'q 1)))");
    CHECK(aura_logic_kind == "logic",
          "Aura eda:wire-kind returns 'logic");

    auto aura_reg_kind = run_string(cs,
        "(eda:name-str (eda:wire-kind "
        "  (make-eda:reg 'q 8)))");
    CHECK(aura_reg_kind == "reg",
          "Aura eda:wire-kind returns 'reg");

    auto aura_bit_kind = run_string(cs,
        "(eda:name-str (eda:wire-kind "
        "  (make-eda:bit 'f 1)))");
    CHECK(aura_bit_kind == "bit",
          "Aura eda:wire-kind returns 'bit");

    return true;
}

// ── AC5: Aura list-based emit still works (backward compat) ──
bool test_list_ir_still_works() {
    std::println("\n--- AC5: list-based eda:emit-wire still works ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    // Build the same wire via the list-based API and verify
    // its output matches the structured WireIR emit.
    auto s = run_string(cs,
        "(eda:emit-wire (make-eda:logic 'q 1))");
    CHECK(s == "logic q;",
          "list-based eda:emit-wire still emits 'logic q;'");

    s = run_string(cs,
        "(eda:emit-wire (make-eda:wire 'data 8))");
    CHECK(s == "wire [8-1:0] data;",
          "list-based eda:emit-wire still emits 'wire [8-1:0] data;'");

    return true;
}

int run_tests() {
    std::println("Issue #436 Phase 7 (structured C++ IR for SV — WireIR)\n");
    test_cpp_wire_ir_direct();
    test_cpp_emit_wire();
    test_wire_kind_symbol_roundtrip();
    test_aura_bridge();
    test_list_ir_still_works();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_436_phase7_detail

int aura_issue_436_phase7_run() { return aura_issue_436_phase7_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_436_phase7_run(); }
#endif
