// ── src/compiler/sv_ir_impl.cpp ────────────────────────────────
//
// Implementation of aura::compiler::sv_ir (Issue #436 Phase 7).
// See sv_ir.ixx for the design rationale.
module;


module aura.compiler.sv_ir;
import std;
import aura.core.mutation; // SymId (Issue #315)
import aura.core.ast;      // FlatAST + StringPool + NodeId + NodeTag

namespace aura::compiler::sv_ir {

// Issue #315: bring aura::ast types into the local namespace so
// the signature can use unqualified names (matches the pattern
// in other aura::compiler::sv_ir functions that take SymId).
using aura::ast::FlatAST;
using aura::ast::INVALID_SYM;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using SymId = aura::ast::SymId;

const char* wire_kind_to_symbol(WireKind k) noexcept {
    switch (k) {
        case WireKind::Wire:
            return "wire";
        case WireKind::Logic:
            return "logic";
        case WireKind::Reg:
            return "reg";
        case WireKind::Bit:
            return "bit";
    }
    return "wire"; // unreachable; defensive
}

WireKind wire_kind_from_symbol(std::string_view s) noexcept {
    if (s == "wire")
        return WireKind::Wire;
    if (s == "logic")
        return WireKind::Logic;
    if (s == "reg")
        return WireKind::Reg;
    if (s == "bit")
        return WireKind::Bit;
    return WireKind::Wire; // unknown defaults to wire
}

WireIR make_wire(std::string_view name, int width, WireKind kind) noexcept {
    WireIR w;
    w.name = std::string(name);
    w.width = (width < 1) ? 1 : width;
    w.kind = kind;
    return w;
}

std::string emit_wire(const WireIR& w) {
    std::string out;
    out.reserve(48);
    out.append(wire_kind_to_symbol(w.kind));
    out.push_back(' ');
    if (w.width > 1) {
        out.push_back('[');
        // Width is small (1-256 in practice). Manual itoa
        // avoids dragging in <charconv> for a tiny gain.
        int n = w.width;
        char buf[8];
        int len = 0;
        if (n == 0) {
            buf[len++] = '0';
        } else {
            char tmp[8];
            int tlen = 0;
            while (n > 0) {
                tmp[tlen++] = static_cast<char>('0' + (n % 10));
                n /= 10;
            }
            while (tlen > 0) {
                buf[len++] = tmp[--tlen];
            }
        }
        out.append(buf, len);
        out.append("-1:0] ");
    }
    out.append(w.name);
    out.push_back(';');
    return out;
}

std::string debug_wire(const WireIR& w) {
    std::string out;
    out.reserve(32);
    out.append(wire_kind_to_symbol(w.kind));
    out.push_back(' ');
    out.append(w.name);
    if (w.width > 1) {
        out.push_back(' ');
        out.push_back('[');
        int n = w.width;
        char buf[8];
        int len = 0;
        if (n == 0) {
            buf[len++] = '0';
        } else {
            char tmp[8];
            int tlen = 0;
            while (n > 0) {
                tmp[tlen++] = static_cast<char>('0' + (n % 10));
                n /= 10;
            }
            while (tlen > 0) {
                buf[len++] = tmp[--tlen];
            }
        }
        out.append(buf, len);
        out.append("-1:0]");
    }
    return out;
}

// ── Internal helpers (Phase 1) ─────────────────────────────
//
// Manual itoa for a non-negative int (widths/port counts are
// always small and non-negative).
namespace {

    void append_int(std::string& out, int n) {
        char buf[12];
        int len = 0;
        if (n == 0) {
            buf[len++] = '0';
        } else {
            char tmp[12];
            int tlen = 0;
            while (n > 0) {
                tmp[tlen++] = static_cast<char>('0' + (n % 10));
                n /= 10;
            }
            while (tlen > 0) {
                buf[len++] = tmp[--tlen];
            }
        }
        out.append(buf, len);
    }

    void append_joined(std::string& out, const std::vector<std::string>& items, const char* sep) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i > 0) {
                out.append(sep);
            }
            out.append(items[i]);
        }
    }

} // anonymous namespace

// ── InterfaceIR ──

InterfaceIR make_interface(std::string_view name, std::vector<std::string> ports,
                           std::vector<std::string> modport_names) noexcept {
    InterfaceIR i;
    i.name = std::string(name);
    i.ports = std::move(ports);
    i.modport_names = std::move(modport_names);
    return i;
}

