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
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

export module aura.compiler.sv_ir;
import std;
import aura.core.mutation; // Issue #315: SymId for the structured
                           // SVInterfaceIR / SVModportIR shapes.
import aura.core.ast;      // Issue #315: FlatAST + StringPool for
                           // the AST → IR mapping helper.

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

export InterfaceIR make_interface(std::string_view name, std::vector<std::string> ports,
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

export ModportIR make_modport(std::string_view name, std::vector<std::string> port_names) noexcept;

export std::string emit_modport(const ModportIR& m);
export std::string debug_modport(const ModportIR& m);

// ── ClassIR ──
//
// Issue #435 Phase 6 — structured IR for SV class
// declarations (OOP + randomization container). Mirrors the
// list-based `(eda:class name base items)` representation.
//
// Layout:
//   "class NAME extends BASE;\n"
//   "  <member1>\n"
//   "  <member2>\n"
//   "endclass"
// (base clause omitted when base is empty; member string
// items are joined with "\n  " and routed via
// emit_class_item — constraints and covergroups use their
// own structured emit, plain strings are emitted as-is.)
export struct ClassIR {
    std::string name;
    std::string base;
    std::vector<std::string> items;
};

export ClassIR make_class(std::string_view name, std::string_view base,
                          std::vector<std::string> items) noexcept;

export std::string emit_class(const ClassIR& c);
export std::string debug_class(const ClassIR& c);

// ── ConstraintIR ──
//
// Issue #435 Phase 5 — structured IR for SV constraints
// (class randomization). Mirrors the list-based
// `(eda:constraint name expressions)` representation.
//
// Layout:
//   "constraint NAME { EXPR1; EXPR2; }"
export struct ConstraintIR {
    std::string name;
    std::vector<std::string> expressions;
};

export ConstraintIR make_constraint(std::string_view name,
                                    std::vector<std::string> expressions) noexcept;

export std::string emit_constraint(const ConstraintIR& c);
export std::string debug_constraint(const ConstraintIR& c);

// ── CoverpointIR ──
//
// Issue #435 Phase 4 — structured IR for SV coverpoint.
// Mirrors the list-based
// `(eda:coverpoint var bins)` representation.
//
// Layout:
//   "VAR : coverpoint { BIN1, BIN2, BIN3 }"
// (or "VAR : coverpoint { /* no bins */ }" when bins is empty)
export struct CoverpointIR {
    std::string var;
    std::vector<std::string> bins;
};

export CoverpointIR make_coverpoint(std::string_view var, std::vector<std::string> bins) noexcept;

export std::string emit_coverpoint(const CoverpointIR& cp);
export std::string debug_coverpoint(const CoverpointIR& cp);

// ── CovergroupIR ──
//
// Issue #435 Phase 4 — structured IR for SV covergroup.
// Mirrors the list-based
// `(eda:covergroup name coverpoints event)` representation.
//
// Layout:
//   "covergroup NAME@(*) { cp1; cp2; }"
// (event defaults to "@(*)" when empty)
export struct CovergroupIR {
    std::string name;
    std::vector<std::string> coverpoint_strs;
    std::string event;
};

export CovergroupIR make_covergroup(std::string_view name, std::vector<std::string> coverpoint_strs,
                                    std::string_view event = "") noexcept;

export std::string emit_covergroup(const CovergroupIR& cg);
export std::string debug_covergroup(const CovergroupIR& cg);

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

export SequenceIR make_sequence(std::string_view name, std::string_view expr) noexcept;

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

export PropertyIR make_property(std::string_view name, std::string_view expr) noexcept;

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
export WireIR make_wire(std::string_view name, int width, WireKind kind) noexcept;

// Emits the wire declaration as a single line of Verilog.
// Result ends with ';' but no trailing newline.
export std::string emit_wire(const WireIR& w);

// Format as a debug string: "<kind> <name> [W-1:0]".
export std::string debug_wire(const WireIR& w);

// ── SVInterfaceIR / SVModportIR (SymId-based structured IR) ───
//
// Issue #315 — parallel API to InterfaceIR / ModportIR (above).
// Both representations exist:
//   - InterfaceIR / ModportIR use std::string for names
//     (issue #435 — the list-emit-friendly variant)
//   - SVInterfaceIR / SVModportIR use SymId for names
//     (issue #315 — the AST-mapping-friendly variant)
//
// The SymId-based layer is canonical for the lowering pass
// + AST mutation pipeline because SymId is stable across
// editing sessions (interned once, reused forever) and
// directly comparable. The std::string layer stays for the
// emit path where human-readable text is the form.
export struct SVModportIR {
    aura::ast::SymId name = aura::ast::SymId{}; // INVALID_SYM = uninitialized
    std::vector<aura::ast::SymId> port_names;   // ports referenced by this modport
};

export struct SVInterfaceIR {
    aura::ast::SymId name = aura::ast::SymId{};
    std::vector<SVModportIR> modports; // nested modport declarations
};

// Constructors.
export SVModportIR make_sv_modport(aura::ast::SymId name,
                                   std::vector<aura::ast::SymId> port_names) noexcept;

export SVInterfaceIR make_sv_interface(aura::ast::SymId name,
                                       std::vector<SVModportIR> modports) noexcept;

// Convenience overload that takes 2 modports as separate args
// (avoids the {a, b} initializer-list dance in callers — the
// vector-only form is the canonical one for variadic use).
export SVInterfaceIR make_sv_interface(aura::ast::SymId name, SVModportIR mp0,
                                       SVModportIR mp1) noexcept;

// Convenience overload that takes 3 modports (covers the
// common bus-style 3-role interfaces: master/slave/monitor).
export SVInterfaceIR make_sv_interface(aura::ast::SymId name, SVModportIR mp0, SVModportIR mp1,
                                       SVModportIR mp2) noexcept;

// Issue #315: AST → IR mapping. Walks the body of an
// Interface AST node + collects the nested Modport nodes.
// Not a full lowering pass — surface-only conversion.
//
// Returns std::nullopt if the node isn't an Interface, or
// if any body item has an unexpected tag (the surface-only
// walker only handles Modport children; signals + nested
// modules are skipped silently — a follow-up issue can
// extend the walker to handle them). The expectation is
// that callers either pre-validate the AST with a query:
// (query:by-tag :interface) or accept that some signals
// aren't reflected in the IR (for tests that build the
// AST directly).
export std::optional<SVInterfaceIR> map_interface_node_to_ir(const aura::ast::FlatAST& flat,
                                                             const aura::ast::StringPool& pool,
                                                             aura::ast::NodeId id);

// Issue #694: AST tag → string-based SVA IR mappers.
export std::optional<PropertyIR> map_property_node_to_ir(const aura::ast::FlatAST& flat,
                                                         const aura::ast::StringPool& pool,
                                                         aura::ast::NodeId id);

export std::optional<SequenceIR> map_sequence_node_to_ir(const aura::ast::FlatAST& flat,
                                                         const aura::ast::StringPool& pool,
                                                         aura::ast::NodeId id);

export std::optional<CoverpointIR> map_coverpoint_node_to_ir(const aura::ast::FlatAST& flat,
                                                             const aura::ast::StringPool& pool,
                                                             aura::ast::NodeId id);

export std::optional<CovergroupIR> map_covergroup_node_to_ir(const aura::ast::FlatAST& flat,
                                                             const aura::ast::StringPool& pool,
                                                             aura::ast::NodeId id);

export std::optional<PropertyIR> map_assert_node_to_ir(const aura::ast::FlatAST& flat,
                                                       const aura::ast::StringPool& pool,
                                                       aura::ast::NodeId id);

// Issue #496: AST tag → ConstraintIR / ClassIR mappers.
export std::optional<ConstraintIR> map_constraint_node_to_ir(const aura::ast::FlatAST& flat,
                                                             const aura::ast::StringPool& pool,
                                                             aura::ast::NodeId id);

export std::optional<ClassIR> map_class_node_to_ir(const aura::ast::FlatAST& flat,
                                                   const aura::ast::StringPool& pool,
                                                   aura::ast::NodeId id);

// Debug helpers (SymId-aware — resolve via the pool).
export std::string debug_sv_modport(const SVModportIR& m, const aura::ast::StringPool& pool);

export std::string debug_sv_interface(const SVInterfaceIR& i, const aura::ast::StringPool& pool);

// ═════════════════════════════════════════════════════════════════
// Issue #316 — SystemVerilog emit (symid IR variant)
// ═════════════════════════════════════════════════════════════════
//
// Minimal SV emit for the SVInterfaceIR / SVModportIR layer (the
// SymId-based structured IR from #315). The existing #435 layer
// already emits the std::string InterfaceIR / ModportIR variant;
// these exports are the parallel emit API for the SymId-based
// pipeline.
//
// Output grammar (matches the SV Language Standard 1800-2017,
// Section 25.10 — Interfaces):
//   interface NAME();
//     modport M1(...);
//     modport M2(...);
//   endinterface
//
// Missing or INVALID_SYM symbol entries are silently skipped
// (the caller is responsible for validating the IR before
// emit; a follow-up issue can wire the validate pass).

// Emit a modport declaration as a single line (no trailing
// newline). Example output:
//   modport master(input data, output valid);
export std::string emit_sv_modport(const SVModportIR& m, const aura::ast::StringPool& pool);

// Emit a complete interface block. The returned text is ready
// to append to a .sv file (the caller adds the trailing newline
// if desired; the current grammar doesn't require one). Example:
//   interface Bus();
//     modport master(input data, output valid);
//   endinterface
export std::string emit_sv_interface(const SVInterfaceIR& i, const aura::ast::StringPool& pool);

// Issue #693: SV closed-loop emit helpers.
export struct SvReemitResult {
    std::string sv_text;
    std::string commercial_do_stub;
    std::int64_t ppa_savings = 0;
};

export std::string emit_sv_diff(std::string_view before_sv, std::string_view after_sv);

export std::int64_t estimate_ppa_savings(std::string_view before_sv, std::string_view after_sv);

export std::string emit_commercial_simulator_do_file(std::string_view simulator,
                                                     std::string_view sv_filename);

export SvReemitResult reemit_sv_node(const aura::ast::FlatAST& flat,
                                     const aura::ast::StringPool& pool, aura::ast::NodeId id,
                                     std::string_view simulator = "vcs");

// Issue #698: mock SV emit validator (balanced delimiters + construct keywords).
export struct SvEmitValidation {
    bool ok = false;
    std::string error;
};

export SvEmitValidation validate_sv_emit(std::string_view sv_text);

} // namespace aura::compiler::sv_ir
