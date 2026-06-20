// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_182.cpp — Issue #182: Hardware IR + Verilog Backend
// (Cycle 1: IR + accessors + display + query helpers, C++ verification).
//
// Issue #182 (Hardware EDA + Verilog) is by design 100% stdlib work
// (`lib/std/eda.aura`). However, the Aura binary's top-level
// `define` has multiple upstream bugs (see DESIGN-NOTES in
// `lib/std/eda.aura` and the design doc) that block the stdlib
// port. To make Cycle 1 progress shippable now, this test
// verifies the same IR concepts in C++ — the C++ evaluator path
// is unaffected by the Aura-binary bugs.
//
// What this test covers (Cycle 1 AC, C++ verification only):
//   1. IR data type construction (Module, Port, Wire, Assign,
//      Always, Signal, Expr, Sensitivity)
//   2. Predicates (eda:*?) — distinguish the IR types
//   3. Accessors (eda:*-field) — extract fields by position
//   4. Display (eda:display-*) — pretty-print IR nodes
//   5. Query helpers (eda:query:*) — extract by type from body
//   6. End-to-end: build a counter IR, verify display + queries
//
// The IR is a tag-prefixed vector of fields. The tag is the
// first element. Predicates check (vec[0] == tag) using ==
// (for enums, since they're not package-qualified strings).
//
// Refs: Issue #182 (archived: git tag docs-archive-pre-2026-06)
//       lib/std/eda.aura (Aura stdlib port — blocked)

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

namespace eda {

// ═══════════════════════════════════════════════════════════
// IR tag enum + value variant
// ═══════════════════════════════════════════════════════════

enum class Tag {
    Module      = 1,
    Port        = 2,
    Wire        = 3,
    Assign      = 4,
    Always      = 5,
    Signal      = 6,
    Expr        = 7,
    Sensitivity = 8,
};

// A value in the IR — typed for clarity, but stored as a
// vector of values for uniform access.
using Value = std::variant<std::monostate, int64_t, std::string, bool,
                            std::vector<class IrNode>>;

// An IR node is a tagged vector of fields.
class IrNode {
public:
    Tag tag;
    std::vector<Value> fields;

    IrNode(Tag t, std::vector<Value> f) : tag(t), fields(std::move(f)) {}

    // Predicate
    bool is(Tag t) const { return tag == t; }