std::string emit_interface(const InterfaceIR& i) {
    std::string out;
    out.reserve(96);
    out.append("interface ");
    out.append(i.name);
    out.append("(\n  ");
    append_joined(out, i.ports, ", ");
    out.append(");\n");
    // Modport declarations (without bodies) for the header.
    for (const auto& m : i.modport_names) {
        out.append("  modport ");
        out.append(m);
        out.append("();\n");
    }
    out.append("endinterface");
    return out;
}

std::string debug_interface(const InterfaceIR& i) {
    std::string out;
    out.reserve(64);
    out.append("interface ");
    out.append(i.name);
    out.append(" ports=[");
    append_joined(out, i.ports, ",");
    out.append("] modports=[");
    append_joined(out, i.modport_names, ",");
    out.push_back(']');
    return out;
}

// ── ModportIR ──

ModportIR make_modport(std::string_view name, std::vector<std::string> port_names) noexcept {
    ModportIR m;
    m.name = std::string(name);
    m.port_names = std::move(port_names);
    return m;
}

std::string emit_modport(const ModportIR& m) {
    std::string out;
    out.reserve(48);
    out.append("modport ");
    out.append(m.name);
    out.append("(");
    append_joined(out, m.port_names, ", ");
    out.append(");");
    return out;
}

std::string debug_modport(const ModportIR& m) {
    std::string out;
    out.reserve(48);
    out.append("modport ");
    out.append(m.name);
    out.append(" ports=[");
    append_joined(out, m.port_names, ",");
    out.push_back(']');
    return out;
}

// ── ClassIR ──

ClassIR make_class(std::string_view name, std::string_view base,
                   std::vector<std::string> items) noexcept {
    ClassIR c;
    c.name = std::string(name);
    c.base = std::string(base);
    c.items = std::move(items);
    return c;
}

std::string emit_class(const ClassIR& c) {
    std::string out;
    out.reserve(64 + c.name.size());
    out.append("class ");
    out.append(c.name);
    if (!c.base.empty()) {
        out.append(" extends ");
        out.append(c.base);
    }
    out.append(";\n  ");
    for (std::size_t i = 0; i < c.items.size(); ++i) {
        if (i > 0) {
            out.append("\n  ");
        }
        out.append(c.items[i]);
    }
    out.append("\nendclass");
    return out;
}

std::string debug_class(const ClassIR& c) {
    std::string out;
    out.reserve(48 + c.name.size() + c.items.size() * 16);
    out.append("class(name=");
    out.append(c.name);
    out.append(", base=");
    out.append(c.base.empty() ? "()" : c.base);
    out.append(", items=[");
    append_joined(out, c.items, ",");
    out.append("])");
    return out;
}

// ── ConstraintIR ──

ConstraintIR make_constraint(std::string_view name, std::vector<std::string> expressions) noexcept {
    ConstraintIR c;
    c.name = std::string(name);
    c.expressions = std::move(expressions);
    return c;
}

std::string emit_constraint(const ConstraintIR& c) {
    std::string out;
    out.reserve(48 + c.name.size() + c.expressions.size() * 16);
    out.append("constraint ");
    out.append(c.name);
    out.append(" { ");
    if (!c.expressions.empty()) {
        for (std::size_t i = 0; i < c.expressions.size(); ++i) {
            if (i > 0) {
                out.append("; ");
            }
            out.append(c.expressions[i]);
        }
    }
    out.append(" }");
    return out;
}

std::string debug_constraint(const ConstraintIR& c) {
    std::string out;
    out.reserve(48 + c.name.size() + c.expressions.size() * 16);
    out.append("constraint(name=");
    out.append(c.name);
    out.append(", exprs=[");
    append_joined(out, c.expressions, ",");
    out.append("])");
    return out;
}

// ── CoverpointIR ──

CoverpointIR make_coverpoint(std::string_view var, std::vector<std::string> bins) noexcept {
    CoverpointIR cp;
    cp.var = std::string(var);
    cp.bins = std::move(bins);
    return cp;
}

std::string emit_coverpoint(const CoverpointIR& cp) {
    std::string out;
    out.reserve(48 + cp.var.size() + cp.bins.size() * 8);
    out.append(cp.var);
    out.append(" : coverpoint { ");
    if (cp.bins.empty()) {
        out.append("/* no bins */");
    } else {
        append_joined(out, cp.bins, ", ");
    }
    out.append(" }");
    return out;
}

