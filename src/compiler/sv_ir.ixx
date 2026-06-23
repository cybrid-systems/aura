// ── src/compiler/sv_ir.ixx ─────────────────────────────────────
//
// Issue #436 Phase 7 — Structured C++ IR for SystemVerilog
// core constructs. Demonstrates the pattern that #435 will
// scale: dedicated C++ types replace the list-based Aura
// representation for the most-used SV IR nodes.
//
// Phase 7 is intentionally minimal — only WireIR (the most-
// used type-defining construct). The other 4-element list
// types (eda:port, eda:module, eda:always, eda:signal) can
// each get a similar C++ struct in subsequent issues under
// the #435 umbrella.
//
// Backward compat: the list-based eda:wire IR
// `(eda:wire name width kind)` is unchanged. Existing list
// consumers (test_issue_284/436/phase2-6) continue to work.
// The structured WireIR is a new parallel API; the two can
// be mixed in the same module body (a future migration would
// convert list-based to structured).
//
// The C++ type is allocated in the module's pmr arena so
// the lifetime matches the existing IR lifecycle.
export module aura.compiler.sv_ir;
import std;

namespace aura::compiler::sv_ir {

// ── WireKind ──
//
// Matches the Aura symbol names used by the list-based
// eda:wire-kind (Issue #436 Phase 4). The C++ enum is the
// canonical source; the Aura side converts symbol→enum when
// bridging.
export enum class WireKind : std::uint8_t {
    Wire = 0,
    Logic = 1,
    Reg = 2,
    Bit = 3,
};

// Convert enum to its Aura-symbol name (lowercase).
export const char* wire_kind_to_symbol(WireKind k) noexcept;

// Convert Aura-symbol name to enum; returns Wire on no match.
export WireKind wire_kind_from_symbol(std::string_view s) noexcept;

// ── InterfaceIR ──
//
// Issue #435 Phase 1 — structured IR for SV interface
// declarations. Mirrors the list-based
// `(eda:interface name ports modports)` representation but
// uses a fixed-shape struct that the optimizer, formatter,
// and (eventual) mutation primitives can target without
// pattern-matching on tagged-list layout.
//
// Layout:
//   "interface NAME(\n  <port-list>);\n  <modports>\nendinterface"
export struct InterfaceIR {
    std::string name;
    std::vector<std::string> ports;
    std::vector<std::string> modport_names;
};

export InterfaceIR make_interface(std::string_view name,
                                  std::vector<std::string> ports,
                                  std::vector<std::string> modport_names) noexcept;

export std::string emit_interface(const InterfaceIR& i);
export std::string debug_interface(const InterfaceIR& i);

// ── ModportIR ──
//
// Issue #435 Phase 2 — named view of an interface's ports.
// Layout (per modport):
//   "modport NAME(input clk, output data);"
export struct ModportIR {
    std::string name;
    std::vector<std::string> port_names;
};

export ModportIR make_modport(std::string_view name,
                              std::vector<std::string> port_names) noexcept;

export std::string emit_modport(const ModportIR& m);
export std::string debug_modport(const ModportIR& m);

// ── SequenceIR ──
//
// Issue #435 Phase 3 — structured IR for SV sequence
// declarations (SVA). Mirrors the list-based
// `(eda:sequence name expr)` representation.
//
// Layout:
//   "sequence NAME;\n  EXPR\nendsequence"
export struct SequenceIR {
    std::string name;
    std::string expr;
};

export SequenceIR make_sequence(std::string_view name,
                                std::string_view expr) noexcept;

export std::string emit_sequence(const SequenceIR& s);
export std::string debug_sequence(const SequenceIR& s);

// ── PropertyIR ──
//
// Issue #435 Phase 3 — structured IR for SV property
// declarations (SVA). Mirrors the list-based
// `(eda:property name expr)` representation.
//
// Layout:
//   "property NAME;\n  EXPR\nendproperty"
export struct PropertyIR {
    std::string name;
    std::string expr;
};

export PropertyIR make_property(std::string_view name,
                                std::string_view expr) noexcept;

export std::string emit_property(const PropertyIR& p);
export std::string debug_property(const PropertyIR& p);

// ── WireIR ──
//
// A single SystemVerilog wire/logic/reg/bit declaration.
//
// Layout:
//   "wire   [W-1:0] name;"     (width > 1)
//   "wire         name;"     (width = 1)
export struct WireIR {
    std::string name;
    int width = 1;
    WireKind kind = WireKind::Wire;
};

// Build a structured wire declaration.
export WireIR make_wire(std::string_view name, int width,
                        WireKind kind) noexcept;

// Emits the wire declaration as a single line of Verilog.
// Result ends with ';' but no trailing newline.
export std::string emit_wire(const WireIR& w);

// Format as a debug string: "<kind> <name> [W-1:0]".
export std::string debug_wire(const WireIR& w);

} // namespace aura::compiler::sv_ir
