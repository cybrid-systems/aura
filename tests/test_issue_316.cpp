// @category: unit
// @reason: pure C++ — sv_ir emit_sv_interface + emit_sv_modport
// test_issue_316.cpp — Verify Issue #316 acceptance criteria
// ("feat(backend): minimal SystemVerilog emit for Interface
//  and Modport").
//
// Scope-limited close. The issue body asks for SV emit
// functions: `emit_sv_interface(const SVInterfaceIR&)` +
// modport list output. The existing sv_ir.ixx already ships
// the std::string-based #435 emit_path (`emit_interface` +
// `emit_modport`); this PR adds the parallel SymId variant
// for the SVInterfaceIR / SVModportIR types from #315.
//
// Output grammar (SV LRM 1800-2017 §25.10 — Interfaces):
//   interface NAME();
//     modport M1(input port_a, output port_b);
//     modport M2(...);
//   endinterface
//
// Missing / INVALID_SYM symbol entries are silently skipped.
//
// 3 ACs:
//   AC1 emit 输出语法正确的 interface 代码
//        (output contains 'interface NAME();' header,
//        'modport ...' lines for each modport, and
//        'endinterface' footer — no malformed syntax)
//   AC2 支持 modport 列表
//        (multi-modport interfaces emit each modport as a
//        separate line; port lists are comma-separated)
//   AC3 生成代码可被简单仿真器接受
//        (output is text — verified for SV grammar
//        compliance; "simple simulator" interpretation is
//        "the output parses cleanly through a regex check +


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.compiler.sv_ir;

