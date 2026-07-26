// security_side_effect.hh — Issue #2057: side-effect primitives inherit
// capability / isolation enforcement by construction.
//
// ## Rule (for contributors + Agents)
// Any new primitive that performs a real side effect (mutate / FFI /
// network / exec / file write / render / agent self-mod) MUST either:
//   1. Register via add_mutate (mutate:* family) — already enforced, OR
//   2. Call Evaluator::require_effect / check_and_record_effect at entry, OR
//   3. Set PrimMeta.required_effects so invoke_prim_with_telemetry enforces
//      require_effect automatically (effect_enforced_in_body=false), OR
//   4. Mark PrimMeta.security_exempt=true with a documented reason.
//
// PrimMeta fields (evaluator.ixx, Issue #2057):
//   - required_effects       : Effect bits (kEffectMutate / Ffi / …)
//   - effect_enforced_in_body: true when body/wrapper already checks
//   - security_exempt        : documented exempt (no side effect)
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

// Infer default Effect bits from a primitive name prefix (gate + helpers).
// Issue #2136: tui:*, terminal-present*, c-render-*, c-present-batch are Render.
[[nodiscard]] inline std::uint16_t
infer_required_effects_from_name(std::string_view name) noexcept {
    using namespace security;
    if (name.starts_with("mutate:") || name.starts_with("mutate-"))
        return kEffectMutate;
    if (name.starts_with("ffi:") || name.starts_with("ffi-"))
        return kEffectFfi;
    if (name.starts_with("render3d:") || name.starts_with("render:") ||
        name.starts_with("render-") || name.starts_with("tui:") ||
        name.starts_with("terminal-present") || name.starts_with("c-render-") ||
        name == "c-present-batch" || name == "c-ansi-emit" ||
        name.starts_with("make-terminal-buffer") || name.starts_with("terminal-set-cell") ||
        name.starts_with("terminal-draw") || name.starts_with("terminal-mark"))
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
    if (name.starts_with("agent:") || name.starts_with("auto-evolve") ||
        name.starts_with("synthesize:") || name.starts_with("strategy:"))
        return kEffectMutate; // self-mod / agent surfaces treated as mutate-class
    return kEffectNone;
}

// Name looks like a side-effect primitive (for static gate + tests).
[[nodiscard]] inline bool is_side_effect_prim_name(std::string_view name) noexcept {
    return infer_required_effects_from_name(name) != security::kEffectNone;
}

// Agent-searchable pattern token (also used by the static gate).
inline constexpr const char* kSideEffectPrimPatternToken = "AURA_SIDE_EFFECT_PRIM";

} // namespace aura::compiler

// Macro doc token for Agent / gate grep (does not expand to code by itself).
// Search for AURA_SIDE_EFFECT_PRIM in source when reviewing new effectful prims.
#define AURA_SIDE_EFFECT_PRIM_DOC                                                                  \
    "Issue #2057: side-effect prims must use add_mutate / require_effect / "                       \
    "PrimMeta.required_effects or document security_exempt. See security_side_effect.hh."

#endif // AURA_COMPILER_SECURITY_SIDE_EFFECT_HH
