// @category: unit
// @reason: pure C++ — sv_ir SVInterfaceIR / SVModportIR + AST mapping
// test_issue_315.cpp — Verify Issue #315 acceptance criteria
// ("feat(ir): define minimal SVInterfaceIR and SVModportIR
//  structures").
//
// Scope-limited close. The issue body asks for the SymId-
// based structured C++ IR for SV interfaces + modports.
// sv_ir.ixx already shipped the std::string-based list-
// emit-friendly #435 variants (InterfaceIR / ModportIR)
// in Phase 1+2. This PR adds the SymId-based parallel
// layer (SVInterfaceIR / SVModportIR) that the lowering
// pass + AST mutation pipeline will use directly
// (SymId is stable across sessions and directly
// comparable).
//
// 3 ACs:
//   AC1 IR 结构体可构造
//        (SVInterfaceIR + SVModportIR are constructible
//        via make_sv_interface / make_sv_modport; both
//        fields populated correctly)
//   AC2 与 AST NodeId 有基础双向映射
//        (map_interface_node_to_ir walks the Interface
//        AST node, collects nested Modport children +
//        their port-list symbols; the inverse path
//        (debug helpers) resolves SymIds back to strings
//        via StringPool)
//   AC3 编译通过
//        (the new module compiles + links into
//        aura_test_objects.a; existing tests still pass)


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.compiler.sv_ir;