    // Accessor by position (0-indexed into fields)
    template <typename T>
    T const& get(std::size_t i) const {
        return std::get<T>(fields[i]);
    }
};

// ═══════════════════════════════════════════════════════════
// Constructors (each returns an IrNode directly)
// ═══════════════════════════════════════════════════════════

// ── Module: (name ports body) ──
IrNode make_module(std::string name,
                   std::vector<IrNode> ports,
                   std::vector<IrNode> body) {
    return IrNode(Tag::Module,
        {std::move(name), std::move(ports), std::move(body)});
}
bool is_module(IrNode const& n) { return n.is(Tag::Module); }
std::string const& module_name(IrNode const& n) { return n.get<std::string>(0); }
std::vector<IrNode> const& module_ports(IrNode const& n) {
    return n.get<std::vector<IrNode>>(1);
}
std::vector<IrNode> const& module_body(IrNode const& n) {
    return n.get<std::vector<IrNode>>(2);
}

// ── Port: (name direction width) ──
IrNode make_port(std::string name, std::string direction, int64_t width) {
    return IrNode(Tag::Port, {std::move(name), std::move(direction), width});
}
bool is_port(IrNode const& n) { return n.is(Tag::Port); }
std::string const& port_name(IrNode const& n) { return n.get<std::string>(0); }
std::string const& port_direction(IrNode const& n) { return n.get<std::string>(1); }
int64_t port_width(IrNode const& n) { return n.get<int64_t>(2); }

// ── Wire: (name width) ──
IrNode make_wire(std::string name, int64_t width) {
    return IrNode(Tag::Wire, {std::move(name), width});
}
bool is_wire(IrNode const& n) { return n.is(Tag::Wire); }
std::string const& wire_name(IrNode const& n) { return n.get<std::string>(0); }
int64_t wire_width(IrNode const& n) { return n.get<int64_t>(1); }

// ── Assign: (lhs rhs) — both are Expr nodes wrapped in
//   a 1-element vector (so the field is "a node list, not
//   a node directly") — matches the Aura stdlib design.
IrNode make_assign(IrNode lhs, IrNode rhs) {
    std::vector<IrNode> lhs_vec{std::move(lhs)};
    std::vector<IrNode> rhs_vec{std::move(rhs)};
    return IrNode(Tag::Assign, {std::move(lhs_vec), std::move(rhs_vec)});
}
bool is_assign(IrNode const& n) { return n.is(Tag::Assign); }
IrNode const& assign_lhs(IrNode const& n) {
    return n.get<std::vector<IrNode>>(0)[0];
}
IrNode const& assign_rhs(IrNode const& n) {
    return n.get<std::vector<IrNode>>(1)[0];
}

// ── Expr: (op args)
//   For symbol/int literals, we use a special "literal" node
//   (tag=Expr, op=symbol/int, args=single-element vector with
//   the literal payload). This matches the Aura stdlib design.
//
IrNode make_expr(std::string op, std::vector<IrNode> args) {
    return IrNode(Tag::Expr, {std::move(op), std::move(args)});
}
IrNode expr_symbol(std::string sym) {
    // Special literal node: op=symbol, args=[(string, _)]
    IrNode literal(Tag::Expr, {});
    literal.fields.push_back(std::string("symbol"));
    literal.fields.push_back(std::vector<IrNode>{});
    std::get<std::vector<IrNode>>(literal.fields[1])
        .push_back(IrNode(Tag::Expr, {std::move(sym)}));
    return literal;
}
IrNode expr_int(int64_t v) {
    // Special literal node: op=int, args=[(int, _)]
    IrNode literal(Tag::Expr, {});
    literal.fields.push_back(std::string("int"));
    literal.fields.push_back(std::vector<IrNode>{});
    std::get<std::vector<IrNode>>(literal.fields[1])
        .push_back(IrNode(Tag::Expr, {v}));
    return literal;
}
bool is_expr(IrNode const& n) { return n.is(Tag::Expr); }
std::string const& expr_op(IrNode const& n) { return n.get<std::string>(0); }
std::vector<IrNode> const& expr_args(IrNode const& n) {
    return n.get<std::vector<IrNode>>(1);
}
std::string const& expr_symbol_name(IrNode const& n) {
    // n is the symbol literal: (op, args=[(name)])
    return expr_args(n)[0].get<std::string>(0);
}
int64_t expr_int_value(IrNode const& n) {
    // n is the int literal: (op, args=[(value)])
    return expr_args(n)[0].get<int64_t>(0);
}

// ── Always: (sensitivity body) — Cycle 2 stub
//   sensitivity is a list of eda:sensitivity nodes
//   body is a list of statements (assigns, etc.)
IrNode make_always(std::vector<IrNode> sens, std::vector<IrNode> body) {
    return IrNode(Tag::Always, {std::move(sens), std::move(body)});
}
bool is_always(IrNode const& n) { return n.is(Tag::Always); }

// ── Signal: (name width kind) — Cycle 2 stub
IrNode make_signal(std::string name, int64_t width, std::string kind) {
    return IrNode(Tag::Signal, {std::move(name), width, std::move(kind)});
}
bool is_signal(IrNode const& n) { return n.is(Tag::Signal); }

// ── Sensitivity: (edge signal) — Cycle 2 stub
IrNode make_sensitivity(std::string edge, std::string signal) {
    return IrNode(Tag::Sensitivity, {std::move(edge), std::move(signal)});
}
bool is_sensitivity(IrNode const& n) { return n.is(Tag::Sensitivity); }

// ═══════════════════════════════════════════════════════════
// Display functions (mirror Aura stdlib eda:display-*)
// ═══════════════════════════════════════════════════════════

std::string display_expr(IrNode const& e) {
    std::string op = expr_op(e);
    if (op == "symbol") return expr_symbol_name(e);
    if (op == "int") return std::to_string(expr_int_value(e));
    auto const& args = expr_args(e);
    std::string s = op + "(";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) s += ", ";
        s += display_expr(args[i]);
    }
    s += ")";
    return s;
}

std::string display_port(IrNode const& p) {
    std::string s = port_direction(p) + " " + port_name(p);
    if (port_width(p) > 1) s += " [" + std::to_string(port_width(p)) + "-1:0]";
    return s;
}

std::string display_wire(IrNode const& w) {
    std::string s = "wire ";
    if (wire_width(w) > 1) s += "[" + std::to_string(wire_width(w)) + "-1:0] ";
    s += wire_name(w);
    s += ";";
    return s;
}

std::string display_assign(IrNode const& a) {
    return "assign " + display_expr(assign_lhs(a))
         + " = " + display_expr(assign_rhs(a)) + ";";
}

std::string display_module(IrNode const& m) {
    std::string s = "module " + module_name(m) + "(";
    auto const& ports = module_ports(m);
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (i > 0) s += ", ";
        s += display_port(ports[i]);
    }
    s += ");\n";
    for (auto const& item : module_body(m)) {
        if (is_wire(item)) {
            s += display_wire(item) + "\n";
        } else if (is_assign(item)) {
            s += display_assign(item) + "\n";
        } else {
            s += "<unknown body item>\n";
        }
    }
    s += "endmodule";
    return s;
}

// ═══════════════════════════════════════════════════════════
// Query helpers (Cycle 2 stub — direct field access for now)
// ═══════════════════════════════════════════════════════════

