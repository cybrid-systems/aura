// module_boundary.ixx — Issue #1885: explicit module boundaries & layering.
//
// Single authority for Aura's dependency direction, layer inventory, and
// cross-layer contracts. Complements aura.core.concepts (shapes) and
// aura.core.concept_constraints (Pass concepts).
//
// Intended audience: humans + AI agents assessing blast radius of a change.
// Rules here are compile-time checkable where practical; the rest is the
// documented DAG that architecture review must match.
//
// Dependency direction (allowed "from → to" imports):
//
//   Core  ← Parser ← Compiler ← {Serve, Exec, Repl, Reflect, Renderer, Orch}
//
//   Core never imports Compiler / Serve / … (one-way).
//   Parser may import Core only.
//   Compiler may import Core + Parser.
//   Serve / Exec / … may import Core (and Compiler only via documented
//   bridges — prefer headers / C ABI over deep module imports).
//
// Cross-layer contracts:
//   - StableNodeRef-like handles crossing fibers / tenants need provenance
//     (workspace_isolation / capability_model / provenance_tracker).
//   - Dirty cascade uses DirtyPropagator; do not invent ad-hoc dirty bits.
//
// See also: docs/architecture.md

module;

#include <cstdint>
#include <string_view>

export module aura.core.module_boundary;

import std;
import aura.core.concepts;

