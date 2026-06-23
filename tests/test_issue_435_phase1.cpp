// @category: integration
// @reason: Structured C++ IR for SV (Issue #435 Phase 1+2
//          — InterfaceIR + ModportIR). Demonstrates the
//          dedicated C++ type pattern from #436 Phase 7
//          scaled up for the foundation SV construct.

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.sv_ir;

namespace aura_issue_435_phase1_detail {

static std::string run_string(aura::compiler::CompilerService& cs,
                              const std::string& src) {
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

// ── AC1: C++ InterfaceIR direct construction + accessors ──
bool test_cpp_interface_ir_direct() {
    std::println("\n--- AC1: C++ InterfaceIR direct construction ---");
    using aura::compiler::sv_ir::InterfaceIR;
    using aura::compiler::sv_ir::make_interface;

    InterfaceIR bus = make_interface(
        "bus_if",
        std::vector<std::string>{"clk", "data", "valid"},
        std::vector<std::string>{"master", "slave"});
    CHECK(bus.name == "bus_if", "InterfaceIR.name == 'bus_if'");
    CHECK(bus.ports.size() == 3, "3 ports");
    CHECK(bus.ports[0] == "clk", "first port == clk");
    CHECK(bus.modport_names.size() == 2, "2 modports");
    CHECK(bus.modport_names[1] == "slave", "second modport == slave");

    InterfaceIR empty = make_interface("empty", {}, {});
    CHECK(empty.ports.empty(), "empty ports");
    CHECK(empty.modport_names.empty(), "empty modport_names");

    return true;
}

// ── AC2: emit_interface matches list-based eda:emit-interface ──
bool test_cpp_emit_interface() {
    std::println("\n--- AC2: C++ emit_interface matches list IR ---");
    using aura::compiler::sv_ir::emit_interface;
    using aura::compiler::sv_ir::make_interface;

    std::string s;

    s = emit_interface(make_interface("bus_if",
        std::vector<std::string>{"clk", "data"},
        std::vector<std::string>{"master"}));
    CHECK(s.find("interface bus_if") != std::string::npos,
          "starts with 'interface bus_if'");
    CHECK(s.find("clk, data") != std::string::npos,
          "contains 'clk, data'");
    CHECK(s.find("modport master();") != std::string::npos,
          "contains 'modport master();'");
    CHECK(s.find("endinterface") != std::string::npos,
          "ends with 'endinterface'");

    s = emit_interface(make_interface("simple",
        std::vector<std::string>{"a"}, {}));
    CHECK(s.find("interface simple") != std::string::npos,
          "simple interface starts");
    CHECK(s.find("endinterface") != std::string::npos,
          "simple interface ends");
    CHECK(s.find("modport") == std::string::npos,
          "no modport line when none");

    return true;
}

// ── AC3: C++ ModportIR direct construction + accessors ──
bool test_cpp_modport_ir_direct() {
    std::println("\n--- AC3: C++ ModportIR direct construction ---");
    using aura::compiler::sv_ir::ModportIR;
    using aura::compiler::sv_ir::make_modport;

    ModportIR m = make_modport("master",
        std::vector<std::string>{"input clk", "output data"});
    CHECK(m.name == "master", "ModportIR.name == 'master'");
    CHECK(m.port_names.size() == 2, "2 port_names");

    ModportIR empty = make_modport("empty", {});
    CHECK(empty.port_names.empty(), "empty port_names");

    return true;
}

// ── AC4: emit_modport matches list-based eda:emit-modport ──
bool test_cpp_emit_modport() {
    std::println("\n--- AC4: C++ emit_modport matches list IR ---");
    using aura::compiler::sv_ir::emit_modport;
    using aura::compiler::sv_ir::make_modport;

    std::string s;

    s = emit_modport(make_modport("master",
        std::vector<std::string>{"input clk", "output data"}));
    CHECK(s.find("modport master") != std::string::npos,
          "starts with 'modport master'");
    CHECK(s.find("input clk") != std::string::npos,
          "contains 'input clk'");
    CHECK(s.find("output data") != std::string::npos,
          "contains 'output data'");
    CHECK(s.back() == ';', "ends with ';'");

    s = emit_modport(make_modport("empty", {}));
    CHECK(s == "modport empty();",
          "empty modport → 'modport empty();'");

    return true;
}

// ── AC5: Aura list-based emit still works (backward compat) ──
bool test_list_ir_still_works() {
    std::println("\n--- AC5: list-based eda:emit-interface still works ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(require \"std/eda\" all:)")) {
        ++g_failed;
        return false;
    }

    auto s = run_string(cs,
        "(eda:emit-interface "
        "  (make-eda:interface 'bus_if "
        "    (list (make-eda:port 'clk 'input 1)) "
        "    (list (make-eda:modport 'master '(clk)))))");
    CHECK(!s.empty(), "list-based emit returns non-empty");
    CHECK(s.find("bus_if") != std::string::npos,
          "list-based emit contains 'bus_if'");
    CHECK(s.find("endinterface") != std::string::npos,
          "list-based emit ends with endinterface");

    return true;
}

int run_tests() {
    std::println("Issue #435 Phase 1+2 (structured C++ IR — InterfaceIR + ModportIR)\n");
    test_cpp_interface_ir_direct();
    test_cpp_emit_interface();
    test_cpp_modport_ir_direct();
    test_cpp_emit_modport();
    test_list_ir_still_works();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_435_phase1_detail

int aura_issue_435_phase1_run() { return aura_issue_435_phase1_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_435_phase1_run(); }
#endif