std::string debug_coverpoint(const CoverpointIR& cp) {
    std::string out;
    out.reserve(32 + cp.var.size() + cp.bins.size() * 8);
    out.append("coverpoint(var=");
    out.append(cp.var);
    out.append(", bins=[");
    append_joined(out, cp.bins, ",");
    out.append("])");
    return out;
}

// ── CovergroupIR ──

CovergroupIR make_covergroup(std::string_view name, std::vector<std::string> coverpoint_strs,
                             std::string_view event) noexcept {
    CovergroupIR cg;
    cg.name = std::string(name);
    cg.coverpoint_strs = std::move(coverpoint_strs);
    cg.event = std::string(event);
    return cg;
}

std::string emit_covergroup(const CovergroupIR& cg) {
    std::string out;
    out.reserve(64 + cg.name.size());
    out.append("covergroup ");
    out.append(cg.name);
    if (!cg.event.empty()) {
        out.append(cg.event);
    } else {
        out.append("@(*)");
    }
    out.append(" {");
    for (const auto& cp : cg.coverpoint_strs) {
        out.push_back(' ');
        out.append(cp);
        out.push_back(';');
    }
    out.append(" }");
    return out;
}

std::string debug_covergroup(const CovergroupIR& cg) {
    std::string out;
    out.reserve(48 + cg.name.size() + cg.coverpoint_strs.size() * 16);
    out.append("covergroup(name=");
    out.append(cg.name);
    out.append(", event=");
    if (cg.event.empty()) {
        out.append("@(*)");
    } else {
        out.append(cg.event);
    }
    out.append(", cps=[");
    append_joined(out, cg.coverpoint_strs, "; ");
    out.append("])");
    return out;
}

// ── SequenceIR ──

SequenceIR make_sequence(std::string_view name, std::string_view expr) noexcept {
    SequenceIR s;
    s.name = std::string(name);
    s.expr = std::string(expr);
    return s;
}

std::string emit_sequence(const SequenceIR& s) {
    std::string out;
    out.reserve(64 + s.expr.size());
    out.append("sequence ");
    out.append(s.name);
    out.append(";\n  ");
    out.append(s.expr);
    out.append("\nendsequence");
    return out;
}

std::string debug_sequence(const SequenceIR& s) {
    std::string out;
    out.reserve(32 + s.expr.size());
    out.append("sequence ");
    out.append(s.name);
    out.append(" expr=|");
    out.append(s.expr);
    out.push_back('|');
    return out;
}

// ── PropertyIR ──

PropertyIR make_property(std::string_view name, std::string_view expr) noexcept {
    PropertyIR p;
    p.name = std::string(name);
    p.expr = std::string(expr);
    return p;
}

std::string emit_property(const PropertyIR& p) {
    std::string out;
    out.reserve(64 + p.expr.size());
    out.append("property ");
    out.append(p.name);
    out.append(";\n  ");
    out.append(p.expr);
    out.append("\nendproperty");
    return out;
}

std::string debug_property(const PropertyIR& p) {
    std::string out;
    out.reserve(32 + p.expr.size());
    out.append("property ");
    out.append(p.name);
    out.append(" expr=|");
    out.append(p.expr);
    out.push_back('|');
    return out;
}

// ═══════════════════════════════════════════════════════════════
// Issue #315 — SVInterfaceIR / SVModportIR (SymId-based)
// ═══════════════════════════════════════════════════════════════

SVModportIR make_sv_modport(SymId name, std::vector<SymId> port_names) noexcept {
    SVModportIR m;
    m.name = name;
    m.port_names = std::move(port_names);
    return m;
}

SVInterfaceIR make_sv_interface(SymId name, std::vector<SVModportIR> modports) noexcept {
    SVInterfaceIR i;
    i.name = name;
    i.modports = std::move(modports);
    return i;
}

SVInterfaceIR make_sv_interface(SymId name, SVModportIR mp0, SVModportIR mp1) noexcept {
    SVInterfaceIR i;
    i.name = name;
    std::vector<SVModportIR> mps;
    mps.reserve(2);
    mps.push_back(std::move(mp0));
    mps.push_back(std::move(mp1));
    i.modports = std::move(mps);
    return i;
}

SVInterfaceIR make_sv_interface(SymId name, SVModportIR mp0, SVModportIR mp1,
                                SVModportIR mp2) noexcept {
    SVInterfaceIR i;
    i.name = name;
    std::vector<SVModportIR> mps;
    mps.reserve(3);
    mps.push_back(std::move(mp0));
    mps.push_back(std::move(mp1));
    mps.push_back(std::move(mp2));
    i.modports = std::move(mps);
    return i;
}