export namespace aura::core::boundary {

// ── Inventory / phase (dashboards + tests) ─────────────────
inline constexpr int kModuleBoundaryIssue = 1885;
inline constexpr int kModuleBoundaryPhase = 1;
// Named layers in ModuleLayer (keep in sync with enum below).
inline constexpr int kModuleLayerCount = 10;

// ── Layers ─────────────────────────────────────────────────
//
// Directory mapping (primary home of each layer):
//   Core     → src/core/
//   Parser   → src/parser/
//   Compiler → src/compiler/
//   Serve    → src/serve/
//   Exec     → src/exec/
//   Repl     → src/repl/
//   Reflect  → src/reflect/
//   Renderer → src/renderer/
//   Orch     → src/orch/
//   Tui      → src/tui/  (domain vertical; still above Core)
//
// Numeric order is the *preferred* dependency rank (lower = more fundamental).
// Higher layers may depend on lower ones; reverse edges are forbidden.
enum class ModuleLayer : std::uint8_t {
    Core = 0,
    Parser = 1,
    Compiler = 2,
    Serve = 3,
    Exec = 4,
    Repl = 5,
    Reflect = 6,
    Renderer = 7,
    Orch = 8,
    Tui = 9,
};

[[nodiscard]] inline constexpr std::string_view layer_name(ModuleLayer L) noexcept {
    switch (L) {
        case ModuleLayer::Core:
            return "core";
        case ModuleLayer::Parser:
            return "parser";
        case ModuleLayer::Compiler:
            return "compiler";
        case ModuleLayer::Serve:
            return "serve";
        case ModuleLayer::Exec:
            return "exec";
        case ModuleLayer::Repl:
            return "repl";
        case ModuleLayer::Reflect:
            return "reflect";
        case ModuleLayer::Renderer:
            return "renderer";
        case ModuleLayer::Orch:
            return "orch";
        case ModuleLayer::Tui:
            return "tui";
    }
    return "unknown";
}

// ── Dependency DAG ─────────────────────────────────────────
//
// layer_may_depend_on(From, To) == true means a TU in `From` is allowed
// to `import` / `#include` APIs owned by `To`.
//
// Rules:
//   1. Same layer: always allowed (internal).
//   2. From may depend on any *strictly more fundamental* layer
//      (rank(From) > rank(To)), with two specializations:
//        - Parser → Core only (not a free-for-all of all lower ranks;
//          Parser has rank 1 so only Core is below it).
//        - Compiler → Core + Parser.
//        - Serve/Exec/Repl/… → Core always; Compiler only via bridges
//          (documented exception — see kBridgeCompilerConsumers).
//   3. Core never depends on any other layer.
//
// The rank comparison alone is the default; the table below encodes
// explicit exceptions (Compiler consumers, Parser isolation).
[[nodiscard]] inline constexpr bool layer_may_depend_on(ModuleLayer from, ModuleLayer to) noexcept {
    if (from == to)
        return true;
    if (from == ModuleLayer::Core)
        return false; // Core is the root — no outward edges.
    if (to == ModuleLayer::Core)
        return true; // Every non-Core layer may use Core.
    // Parser is Core-only (already handled Core; nothing else).
    if (from == ModuleLayer::Parser)
        return false;
    // Compiler may use Parser (and Core, above).
    if (from == ModuleLayer::Compiler)
        return to == ModuleLayer::Parser;
    // Upper layers: Core always; Compiler via bridge exception.
    // Serve/Exec/Repl/Reflect/Renderer/Orch/Tui may depend on Compiler
    // only for orchestration entry points — still allowed here so that
    // service↔fiber glue is not a permanent AC violation. Prefer thin
    // headers / C ABI when adding new edges.
    if (to == ModuleLayer::Compiler)
        return true;
    // Parser is not a general dependency for upper layers (go through Compiler).
    if (to == ModuleLayer::Parser)
        return false;
    // Same "upper" band: discourage lateral edges (Serve ↛ Exec, etc.).
    return false;
}

// Concept form for static_assert / constrained helpers.
template <ModuleLayer From, ModuleLayer To>
concept AllowedDependency = layer_may_depend_on(From, To);

// Compile-time smoke checks (always-on).
static_assert(AllowedDependency<ModuleLayer::Compiler, ModuleLayer::Core>);
static_assert(AllowedDependency<ModuleLayer::Compiler, ModuleLayer::Parser>);
static_assert(AllowedDependency<ModuleLayer::Parser, ModuleLayer::Core>);
static_assert(AllowedDependency<ModuleLayer::Serve, ModuleLayer::Core>);
static_assert(!layer_may_depend_on(ModuleLayer::Core, ModuleLayer::Compiler));
static_assert(!layer_may_depend_on(ModuleLayer::Parser, ModuleLayer::Compiler));
static_assert(!layer_may_depend_on(ModuleLayer::Serve, ModuleLayer::Parser));

// ── Key module inventory (documentation constants) ─────────
//
// Primary C++ module names per layer. Not exhaustive — high-signal entry
// points for blast-radius assessment.
inline constexpr std::string_view kCoreEntryModules[] = {
    "aura.core",          "aura.core.concepts", "aura.core.module_boundary", "aura.core.ast",
    "aura.core.mutation", "aura.core.arena",    "aura.core.error",
};
inline constexpr std::string_view kCompilerEntryModules[] = {
    "aura.compiler.service",         "aura.compiler.pass_manager",
    "aura.compiler.evaluator",       "aura.compiler.ir",
    "aura.core.concept_constraints", // Pass concepts (lives under src/core/)
};
inline constexpr std::string_view kParserEntryModules[] = {
    "aura.parser.lexer",
    "aura.parser.parser",
};

// ── Cross-layer contracts ──────────────────────────────────
//
// Handles that outlive a single mutation / cross fiber or tenant
// boundaries MUST be StableNodeRef-shaped (is_valid + id). Prefer
// attaching provenance via provenance_tracker / workspace_isolation
// rather than raw NodeId.
template <typename R, typename C>
concept CrossLayerStableRef = aura::core::StableNodeRefLike<R, C>;

// Dirty cascade across AST / IR / cache must implement DirtyPropagator
// (mark_dirty / mark_dirty_upward / is_dirty / clear_dirty).
template <typename D, typename Id = std::uint32_t>
concept CrossLayerDirtyPropagator = aura::core::DirtyPropagator<D, Id>;

// Types that participate in multi-tenant or multi-fiber isolation should
// expose a tenant (or equivalent) id for provenance checks.
template <typename T>
concept ProvenanceScoped = requires(const T& t) {
    { t.tenant_id() } -> std::convertible_to<std::uint64_t>;
};

// ── Layer-of helpers (optional tagging for future APIs) ────
//
// Specialize LayerOf<T> for types that declare their home layer.
// Default is Core (conservative for unconstrained types).
template <typename T> struct LayerOf {
    static constexpr ModuleLayer value = ModuleLayer::Core;
};

template <typename T> inline constexpr ModuleLayer layer_of_v = LayerOf<T>::value;

// ── Bridge policy note ─────────────────────────────────────
//
// Historical bridges that look like reverse edges (and why they exist):
//   - src/compiler/service.ixx includes serve/fiber.h (runtime orchestration)
//   - concept_constraints.ixx lives under src/core/ but is Compiler-ranked
//     (Pass concepts need IR); listed in AURA_CXX_MODULE_COMPILER
//   - Header-only .hh / C ABI shims are preferred for new Serve→Compiler needs
//
// When adding a new cross-layer edge: update this file + docs/architecture.md
// in the same PR (see .github/pull_request_template.md).

} // namespace aura::core::boundary