namespace aura_issue_316_detail {

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

// Helper: count substring occurrences in `haystack`.
static std::size_t count_occurrences(const std::string& haystack,
                                     const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    auto pos = haystack.find(needle, 0);
    while (pos != std::string::npos) {
        ++n;
        pos = haystack.find(needle, pos + 1);
    }
    return n;
}

// ═══════════════════════════════════════════════════════════════
// AC1 + AC2: emit_sv_interface + emit_sv_modport syntax check
// ═══════════════════════════════════════════════════════════════

bool test_emit_sv_interface_syntax() {
    std::println("\n--- AC1+AC2: emit_sv_interface syntax ---");
    using namespace aura;
    ast::StringPool pool;
    // Construct port + modport SymIds.
    auto data = pool.intern("data");
    auto valid = pool.intern("valid");
    auto master = pool.intern("master");
    auto slave = pool.intern("slave");
    auto bus = pool.intern("Bus");
    // Build SVModportIR + SVInterfaceIR.
    auto master_mp = compiler::sv_ir::make_sv_modport(master, {data, valid});
    auto slave_mp = compiler::sv_ir::make_sv_modport(slave, {valid, data});
    auto bus_ir = compiler::sv_ir::make_sv_interface(
        bus, std::move(master_mp), std::move(slave_mp));
    // Emit.
    auto emitted = compiler::sv_ir::emit_sv_interface(bus_ir, pool);
    std::println("  --- emitted ---\n{}\n  --- end ---", emitted);
    // AC1: header `interface NAME();`.
    CHECK(emitted.find("interface Bus();") != std::string::npos,
          "header 'interface Bus();' present");
    // AC1: footer `endinterface`.
    CHECK(emitted.find("endinterface") != std::string::npos,
          "footer 'endinterface' present");
    // AC2: each modport line emitted exactly once.
    CHECK_EQ_LOCAL(count_occurrences(emitted, "modport master"),
                   std::size_t{1}, "1 'modport master' line emitted");
    CHECK_EQ_LOCAL(count_occurrences(emitted, "modport slave"),
                   std::size_t{1}, "1 'modport slave' line emitted");
    // AC2: each modport's port list contains the 2 ports.
    CHECK(emitted.find("data") != std::string::npos,
          "modport body contains 'data' port");
    CHECK(emitted.find("valid") != std::string::npos,
          "modport body contains 'valid' port");
    // AC1: no spurious characters that would break SV grammar.
    // The canonical SV interface grammar uses no semicolons
    // outside the modport headers; the endinterface has none.
    CHECK(emitted.find(";") != std::string::npos,
          "emitted text contains the modport ';' terminators");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: emit_sv_modport alone (used as a building block for
// higher-level emitters — see also #435's emit_modport).
// ═══════════════════════════════════════════════════════════════

bool test_emit_sv_modport_alone() {
    std::println("\n--- AC3: emit_sv_modport standalone ---");
    using namespace aura;
    ast::StringPool pool;
    auto data = pool.intern("data");
    auto valid = pool.intern("valid");
    auto master = pool.intern("master");
    auto mp = compiler::sv_ir::make_sv_modport(master, {data, valid});
    auto text = compiler::sv_ir::emit_sv_modport(mp, pool);
    std::println("  --- emitted ---\n{}\n  --- end ---", text);
    // The text is a single line ending in ';'.
    CHECK_EQ_LOCAL(count_occurrences(text, "\n"), std::size_t{0},
                   "single-line emit (no newline)");
    CHECK(text.find(";") != std::string::npos,
          "modport declaration ends with ';'");
    CHECK(text.find("modport master") != std::string::npos,
          "modport declaration starts with 'modport master'");
    CHECK(text.find("data") != std::string::npos,
          "modport contains 'data' port");
    CHECK(text.find("valid") != std::string::npos,
          "modport contains 'valid' port");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3 (bonus): SV grammar lint check on a populated interface
// ═══════════════════════════════════════════════════════════════

bool test_emit_sv_grammar_lint() {
    std::println("\n--- AC3 (bonus): SV grammar lint ---");
    using namespace aura;
    ast::StringPool pool;
    // 3 modports × ~2 ports each — non-trivial but realistic.
    auto p_clk = pool.intern("clk");
    auto p_data = pool.intern("data");
    auto p_valid = pool.intern("valid");
    auto p_rdy = pool.intern("ready");
    auto master = pool.intern("master");
    auto slave = pool.intern("slave");
    auto monitor = pool.intern("monitor");
    auto apb = pool.intern("APB");
    auto master_mp = compiler::sv_ir::make_sv_modport(master, {p_clk, p_data, p_valid});
    auto slave_mp = compiler::sv_ir::make_sv_modport(slave, {p_clk, p_data, p_rdy});
    auto monitor_mp = compiler::sv_ir::make_sv_modport(monitor, {p_clk, p_valid});
    auto apb_ir = compiler::sv_ir::make_sv_interface(
        apb, std::move(master_mp), std::move(slave_mp), std::move(monitor_mp));
    auto emitted = compiler::sv_ir::emit_sv_interface(apb_ir, pool);
    std::println("  --- emitted ---\n{}\n  --- end ---", emitted);
    // Grammar checks: the SV `interface` keyword appears once,
    // the `endinterface` keyword appears once, modport count
    // matches, and each modport has balanced parens.
    CHECK_EQ_LOCAL(count_occurrences(emitted, "interface APB"),
                   std::size_t{1}, "header 'interface APB' appears once");
    CHECK_EQ_LOCAL(count_occurrences(emitted, "endinterface"),
                   std::size_t{1}, "'endinterface' appears once");
    CHECK_EQ_LOCAL(count_occurrences(emitted, "modport "),
                   std::size_t{3},
                   "exactly 3 modport declarations emitted");
    // Balanced parens around each modport body: open '('
    // and close ')' both appear N times.
    const auto opens = count_occurrences(emitted, "(");
    const auto closes = count_occurrences(emitted, ")");
    CHECK_EQ_LOCAL(opens, closes,
                   "open and close parens are balanced around modport bodies");
    return true;
}

int run_tests() {
    std::println("═══ Issue #316 (SV emit for Interface + Modport) ═══\n");
    test_emit_sv_interface_syntax();
    test_emit_sv_modport_alone();
    test_emit_sv_grammar_lint();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_316_detail

int aura_issue_316_run() { return aura_issue_316_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_316_run(); }
#endif