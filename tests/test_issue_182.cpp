// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_182.cpp — Issue #182: Hardware IR + Verilog Backend
// (Cycle 1-4: IR + accessors + display + query/mutate helpers +
// Verilog emitter + type-checked pipeline, C++ verification).
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
#include <optional>
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

namespace aura_issue_182_detail {
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

std::string emit_expr(IrNode const& e);

std::string emit_infix(std::string_view op_str,
                       std::vector<IrNode> const& args) {
    if (args.empty()) return "";
    if (args.size() == 1) return emit_expr(args[0]);
    return emit_expr(args[0]) + " " + std::string(op_str) + " "
         + emit_infix(op_str, {args.begin() + 1, args.end()});
}

std::string emit_expr(IrNode const& e) {
    std::string op = expr_op(e);
    if (op == "symbol") return expr_symbol_name(e);
    if (op == "int") return std::to_string(expr_int_value(e));
    if (op == "slice") {
        auto const& a = expr_args(e);
        return emit_expr(a[0]) + "[" + std::to_string(expr_int_value(a[1]))
             + ":" + std::to_string(expr_int_value(a[2])) + "]";
    }
    if (op == "+") return "(" + emit_infix("+", expr_args(e)) + ")";
    if (op == "-") return "(" + emit_infix("-", expr_args(e)) + ")";
    if (op == "&") return "(" + emit_infix("&", expr_args(e)) + ")";
    return op + "(...)";
}

std::string emit_port(IrNode const& p) {
    std::string s = port_direction(p) + " " + port_name(p);
    if (port_width(p) > 1) s += " [" + std::to_string(port_width(p)) + "-1:0]";
    return s;
}

std::string emit_wire(IrNode const& w) {
    std::string s = "wire ";
    if (wire_width(w) > 1) s += "[" + std::to_string(wire_width(w)) + "-1:0] ";
    s += wire_name(w);
    s += ";";
    return s;
}

std::string emit_assign(IrNode const& a) {
    return "assign " + emit_expr(assign_lhs(a))
         + " = " + emit_expr(assign_rhs(a)) + ";";
}

std::string emit_sensitivity(IrNode const& s) {
    return std::get<std::string>(s.fields[0]) + " "
         + std::get<std::string>(s.fields[1]);
}

std::string emit_always_body_stmt(IrNode const& item) {
    if (is_assign(item)) return emit_assign(item);
    if (is_wire(item)) return emit_wire(item);
    return "";
}

std::string emit_always_body(std::vector<IrNode> const& body) {
    if (body.empty()) return "end\n";
    if (body.size() == 1) return emit_always_body_stmt(body[0]) + "end\n";
    return emit_always_body_stmt(body[0]) + "\n" + emit_always_body(
        {body.begin() + 1, body.end()});
}

std::string emit_always(IrNode const& a) {
    std::string s = "always @(";
    auto const& sens = std::get<std::vector<IrNode>>(a.fields[0]);
    for (std::size_t i = 0; i < sens.size(); ++i) {
        if (i > 0) s += ", ";
        s += emit_sensitivity(sens[i]);
    }
    s += ") begin\n";
    s += emit_always_body(std::get<std::vector<IrNode>>(a.fields[1]));
    return s;
}

std::string emit_body_item(IrNode const& item) {
    if (is_wire(item)) return emit_wire(item) + "\n";
    if (is_assign(item)) return emit_assign(item) + "\n";
    if (is_always(item)) return emit_always(item);
    if (is_signal(item)) {
        std::string s = item.get<std::string>(2) + " ";
        if (item.get<int64_t>(1) > 1)
            s += "[" + std::to_string(item.get<int64_t>(1)) + "-1:0] ";
        s += item.get<std::string>(0) + ";\n";
        return s;
    }
    return "";
}

std::string emit_module_body(std::vector<IrNode> const& body) {
    if (body.empty()) return "";
    if (body.size() == 1) return emit_body_item(body[0]);
    return emit_body_item(body[0]) + emit_module_body(
        {body.begin() + 1, body.end()});
}

std::string emit_verilog(IrNode const& ir) {
    if (is_module(ir)) {
        std::string s = "module " + module_name(ir) + "(";
        auto const& ports = module_ports(ir);
        for (std::size_t i = 0; i < ports.size(); ++i) {
            if (i > 0) s += ", ";
            s += emit_port(ports[i]);
        }
        s += ");\n";
        s += emit_module_body(module_body(ir));
        s += "endmodule";
        return s;
    }
    return emit_body_item(ir);
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

std::vector<std::string> collect_symbols(IrNode const& e) {
    if (expr_op(e) == "symbol") return {expr_symbol_name(e)};
    std::vector<std::string> result;
    for (auto const& arg : expr_args(e)) {
        auto sub = collect_symbols(arg);
        result.insert(result.end(), sub.begin(), sub.end());
    }
    return result;
}

bool expr_mentions(IrNode const& e, std::string_view sym) {
    for (auto const& s : collect_symbols(e)) {
        if (s == sym) return true;
    }
    return false;
}

int64_t query_bit_width(std::string_view name, IrNode const& m) {
    for (auto const& p : module_ports(m)) {
        if (port_name(p) == name) return port_width(p);
    }
    for (auto const& w : query_wires(m)) {
        if (wire_name(w) == name) return wire_width(w);
    }
    return 0;
}

std::vector<std::string> query_dependencies(std::string_view sig, IrNode const& m) {
    std::vector<std::string> result;
    for (auto const& a : query_assigns(m)) {
        if (expr_mentions(assign_lhs(a), sig)) {
            auto deps = collect_symbols(assign_rhs(a));
            result.insert(result.end(), deps.begin(), deps.end());
        }
    }
    return result;
}

std::vector<std::string> query_dependents(std::string_view sig, IrNode const& m) {
    std::vector<std::string> result;
    for (auto const& a : query_assigns(m)) {
        if (expr_mentions(assign_rhs(a), sig)) {
            result.push_back(expr_symbol_name(assign_lhs(a)));
        }
    }
    return result;
}

IrNode expr_rename_sym(IrNode e, std::string_view old_name, std::string_view new_name) {
    if (expr_op(e) == "symbol") {
        if (expr_symbol_name(e) == old_name)
            return expr_symbol(std::string(new_name));
        return e;
    }
    if (expr_op(e) == "int") return e;
    std::vector<IrNode> new_args;
    for (auto const& arg : expr_args(e))
        new_args.push_back(expr_rename_sym(arg, old_name, new_name));
    return make_expr(expr_op(e), std::move(new_args));
}

IrNode port_rename_sym(IrNode p, std::string_view old_name, std::string_view new_name) {
    if (port_name(p) == old_name)
        return make_port(std::string(new_name), port_direction(p), port_width(p));
    return p;
}

IrNode wire_rename_sym(IrNode w, std::string_view old_name, std::string_view new_name) {
    if (wire_name(w) == old_name)
        return make_wire(std::string(new_name), wire_width(w));
    return w;
}

IrNode assign_rename_sym(IrNode a, std::string_view old_name, std::string_view new_name) {
    return make_assign(
        expr_rename_sym(assign_lhs(a), old_name, new_name),
        expr_rename_sym(assign_rhs(a), old_name, new_name));
}

IrNode body_item_rename_sym(IrNode item, std::string_view old_name,
                            std::string_view new_name) {
    if (is_wire(item)) return wire_rename_sym(item, old_name, new_name);
    if (is_assign(item)) return assign_rename_sym(item, old_name, new_name);
    return item;
}

IrNode mutate_rename_symbol(std::string_view old_name, std::string_view new_name,
                            IrNode m) {
    std::vector<IrNode> ports;
    for (auto const& p : module_ports(m))
        ports.push_back(port_rename_sym(p, old_name, new_name));
    std::vector<IrNode> body;
    for (auto const& item : module_body(m))
        body.push_back(body_item_rename_sym(item, old_name, new_name));
    return make_module(module_name(m), std::move(ports), std::move(body));
}

IrNode mutate_insert_clock_gating(IrNode m, std::string_view clk,
                                  std::string_view en) {
    std::vector<IrNode> body = module_body(m);
    body.insert(body.begin(), make_wire("clk_gated", 1));
    body.insert(body.begin() + 1,
                make_assign(
                    expr_symbol("clk_gated"),
                    make_expr("&", {expr_symbol(std::string(clk)),
                                    expr_symbol(std::string(en))})));
    return make_module(module_name(m), module_ports(m), std::move(body));
}

bool expr_equal(IrNode const& e1, IrNode const& e2) {
    if (expr_op(e1) != expr_op(e2)) return false;
    if (expr_op(e1) == "symbol") return expr_symbol_name(e1) == expr_symbol_name(e2);
    if (expr_op(e1) == "int") return expr_int_value(e1) == expr_int_value(e2);
    auto const& a1 = expr_args(e1);
    auto const& a2 = expr_args(e2);
    if (a1.size() != a2.size()) return false;
    for (std::size_t i = 0; i < a1.size(); ++i) {
        if (!expr_equal(a1[i], a2[i])) return false;
    }
    return true;
}

IrNode mutate_extract_common_expr(IrNode m) {
    auto assigns = query_assigns(m);
    if (assigns.size() < 2) return m;
    for (std::size_t i = 0; i + 1 < assigns.size(); ++i) {
        for (std::size_t j = i + 1; j < assigns.size(); ++j) {
            if (!expr_equal(assign_rhs(assigns[i]), assign_rhs(assigns[j])))
                continue;
            auto const& common_rhs = assign_rhs(assigns[i]);
            std::vector<IrNode> body;
            body.push_back(make_wire("common_expr", 8));
            body.push_back(make_assign(expr_symbol("common_expr"), common_rhs));
            for (auto const& item : module_body(m)) {
                if (is_assign(item) && expr_equal(assign_rhs(item), common_rhs)) {
                    body.push_back(make_assign(assign_lhs(item), expr_symbol("common_expr")));
                } else {
                    body.push_back(item);
                }
            }
            return make_module(module_name(m), module_ports(m), std::move(body));
        }
    }
    return m;
}

// ═══════════════════════════════════════════════════════════
// Cycle 4: dependent types + type-checked mutate (C++ mirror)
// ═══════════════════════════════════════════════════════════

IrNode port_to_typed_signal(IrNode const& p) {
    return IrNode(Tag::Expr,
        {std::string("typed-signal"), port_name(p), port_width(p)});
}

IrNode wire_to_typed_signal(IrNode const& w) {
    return IrNode(Tag::Expr,
        {std::string("typed-signal"), wire_name(w), wire_width(w)});
}

std::vector<IrNode> query_typed_signals(IrNode const& m) {
    std::vector<IrNode> result;
    for (auto const& p : module_ports(m)) result.push_back(port_to_typed_signal(p));
    for (auto const& w : query_wires(m)) result.push_back(wire_to_typed_signal(w));
    return result;
}

bool assign_widths_ok(IrNode const& a, IrNode const& m) {
    auto const& lhs = assign_lhs(a);
    auto const& rhs = assign_rhs(a);
    if (expr_op(lhs) != "symbol" || expr_op(rhs) != "symbol") return true;
    return query_bit_width(expr_symbol_name(lhs), m)
        == query_bit_width(expr_symbol_name(rhs), m);
}

bool check_module_types(IrNode const& m) {
    for (auto const& a : query_assigns(m)) {
        if (!assign_widths_ok(a, m)) return false;
    }
    return true;
}

std::optional<IrNode> mutate_insert_clock_gating_checked(
    IrNode m, std::string_view clk, std::string_view en) {
    if (query_bit_width(clk, m) != 1 || query_bit_width(en, m) != 1)
        return std::nullopt;
    return mutate_insert_clock_gating(std::move(m), clk, en);
}

std::optional<std::string> pipeline_check_and_emit(IrNode const& m) {
    if (!check_module_types(m)) return std::nullopt;
    return emit_verilog(m);
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

bool test_cycle2_dependency_queries() {
    PRINTLN("\n--- Test 8: Cycle 2 query (dependencies / dependents / bit-width) ---");
    using namespace eda;

    std::vector<IrNode> ports = {
        make_port("clk", "input", 1),
        make_port("q", "output", 8),
    };
    std::vector<IrNode> body = {
        make_wire("q_next", 8),
        make_assign(expr_symbol("q"), expr_symbol("q_next")),
    };
    auto m = make_module("counter", ports, body);

    CHECK(query_bit_width("q", m) == 8, "bit-width of port q is 8");
    CHECK(query_bit_width("q_next", m) == 8, "bit-width of wire q_next is 8");

    auto deps = query_dependencies("q", m);
    CHECK(deps.size() == 1, "q has 1 dependency");
    if (!deps.empty()) CHECK(deps[0] == "q_next", "q depends on q_next");

    auto dents = query_dependents("q_next", m);
    CHECK(dents.size() == 1, "q_next has 1 dependent");
    if (!dents.empty()) CHECK(dents[0] == "q", "q_next drives q");

    return true;
}

bool test_cycle2_mutate_rename() {
    PRINTLN("\n--- Test 9: Cycle 2 mutate:rename-symbol ---");
    using namespace eda;

    std::vector<IrNode> body = {
        make_wire("q_next", 8),
        make_assign(expr_symbol("q"), expr_symbol("q_next")),
    };
    auto m = make_module("m", {make_port("q", "output", 8)}, body);
    auto renamed = mutate_rename_symbol("q_next", "next_val", m);

    CHECK(query_wires(renamed).size() == 1, "still 1 wire after rename");
    if (!query_wires(renamed).empty())
        CHECK(wire_name(query_wires(renamed)[0]) == "next_val", "wire renamed");
    auto assigns = query_assigns(renamed);
    CHECK(!assigns.empty(), "assign preserved");
    if (!assigns.empty())
        CHECK(expr_symbol_name(assign_rhs(assigns[0])) == "next_val",
              "assign rhs references renamed symbol");
    return true;
}

bool test_cycle2_mutate_clock_gating() {
    PRINTLN("\n--- Test 10: Cycle 2 mutate:insert-clock-gating ---");
    using namespace eda;

    auto m = make_module("gated", {make_port("clk", "input", 1)}, {});
    auto gated = mutate_insert_clock_gating(m, "clk", "en");

    CHECK(query_wires(gated).size() == 1, "inserted gated wire");
    CHECK(query_assigns(gated).size() == 1, "inserted gate assign");
    auto gated_assigns = query_assigns(gated);
    if (!gated_assigns.empty()) {
        auto const& a = gated_assigns[0];
        CHECK(expr_op(assign_rhs(a)) == "&", "gate uses AND");
        CHECK(expr_args(assign_rhs(a)).size() == 2, "AND has 2 operands");
    }
    return true;
}

bool test_cycle2_mutate_extract_common() {
    PRINTLN("\n--- Test 11: Cycle 2 mutate:extract-common-expr ---");
    using namespace eda;

    auto common = make_expr("+", {expr_symbol("a"), expr_symbol("b")});
    std::vector<IrNode> body = {
        make_assign(expr_symbol("x"), common),
        make_assign(expr_symbol("y"), common),
    };
    auto m = make_module("dup", {}, body);
    auto extracted = mutate_extract_common_expr(m);

    CHECK(query_wires(extracted).size() == 1, "common wire extracted");
    CHECK(query_assigns(extracted).size() == 3, "common assign + 2 refs");
    if (!query_wires(extracted).empty())
        CHECK(wire_name(query_wires(extracted)[0]) == "common_expr",
              "common wire name");
    return true;
}

bool test_cycle3_emit_verilog() {
    PRINTLN("\n--- Test 12: Cycle 3 emit-verilog (module + assigns) ---");
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

    std::string s = emit_verilog(m);
    std::fprintf(stdout, "  emit-verilog:\n%s\n", s.c_str());

    CHECK(s.find("module counter") != std::string::npos,
          "emit contains module header");
    CHECK(s.find("wire [8-1:0] q_next;") != std::string::npos,
          "emit contains wire decl");
    CHECK(s.find("assign q = q_next;") != std::string::npos,
          "emit contains assign");
    CHECK(s.find("endmodule") != std::string::npos,
          "emit contains endmodule");
    CHECK(s.find("eda:module") == std::string::npos,
          "emit does not leak IR tags");
    return true;
}

bool test_cycle3_emit_always() {
    PRINTLN("\n--- Test 13: Cycle 3 emit-always (posedge + infix expr) ---");
    using namespace eda;

    auto inc = make_expr("+", {expr_symbol("q"), expr_int(1)});
    std::vector<IrNode> body = {
        make_assign(expr_symbol("q"), inc),
    };
    auto always_blk = make_always(
        {make_sensitivity("posedge", "clk")},
        body);
    std::vector<IrNode> mod_body = {always_blk};
    auto m = make_module("tick", {make_port("clk", "input", 1)}, mod_body);

    std::string s = emit_verilog(m);
    std::fprintf(stdout, "  always emit:\n%s\n", s.c_str());

    CHECK(s.find("always @(posedge clk)") != std::string::npos,
          "emit contains sensitivity list");
    CHECK(s.find("assign q = (q + 1);") != std::string::npos,
          "emit contains infix assignment");
    CHECK(s.find("end\nendmodule") != std::string::npos,
          "emit closes always block and module");
    return true;
}

bool test_cycle4_typed_signals() {
    PRINTLN("\n--- Test 14: Cycle 4 typed-signal query ---");
    using namespace eda;

    std::vector<IrNode> ports = {
        make_port("clk", "input", 1),
        make_port("q", "output", 8),
    };
    std::vector<IrNode> body = {
        make_wire("q_next", 8),
        make_assign(expr_symbol("q"), expr_symbol("q_next")),
    };
    auto m = make_module("counter", ports, body);

    auto typed = query_typed_signals(m);
    CHECK(typed.size() == 3, "typed-signals: 2 ports + 1 wire");
    return true;
}

bool test_cycle4_check_module_types() {
    PRINTLN("\n--- Test 15: Cycle 4 check-module-types ---");
    using namespace eda;

    auto ok = make_module("ok",
        {make_port("q", "output", 8)},
        {make_wire("q_next", 8),
         make_assign(expr_symbol("q"), expr_symbol("q_next"))});
    CHECK(check_module_types(ok), "matching symbol widths pass");

    auto bad = make_module("bad",
        {make_port("q", "output", 8)},
        {make_assign(expr_symbol("q"), expr_symbol("clk"))});
    CHECK(!check_module_types(bad), "width mismatch fails type check");
    return true;
}

bool test_cycle4_pipeline() {
    PRINTLN("\n--- Test 16: Cycle 4 pipeline (check + emit + gated mutate) ---");
    using namespace eda;

    auto m = make_module("counter",
        {make_port("clk", "input", 1), make_port("en", "input", 1)},
        {});
    auto gated = mutate_insert_clock_gating_checked(m, "clk", "en");
    CHECK(gated.has_value(), "clock-gating-checked accepts 1-bit clk/en");
    if (gated) {
        CHECK(check_module_types(*gated), "gated module passes type check");
        auto out = pipeline_check_and_emit(*gated);
        CHECK(out.has_value(), "pipeline returns verilog string");
        if (out) {
            CHECK(out->find("wire clk_gated;") != std::string::npos,
                  "pipeline emit contains gated wire");
            CHECK(out->find("assign clk_gated = (clk & en);") != std::string::npos,
                  "pipeline emit contains gate assign");
        }
    }

    auto bad_gate = mutate_insert_clock_gating_checked(m, "clk", "q");
    CHECK(!bad_gate.has_value(), "clock-gating-checked rejects missing en port");
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

int run_tests() {
    std::fprintf(stdout,
                 "═══ Issue #182 — Hardware IR + Verilog Backend (Cycle 1-4, C++) ═══\n");
    std::fprintf(stdout, "  Cycle 1: IR + display + basic query.\n");
    std::fprintf(stdout, "  Cycle 2: dependency query + mutate helpers.\n");
    std::fprintf(stdout, "  Cycle 3: Verilog emitter (eda:emit-verilog).\n");
    std::fprintf(stdout, "  Cycle 4: dependent types + type-checked pipeline.\n");
    std::fprintf(stdout, "  Aura stdlib mirror: lib/std/eda.aura (#229-#231 fixed).\n\n");

    test_ir_construction();
    test_ir_accessors();
    test_display();
    test_query_helpers();
    test_predicate_disjoint();
    test_nested_expr();
    test_cycle2_dependency_queries();
    test_cycle2_mutate_rename();
    test_cycle2_mutate_clock_gating();
    test_cycle2_mutate_extract_common();
    test_cycle3_emit_verilog();
    test_cycle3_emit_always();
    test_cycle4_typed_signals();
    test_cycle4_check_module_types();
    test_cycle4_pipeline();
    test_end_to_end_counter();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_182_detail

int aura_issue_182_run() { return aura_issue_182_detail::run_tests(); }