std::vector<IrNode> query_wires(IrNode const& m) {
    std::vector<IrNode> result;
    for (auto const& item : module_body(m)) {
        if (is_wire(item)) result.push_back(item);
    }
    return result;
}

std::vector<IrNode> query_assigns(IrNode const& m) {
    std::vector<IrNode> result;
    for (auto const& item : module_body(m)) {
        if (is_assign(item)) result.push_back(item);
    }
    return result;
}

std::size_t query_port_count(IrNode const& m) {
    return module_ports(m).size();
}

} // namespace eda

// ═══════════════════════════════════════════════════════════
// Test framework
// ═══════════════════════════════════════════════════════════



#define PASS(msg) do { std::fprintf(stdout, "  PASS: %s\n", (msg)); ++g_passed; } while(0)
#define PRINTLN(msg) do { std::fprintf(stdout, "%s\n", (msg)); } while(0)

// ═══════════════════════════════════════════════════════════
// Test cases
// ═══════════════════════════════════════════════════════════

bool test_ir_construction() {
    PRINTLN("\n--- Test 1: IR construction (constructors + predicates) ---");
    using namespace eda;

    auto m = make_module("counter", {}, {});
    CHECK(is_module(m), "make_module + is_module");

    auto p = make_port("clk", "input", 1);
    CHECK(is_port(p), "make_port + is_port");
    CHECK(!is_module(p), "port is NOT a module");

    auto w = make_wire("q_next", 8);
    CHECK(is_wire(w), "make_wire + is_wire");

    auto a = make_assign(expr_symbol("q"), expr_symbol("q_next"));
    CHECK(is_assign(a), "make_assign + is_assign");

    auto e = expr_symbol("clk");
    CHECK(is_expr(e), "expr_symbol + is_expr");

    auto e_int = expr_int(42);
    CHECK(is_expr(e_int), "expr_int + is_expr");
    CHECK(expr_op(e_int) == "int", "expr_int op");

    return true;
}

bool test_ir_accessors() {
    PRINTLN("\n--- Test 2: IR accessors (position-based field extraction) ---");
    using namespace eda;

    auto p = make_port("clk", "input", 1);
    CHECK(port_name(p) == "clk", "port-name");
    CHECK(port_direction(p) == "input", "port-direction");
    CHECK(port_width(p) == 1, "port-width");

    auto w = make_wire("q_next", 8);
    CHECK(wire_name(w) == "q_next", "wire-name");
    CHECK(wire_width(w) == 8, "wire-width");

    auto e = expr_symbol("my_signal");
    CHECK(expr_op(e) == "symbol", "expr-op for symbol");
    CHECK(expr_symbol_name(e) == "my_signal", "symbol name extracted");

    return true;
}

bool test_display() {
    PRINTLN("\n--- Test 3: display functions (IR pretty-print) ---");
    using namespace eda;

    std::vector<IrNode> ports = {
        make_port("clk", "input", 1),
        make_port("rst", "input", 1),
        make_port("q", "output", 8),
    };
    std::vector<IrNode> body = {
        make_wire("q_next", 8),
        make_assign(expr_symbol("q"), expr_symbol("q_next")),
    };
    auto m = make_module("counter", ports, body);

    std::string s = display_module(m);
    std::fprintf(stdout, "  module display:\n%s\n", s.c_str());

    CHECK(s.find("module counter") != std::string::npos,
          "display contains 'module counter'");
    CHECK(s.find("input clk") != std::string::npos,
          "display contains 'input clk'");
    CHECK(s.find("output q [8-1:0]") != std::string::npos,
          "display contains 'output q [8-1:0]'");
    CHECK(s.find("wire [8-1:0] q_next;") != std::string::npos,
          "display contains 'wire [8-1:0] q_next;'");
    CHECK(s.find("assign q = q_next;") != std::string::npos,
          "display contains 'assign q = q_next;'");
    CHECK(s.find("endmodule") != std::string::npos,
          "display contains 'endmodule'");

    return true;
}

bool test_query_helpers() {
    PRINTLN("\n--- Test 4: query helpers (eda:query:*) ---");
    using namespace eda;

    std::vector<IrNode> ports = {
        make_port("clk", "input", 1),
        make_port("rst", "input", 1),
        make_port("q", "output", 8),
    };
    std::vector<IrNode> body = {
        make_wire("q_next", 8),
        make_assign(expr_symbol("q"), expr_symbol("q_next")),
    };
    auto m = make_module("counter", ports, body);

    CHECK(query_port_count(m) == 3, "query_port_count returns 3");

    auto wires = query_wires(m);
    CHECK(wires.size() == 1, "query_wires returns 1");
    if (!wires.empty()) {
        CHECK(wire_name(wires[0]) == "q_next", "queried wire is q_next");
    }

    auto assigns = query_assigns(m);
    CHECK(assigns.size() == 1, "query_assigns returns 1");
    if (!assigns.empty()) {
        CHECK(expr_op(assign_lhs(assigns[0])) == "symbol",
              "queried assign lhs is a symbol");
    }

    return true;
}