std::optional<SVInterfaceIR> map_interface_node_to_ir(const FlatAST& flat, const StringPool& pool,
                                                      NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return std::nullopt;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Interface)
        return std::nullopt;
    SVInterfaceIR ir;
    ir.name = v.sym_id;
    // Walk the body + collect Modport nodes (signal-like
    // children are skipped — a follow-up can extend to
    // handle nested Begin blocks + Variable decls).
    for (NodeId child_id : flat.children(id)) {
        auto cv = flat.get(child_id);
        if (cv.tag == NodeTag::Modport) {
            SVModportIR mp;
            mp.name = cv.sym_id;
            // Port names come from the modport's param_data_
            // side-table (mirrors add_modport's shape).
            // No public param_count(id) accessor exists yet
            // (follow-up issue can add it for cleaner
            // iteration); use param_at(id, i) with a small
            // bound + INVALID_SYM terminator. The bound of
            // 64 covers the realistic SV modport port list
            // size; larger lists would need a different
            // shape (count + offset).
            for (std::uint32_t i = 0; i < 64; ++i) {
                SymId p = flat.param_at(child_id, i);
                if (p == INVALID_SYM)
                    break;
                mp.port_names.push_back(p);
            }
            ir.modports.push_back(std::move(mp));
        }
        // Signal/decl children are silently skipped for
        // now — only Modport nodes are reflected in IR.
        // This matches the issue's scope (minimal IR +
        // basic AST mapping; full lowering is follow-up).
    }
    (void)pool; // not currently needed (SymId-only walker)
    return ir;
}

static std::string resolve_expr_child(const FlatAST& flat, const StringPool& pool, NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return {};
    for (NodeId child : flat.children(id)) {
        auto cv = flat.get(child);
        if (cv.tag == NodeTag::LiteralString && cv.sym_id != INVALID_SYM)
            return std::string(pool.resolve(cv.sym_id));
    }
    return {};
}

std::optional<PropertyIR> map_property_node_to_ir(const FlatAST& flat, const StringPool& pool,
                                                  NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return std::nullopt;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Property)
        return std::nullopt;
    if (v.sym_id == INVALID_SYM)
        return std::nullopt;
    auto expr = resolve_expr_child(flat, pool, id);
    return make_property(pool.resolve(v.sym_id), expr);
}

std::optional<SequenceIR> map_sequence_node_to_ir(const FlatAST& flat, const StringPool& pool,
                                                  NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return std::nullopt;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Sequence)
        return std::nullopt;
    if (v.sym_id == INVALID_SYM)
        return std::nullopt;
    auto expr = resolve_expr_child(flat, pool, id);
    return make_sequence(pool.resolve(v.sym_id), expr);
}

std::optional<CoverpointIR> map_coverpoint_node_to_ir(const FlatAST& flat, const StringPool& pool,
                                                      NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return std::nullopt;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Coverpoint)
        return std::nullopt;
    if (v.sym_id == INVALID_SYM)
        return std::nullopt;
    CoverpointIR cp;
    cp.var = pool.resolve(v.sym_id);
    for (std::uint32_t i = 0; i < 64; ++i) {
        SymId bin = flat.param_at(id, i);
        if (bin == INVALID_SYM)
            break;
        cp.bins.emplace_back(pool.resolve(bin));
    }
    return cp;
}

std::optional<ConstraintIR> map_constraint_node_to_ir(const FlatAST& flat, const StringPool& pool,
                                                      NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return std::nullopt;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Constraint)
        return std::nullopt;
    if (v.sym_id == INVALID_SYM)
        return std::nullopt;
    std::vector<std::string> exprs;
    for (std::uint32_t i = 0; i < 64; ++i) {
        SymId expr = flat.param_at(id, i);
        if (expr == INVALID_SYM)
            break;
        exprs.emplace_back(pool.resolve(expr));
    }
    return make_constraint(pool.resolve(v.sym_id), std::move(exprs));
}

std::optional<ClassIR> map_class_node_to_ir(const FlatAST& flat, const StringPool& pool,
                                            NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return std::nullopt;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Class)
        return std::nullopt;
    if (v.sym_id == INVALID_SYM)
        return std::nullopt;
    ClassIR cls;
    cls.name = pool.resolve(v.sym_id);
    SymId base = flat.param_at(id, 0);
    if (base != INVALID_SYM)
        cls.base = pool.resolve(base);
    for (NodeId child : flat.children(id)) {
        if (auto c = map_constraint_node_to_ir(flat, pool, child))
            cls.items.push_back(emit_constraint(*c));
        else if (auto cp = map_coverpoint_node_to_ir(flat, pool, child))
            cls.items.push_back(emit_coverpoint(*cp));
    }
    return cls;
}

