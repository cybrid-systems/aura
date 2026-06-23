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