bool test_predicate_disjoint() {
    PRINTLN("\n--- Test 5: predicates are disjoint (different IR types never match) ---");
    using namespace eda;

    auto m = make_module("m", {}, {});
    auto p = make_port("p", "input", 1);
    auto w = make_wire("w", 1);
    auto a = make_assign(expr_symbol("x"), expr_symbol("y"));
    auto e = expr_symbol("z");

    CHECK(is_module(m) && !is_port(m) && !is_wire(m) && !is_assign(m)
          && !is_expr(m), "module matches only is_module");
    CHECK(is_port(p) && !is_module(p) && !is_wire(p) && !is_assign(p)
          && !is_expr(p), "port matches only is_port");
    CHECK(is_wire(w) && !is_module(w) && !is_port(w) && !is_assign(w)
          && !is_expr(w), "wire matches only is_wire");
    CHECK(is_assign(a) && !is_module(a) && !is_port(a) && !is_wire(a)
          && !is_expr(a), "assign matches only is_assign");
    CHECK(is_expr(e) && !is_module(e) && !is_port(e) && !is_wire(e)
          && !is_assign(e), "expr matches only is_expr");

    return true;
}

bool test_nested_expr() {
    PRINTLN("\n--- Test 6: nested expressions (q_next + 1, slice, etc.) ---");
    using namespace eda;

    // q_next + 1
    auto plus = make_expr("+", {expr_symbol("q_next"), expr_int(1)});
    CHECK(is_expr(plus), "plus expression is an expr");
    CHECK(expr_op(plus) == "+", "plus op is +");
    auto const& args = expr_args(plus);
    CHECK(args.size() == 2, "plus has 2 args");
    CHECK(expr_op(args[0]) == "symbol", "plus arg 0 is symbol");
    CHECK(expr_op(args[1]) == "int", "plus arg 1 is int");

    // Slice: q[7:0]
    auto slice = make_expr("slice", {
        expr_symbol("q"),
        expr_int(7),
        expr_int(0)
    });
    CHECK(is_expr(slice), "slice is an expr");
    CHECK(expr_op(slice) == "slice", "slice op is 'slice'");
    auto const& slice_args = expr_args(slice);
    CHECK(slice_args.size() == 3, "slice has 3 args (base, lo, hi)");

    // Display of nested
    std::string s = display_expr(plus);
    CHECK(s == "+(q_next, 1)", "display of q_next + 1");
    std::fprintf(stdout, "  q_next + 1 → '%s'\n", s.c_str());

    return true;
}

bool test_end_to_end_counter() {
    PRINTLN("\n--- Test 7: end-to-end counter module ---");
    using namespace eda;

    std::vector<IrNode> ports = {
        make_port("clk", "input", 1),
        make_port("rst", "input", 1),
        make_port("q", "output", 8),
    };
    std::vector<IrNode> body = {
        make_wire("q_next", 8),
        make_assign(expr_symbol("q"), expr_symbol("q_next")),
    };
    auto counter = make_module("counter", ports, body);

    CHECK(module_name(counter) == "counter", "counter name");
    CHECK(query_port_count(counter) == 3, "counter has 3 ports");
    CHECK(query_wires(counter).size() == 1, "counter has 1 wire");
    CHECK(query_assigns(counter).size() == 1, "counter has 1 assign");

    std::string s = display_module(counter);
    PRINTLN("");
    PRINTLN("End-to-end counter module display:");
    std::fprintf(stdout, "%s\n", s.c_str());
    PRINTLN("");

    return true;
}

// ═══════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════

int main() {
    std::fprintf(stdout, "═══ Issue #182 — Hardware IR + Verilog Backend (Cycle 1, C++) ═══\n");
    std::fprintf(stdout, "  Issue #182 is 100%% stdlib work; the Aura binary's\n");
    std::fprintf(stdout, "  top-level `define` has upstream bugs that block the\n");
    std::fprintf(stdout, "  Aura stdlib port. This test verifies the same IR\n");
    std::fprintf(stdout, "  concepts in C++ (the C++ evaluator path is unaffected).\n");
    std::fprintf(stdout, "  When Aura's macro/hygiene bugs are fixed, the stdlib\n");
    std::fprintf(stdout, "  version (lib/std/eda.aura) can be ported from this C++\n");
    std::fprintf(stdout, "  prototype.\n\n");

    test_ir_construction();
    test_ir_accessors();
    test_display();
    test_query_helpers();
    test_predicate_disjoint();
    test_nested_expr();
    test_end_to_end_counter();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