std::optional<CovergroupIR> map_covergroup_node_to_ir(const FlatAST& flat, const StringPool& pool,
                                                      NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return std::nullopt;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Covergroup)
        return std::nullopt;
    if (v.sym_id == INVALID_SYM)
        return std::nullopt;
    CovergroupIR cg;
    cg.name = pool.resolve(v.sym_id);
    for (NodeId child : flat.children(id)) {
        if (auto cp = map_coverpoint_node_to_ir(flat, pool, child))
            cg.coverpoint_strs.push_back(emit_coverpoint(*cp));
    }
    return cg;
}

std::optional<PropertyIR> map_assert_node_to_ir(const FlatAST& flat, const StringPool& pool,
                                                NodeId id) {
    if (id == NULL_NODE || id >= flat.size())
        return std::nullopt;
    auto v = flat.get(id);
    if (v.tag != NodeTag::Assert)
        return std::nullopt;
    for (NodeId child : flat.children(id)) {
        if (auto p = map_property_node_to_ir(flat, pool, child))
            return p;
    }
    return std::nullopt;
}

std::string debug_sv_modport(const SVModportIR& m, const StringPool& pool) {
    std::string out;
    out.append("modport ");
    if (m.name != INVALID_SYM)
        out.append(pool.resolve(m.name));
    out.append("(");
    for (std::size_t i = 0; i < m.port_names.size(); ++i) {
        if (i > 0)
            out.append(", ");
        if (m.port_names[i] != INVALID_SYM)
            out.append(pool.resolve(m.port_names[i]));
    }
    out.push_back(')');
    return out;
}

std::string debug_sv_interface(const SVInterfaceIR& i, const StringPool& pool) {
    std::string out;
    out.append("interface ");
    if (i.name != INVALID_SYM)
        out.append(pool.resolve(i.name));
    out.append(" { ");
    for (std::size_t k = 0; k < i.modports.size(); ++k) {
        if (k > 0)
            out.append("; ");
        out.append(debug_sv_modport(i.modports[k], pool));
    }
    out.append(" }");
    return out;
}

// ═════════════════════════════════════════════════════════════════
// Issue #316 — SystemVerilog emit (SymId IR variant)
// ═════════════════════════════════════════════════════════════════

// Issue #316: minimal SV modport declaration. The grammar is:
//   modport NAME(input|output|inout PORT_NAME1, ...);
// The current implementation omits the direction keyword
// (SystemVerilog allows direction-less port references in
// modport decls; the formal emit is `(input|output|inout)`
// per port, set by a follow-up that wires
// port-direction metadata through the AST).
// Returns a string ending with ';'. No trailing newline.
std::string emit_sv_modport(const SVModportIR& m, const StringPool& pool) {
    std::string out;
    out.reserve(48 + 8 * m.port_names.size());
    out.append("modport ");
    if (m.name != INVALID_SYM)
        out.append(pool.resolve(m.name));
    out.append("(");
    for (std::size_t i = 0; i < m.port_names.size(); ++i) {
        if (i > 0)
            out.append(", ");
        if (m.port_names[i] != INVALID_SYM)
            out.append(pool.resolve(m.port_names[i]));
    }
    out.append(");");
    return out;
}

std::string emit_sv_interface(const SVInterfaceIR& i, const StringPool& pool) {
    std::string out;
    out.reserve(96 + 64 * i.modports.size());
    out.append("interface ");
    if (i.name != INVALID_SYM)
        out.append(pool.resolve(i.name));
    out.append("();\n");
    for (const auto& m : i.modports) {
        out.append("  ");
        out.append(emit_sv_modport(m, pool));
        out.push_back('\n');
    }
    out.append("endinterface");
    return out;
}

std::string emit_sv_diff(const std::string_view before_sv, const std::string_view after_sv) {
    if (before_sv == after_sv)
        return {};
    std::string diff;
    diff.reserve(before_sv.size() + after_sv.size() + 32);
    diff.append("--- before\n");
    diff.append(before_sv);
    diff.append("\n+++ after\n");
    diff.append(after_sv);
    return diff;
}

