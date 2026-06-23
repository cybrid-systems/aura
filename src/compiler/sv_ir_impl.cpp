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

} // namespace aura::compiler::sv_ir