namespace aura_issue_315_detail {

void check_eq_local_(std::size_t a, std::size_t b, const char* msg, int line) {
    if (a == b) {
        std::println("  PASS: {}", msg);
        ++g_passed;
    } else {
        std::println("  FAIL: {} (got {} expected {} line {})", msg, a, b, line);
        ++g_failed;
    }
}
void check_local_(bool cond, const char* msg, int line) {
    if (cond) {
        std::println("  PASS: {}", msg);
        ++g_passed;
    } else {
        std::println("  FAIL: {} (line {})", msg, line);
        ++g_failed;
    }
}
#define CHECK_EQ_LOCAL(a, b, msg) check_eq_local_((std::size_t)(a), (std::size_t)(b), msg, __LINE__)
// CHECK is provided by test_harness.hpp (included above).
// Do not redefine it here — under -Werror the redefinition is
// fatal and breaks the whole build (pre-existing bug from the
// import std migration, 8d3e42b7).

// ═══════════════════════════════════════════════════════════════
// AC1: SVInterfaceIR + SVModportIR constructible
// ═══════════════════════════════════════════════════════════════

bool test_sv_interface_ir_constructible() {
    std::println("\n--- AC1: SVInterfaceIR + SVModportIR constructible ---");
    using namespace aura;
    ast::StringPool pool;
    // Build port lists + nested modports via the constructors.
    auto port_a = pool.intern("data");
    auto port_b = pool.intern("valid");
    auto master = pool.intern("master");
    auto slave = pool.intern("slave");
    auto master_mp = compiler::sv_ir::make_sv_modport(master, {port_a, port_b});
    auto slave_mp = compiler::sv_ir::make_sv_modport(slave, {port_b, port_a});
    CHECK_EQ_LOCAL(master_mp.name, master, "master modport name matches");
    CHECK_EQ_LOCAL(master_mp.port_names.size(), 2, "master modport has 2 port names");
    CHECK_EQ_LOCAL(master_mp.port_names[0], port_a, "master port_names[0] is data");
    CHECK_EQ_LOCAL(master_mp.port_names[1], port_b, "master port_names[1] is valid");
    auto bus = pool.intern("Bus");
    auto bus_ir = compiler::sv_ir::make_sv_interface(
        bus, std::move(master_mp), std::move(slave_mp));
    CHECK_EQ_LOCAL(bus_ir.name, bus, "Bus SVInterfaceIR name matches");
    CHECK_EQ_LOCAL(bus_ir.modports.size(), 2, "Bus SVInterfaceIR has 2 modports");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: AST → IR mapping (Interface + nested Modport)
// ═══════════════════════════════════════════════════════════════

bool test_map_interface_node_to_ir() {
    std::println("\n--- AC2: map_interface_node_to_ir walks AST ---");
    using namespace aura;
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    // Build port-list symbols first.
    auto data_sym = pool.intern("data");
    auto valid_sym = pool.intern("valid");
    // Build a Variable (signal decl — should be skipped by the IR walker).
    auto data_var = flat.add_variable(data_sym);
    auto valid_var = flat.add_variable(valid_sym);
    // Build 2 Modport nodes, each with a 2-port list.
    std::vector<SymId> master_ports{data_sym, valid_sym};
    auto master_mp = flat.add_modport(pool.intern("master"),
                                      std::span<const SymId>(master_ports));
    std::vector<SymId> slave_ports{valid_sym, data_sym};
    auto slave_mp = flat.add_modport(pool.intern("slave"),
                                     std::span<const SymId>(slave_ports));
    // Build the Interface node with the 4 body items.
    std::vector<NodeId> body{data_var, valid_var, master_mp, slave_mp};
    auto bus = flat.add_interface(pool.intern("Bus"), std::span<const NodeId>(body));
    // AC2: map_interface_node_to_ir on the Interface NodeId.
    auto ir_opt = compiler::sv_ir::map_interface_node_to_ir(flat, pool, bus);
    CHECK(ir_opt.has_value(),
          "map_interface_node_to_ir returns a value for Interface node");
    if (!ir_opt) return false;
    auto& ir = *ir_opt;
    CHECK_EQ_LOCAL(ir.name, std::uint32_t{pool.intern("Bus")},
                   "IR.name matches the interface's SymId");
    // Only Modport children are reflected in IR (Variables are skipped).
    CHECK_EQ_LOCAL(ir.modports.size(), 2,
                   "IR collects 2 modports (Variables silently skipped)");
    // Resolve port lists via SymId → pool.resolve.
    CHECK_EQ_LOCAL(ir.modports[0].port_names.size(), 2,
                   "master modport has 2 port names");
    CHECK_EQ_LOCAL(ir.modports[1].port_names.size(), 2,
                   "slave modport has 2 port names");
    // Debug format with the pool — SymIds resolve back to strings.
    auto dbg = compiler::sv_ir::debug_sv_interface(ir, pool);
    CHECK(dbg.find("interface Bus") != std::string::npos,
          "debug_sv_interface contains 'interface Bus'");
    CHECK(dbg.find("master") != std::string::npos,
          "debug_sv_interface contains 'master' modport name");
    CHECK(dbg.find("slave") != std::string::npos,
          "debug_sv_interface contains 'slave' modport name");
    CHECK(dbg.find("data") != std::string::npos,
          "debug_sv_interface resolves 'data' port");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2 (cont): non-Interface nodes → nullopt
// ═══════════════════════════════════════════════════════════════

bool test_map_non_interface_returns_nullopt() {
    std::println("\n--- AC2 (cont): non-Interface → nullopt ---");
    using namespace aura;
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto lit = flat.add_literal(42);
    auto var = flat.add_variable(pool.intern("x"));
    auto begin = flat.add_begin({lit, var});
    CHECK(!compiler::sv_ir::map_interface_node_to_ir(flat, pool, lit).has_value(),
          "LiteralInt → nullopt (not an Interface)");
    CHECK(!compiler::sv_ir::map_interface_node_to_ir(flat, pool, var).has_value(),
          "Variable → nullopt (not an Interface)");
    CHECK(!compiler::sv_ir::map_interface_node_to_ir(flat, pool, begin).has_value(),
          "Begin → nullopt (not an Interface)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #315 (SVInterfaceIR + SVModportIR) ═══\n");
    test_sv_interface_ir_constructible();
    test_map_interface_node_to_ir();
    test_map_non_interface_returns_nullopt();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_315_detail

int aura_issue_315_run() { return aura_issue_315_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_315_run(); }
#endif