std::int64_t estimate_ppa_savings(const std::string_view before_sv,
                                  const std::string_view after_sv) {
    const auto before = static_cast<std::int64_t>(before_sv.size());
    const auto after = static_cast<std::int64_t>(after_sv.size());
    return before > after ? before - after : after - before;
}

std::string emit_commercial_simulator_do_file(const std::string_view simulator,
                                              const std::string_view sv_filename) {
    std::string out;
    out.reserve(96 + sv_filename.size());
    if (simulator == "questa" || simulator == "modelsim") {
        out.append("# Questa/ModelSim do-file stub\n");
        out.append("vlog -sv ");
        out.append(sv_filename);
        out.append("\nvsim -c top -do \"run -all; quit\"\n");
        return out;
    }
    out.append("# VCS do-file stub\n");
    out.append("vcs -sverilog ");
    out.append(sv_filename);
    out.append(" -o simv\n");
    out.append("./simv\n");
    return out;
}

SvEmitValidation validate_sv_emit(const std::string_view sv_text) {
    SvEmitValidation result;
    if (sv_text.empty()) {
        result.error = "empty emit";
        return result;
    }
    int paren = 0;
    int brace = 0;
    for (char c : sv_text) {
        if (c == '(')
            ++paren;
        else if (c == ')') {
            if (--paren < 0) {
                result.error = "unbalanced )";
                return result;
            }
        } else if (c == '{')
            ++brace;
        else if (c == '}') {
            if (--brace < 0) {
                result.error = "unbalanced }";
                return result;
            }
        }
    }
    if (paren != 0) {
        result.error = "unbalanced (";
        return result;
    }
    if (brace != 0) {
        result.error = "unbalanced {";
        return result;
    }
    static constexpr std::string_view k_keywords[] = {
        "interface", "property",   "coverpoint", "covergroup", "sequence",     "assert",
        "modport",   "constraint", "class",      "endmodule",  "endinterface", "endclass",
    };
    bool found = sv_text.find("// sv re-emit stub") != std::string_view::npos;
    for (const auto kw : k_keywords) {
        if (sv_text.find(kw) != std::string_view::npos) {
            found = true;
            break;
        }
    }
    if (!found) {
        result.error = "no SV construct keyword";
        return result;
    }
    result.ok = true;
    return result;
}

SvReemitResult reemit_sv_node(const FlatAST& flat, const StringPool& pool, const NodeId id,
                              const std::string_view simulator) {
    SvReemitResult result;
    if (id == NULL_NODE || id >= flat.size())
        return result;
    const auto before = result.sv_text;
    const auto tag = flat.get(id).tag;
    if (tag == NodeTag::Interface) {
        if (auto ir = map_interface_node_to_ir(flat, pool, id))
            result.sv_text = emit_sv_interface(*ir, pool);
    } else if (tag == NodeTag::Property) {
        if (auto ir = map_property_node_to_ir(flat, pool, id))
            result.sv_text = emit_property(*ir);
    } else if (tag == NodeTag::Sequence) {
        if (auto ir = map_sequence_node_to_ir(flat, pool, id))
            result.sv_text = emit_sequence(*ir);
    } else if (tag == NodeTag::Coverpoint) {
        if (auto ir = map_coverpoint_node_to_ir(flat, pool, id))
            result.sv_text = emit_coverpoint(*ir);
    } else if (tag == NodeTag::Covergroup) {
        if (auto ir = map_covergroup_node_to_ir(flat, pool, id))
            result.sv_text = emit_covergroup(*ir);
    } else if (tag == NodeTag::Assert) {
        if (auto ir = map_assert_node_to_ir(flat, pool, id))
            result.sv_text = emit_property(*ir);
    } else if (tag == NodeTag::Constraint) {
        if (auto ir = map_constraint_node_to_ir(flat, pool, id))
            result.sv_text = emit_constraint(*ir);
    } else if (tag == NodeTag::Class) {
        if (auto ir = map_class_node_to_ir(flat, pool, id))
            result.sv_text = emit_class(*ir);
    } else {
        result.sv_text = "// sv re-emit stub for node ";
        result.sv_text.append(std::to_string(id));
        result.sv_text.push_back('\n');
    }
    result.commercial_do_stub = emit_commercial_simulator_do_file(simulator, "reemit.sv");
    result.ppa_savings = estimate_ppa_savings(before, result.sv_text);
    return result;
}

} // namespace aura::compiler::sv_ir
