// security_side_effect.hh — Issue #2057 / #2152: side-effect primitives
// inherit capability / isolation enforcement by construction + dispatch.
//
// ## Rule (for contributors + Agents)
// Any new primitive that performs a real side effect (mutate / FFI /
// network / exec / file write / render / agent self-mod) MUST either:
//   1. Register via add_mutate (mutate:* family) — already enforced, OR
//   2. Call Evaluator::require_effect / check_and_record_effect at entry, OR
//   3. Set PrimMeta.required_effects so invoke_prim_with_telemetry enforces
//      require_effect automatically (effect_enforced_in_body=false), OR
//   4. Mark PrimMeta.security_exempt=true with a documented reason
//      (comment token SECURITY_EXEMPT: <reason>).
//
// PrimMeta fields (evaluator.ixx, Issue #2057):
//   - required_effects       : Effect bits (kEffectMutate / Ffi / …)
//   - effect_enforced_in_body: true when body/wrapper already checks
//   - security_exempt        : documented exempt (no side effect)
//
// Issue #2152: Primitives::add auto-stamps required_effects from the name
// when unset (infer_required_effects_from_name). Dispatch never skips a
// non-zero required_effects under Restricted/Strict unless body-enforced
// or security_exempt. Last line of defense against novel prim names that
// forget PrimMeta / add_mutate.
//
// Prefer require_effect (not bare check_and_record_effect) for new paths
// so the audit ring + capability metrics stay consistent (#2072).
//
// Gate: scripts/check_side_effect_security.py (wired into ./build.py gate)
// fails if a new effectful name is registered without coverage markers.
//
// Do NOT open nested namespace aura::compiler::security inside module
// partitions of aura.compiler.evaluator (wrong mangling).

#ifndef AURA_COMPILER_SECURITY_SIDE_EFFECT_HH
#define AURA_COMPILER_SECURITY_SIDE_EFFECT_HH

#include "security_capabilities.h"

#include <cstdint>
#include <string_view>

namespace aura::compiler {

inline constexpr int kSideEffectInheritIssue = 2057;
// Issue #2152: dispatch-level non-bypassable required_effects.
inline constexpr int kDispatchRequiredEffectsIssue = 2152;

// Infer default Effect bits from a primitive name prefix (gate + helpers).
// Issue #2136: tui:*, terminal-present*, c-render-*, c-present-batch are Render.
[[nodiscard]] inline std::uint16_t
infer_required_effects_from_name(std::string_view name) noexcept {
    using namespace security;
    if (name.starts_with("mutate:") || name.starts_with("mutate-"))
        return kEffectMutate;
    if (name.starts_with("ffi:") || name.starts_with("ffi-"))
        return kEffectFfi;
    // Issue #2625/#2626: render3d/tui/terminal present surface removed.
    // Residual render: / render- names (if any) still map to Render effect.
    if (name.starts_with("render:") || name.starts_with("render-"))
        return kEffectRender;
    if (name.starts_with("tcp-") || name.starts_with("http:") || name.starts_with("http-") ||
        name.starts_with("net:") || name.starts_with("network:"))
        return kEffectNetwork;
    if (name == "write-file" || name.starts_with("file:write") || name.starts_with("sys-write") ||
        name.starts_with("sys-open") || name.starts_with("git-"))
        return static_cast<std::uint16_t>(kEffectWrite | kEffectExec);
    if (name.starts_with("exec:") || name.starts_with("exec-") || name.starts_with("syscall") ||
        name.starts_with("sys-exec"))
        return kEffectExec;
    // Issue #2627: auto-evolve-* removed; agent: remains the self-evo surface.
    if (name.starts_with("agent:") || name.starts_with("synthesize:") ||
        name.starts_with("strategy:"))
        return kEffectMutate; // self-mod / agent surfaces treated as mutate-class
    return kEffectNone;
}

// Name looks like a side-effect primitive (for static gate + tests).
[[nodiscard]] inline bool is_side_effect_prim_name(std::string_view name) noexcept {
    return infer_required_effects_from_name(name) != security::kEffectNone;
}

// Issue #2152: effective Effect bits for dispatch / registration.
// Prefer explicit PrimMeta.required_effects; when zero and not exempt,
// fall back to name inference so prefix-matched prims cannot skip the gate.
// security_exempt → 0 (no enforce). effect_enforced_in_body is ignored here
// (caller still skips require_effect when body already checks).
[[nodiscard]] inline std::uint16_t effective_required_effects(std::string_view name,
                                                              std::uint16_t meta_required_effects,
                                                              bool security_exempt) noexcept {
    if (security_exempt)
        return security::kEffectNone;
    if (meta_required_effects != 0)
        return meta_required_effects;
    return infer_required_effects_from_name(name);
}

// Agent-searchable pattern token (also used by the static gate).
inline constexpr const char* kSideEffectPrimPatternToken = "AURA_SIDE_EFFECT_PRIM";
// Issue #2152: documented exempt reason token (gate requires this on allowlist).
inline constexpr const char* kSecurityExemptReasonToken = "SECURITY_EXEMPT:";

} // namespace aura::compiler

// Macro doc token for Agent / gate grep (does not expand to code by itself).
// Search for AURA_SIDE_EFFECT_PRIM in source when reviewing new effectful prims.
#define AURA_SIDE_EFFECT_PRIM_DOC                                                                  \
    "Issue #2057/#2152: side-effect prims must use add_mutate / require_effect / "                 \
    "PrimMeta.required_effects or document security_exempt (SECURITY_EXEMPT: reason). "            \
    "Dispatch auto-stamps + enforces name-inferred effects. See security_side_effect.hh."

#endif // AURA_COMPILER_SECURITY_SIDE_EFFECT_HH
