// ── src/compiler/sv_ir_impl.cpp ────────────────────────────────
//
// Implementation of aura::compiler::sv_ir (Issue #436 Phase 7).
// See sv_ir.ixx for the design rationale.
module aura.compiler.sv_ir;
import std;

namespace aura::compiler::sv_ir {

const char* wire_kind_to_symbol(WireKind k) noexcept {
    switch (k) {
        case WireKind::Wire:  return "wire";
        case WireKind::Logic: return "logic";
        case WireKind::Reg:   return "reg";
        case WireKind::Bit:   return "bit";
    }
    return "wire"; // unreachable; defensive
}

WireKind wire_kind_from_symbol(std::string_view s) noexcept {
    if (s == "wire")  return WireKind::Wire;
    if (s == "logic") return WireKind::Logic;
    if (s == "reg")   return WireKind::Reg;
    if (s == "bit")   return WireKind::Bit;
    return WireKind::Wire; // unknown defaults to wire
}

WireIR make_wire(std::string_view name, int width,
                 WireKind kind) noexcept {
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

void append_joined(std::string& out, const std::vector<std::string>& items,
                   const char* sep) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out.append(sep);
        }
        out.append(items[i]);
    }
}

} // anonymous namespace

// ── InterfaceIR ──

InterfaceIR make_interface(std::string_view name,
                           std::vector<std::string> ports,
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

ModportIR make_modport(std::string_view name,
                       std::vector<std::string> port_names) noexcept {
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

// ── ConstraintIR ──

ConstraintIR make_constraint(std::string_view name,
                             std::vector<std::string> expressions) noexcept {
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

CoverpointIR make_coverpoint(std::string_view var,
                             std::vector<std::string> bins) noexcept {
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

CovergroupIR make_covergroup(std::string_view name,
                             std::vector<std::string> coverpoint_strs,
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

SequenceIR make_sequence(std::string_view name,
                         std::string_view expr) noexcept {
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

PropertyIR make_property(std::string_view name,
                         std::string_view expr) noexcept {
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

} // namespace aura::compiler::sv_ir
