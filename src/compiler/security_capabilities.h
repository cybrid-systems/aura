// security_capabilities.h — capability names for Issue #676 sandbox model.

#ifndef AURA_COMPILER_SECURITY_CAPABILITIES_H
#define AURA_COMPILER_SECURITY_CAPABILITIES_H

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

extern "C" std::uint64_t aura_fiber_current_id();

namespace aura::compiler::security {

inline constexpr const char* kCapWildcard = "*";
// Issue #1416: sandbox capability for kPrimSecSandboxed tier dispatch gate.
inline constexpr const char* kCapSandbox = "sandbox";
inline constexpr const char* kCapMutate = "mutate";
inline constexpr const char* kCapIo = "io";
inline constexpr const char* kCapIoRead = "io-read";
inline constexpr const char* kCapIoWrite = "io-write";
inline constexpr const char* kCapExec = "exec";

// Issues #1187/#1192 Phase 1: first-class capability effect names
// (bound to provenance / sandbox enforcement in follow-up peels).
inline constexpr const char* kCapFfi = "ffi";
inline constexpr const char* kCapNetwork = "network";
inline constexpr const char* kCapRender = "render";
inline constexpr const char* kCapTenantAdmin = "tenant-admin";

// Issue #1232 Phase 1: agent / self-evolution capability gates.
inline constexpr const char* kCapSelfEvo = "self-evo";
inline constexpr const char* kCapSynthesize = "synthesize";
inline constexpr const char* kCapStrategy = "strategy";
// Issue #2023: macro expansion self-evolution policy gate (depth/passes).
inline constexpr const char* kCapMacroSelfEvo = "macro-self-evo";

// Issues #1293/#1294/#1295 Phase 1: compile / fiber / workspace / exception
// control capability gates (retrofit scaffold for systematic coverage).
inline constexpr const char* kCapCompile = "compile";
inline constexpr const char* kCapCompileStats = "compile-stats";
inline constexpr const char* kCapCompileDirty = "compile-dirty";
inline constexpr const char* kCapCompileDeopt = "compile-deopt";
inline constexpr const char* kCapFiber = "fiber";
inline constexpr const char* kCapWorkspace = "workspace";
inline constexpr const char* kCapExceptionControl = "exception-control";
inline constexpr const char* kCapMacro = "macro";
inline constexpr const char* kCapQuery = "query";

// Issues #1325–#1330 Phase 1: architecture reduction + cap retrofit scaffold.
// Full Phase 5 gates all remaining ~50 primitives; these names land early so
// Phases 1–4 can wire deny_* helpers incrementally.
inline constexpr const char* kCapCapability = "capability"; // cap:grant / cap:revoke
inline constexpr const char* kCapAgent = "agent";           // agent:tick bridge
inline constexpr const char* kCapSysRead = "sys-read";      // raw sys-read binding
inline constexpr const char* kCapSysWrite = "sys-write";    // raw sys-write binding
inline constexpr const char* kCapSysOpen = "sys-open";      // raw sys-open binding
inline constexpr const char* kCapSyscall = "syscall";       // high-risk arbitrary syscall

// Effect bit tags (mirror aura.core.capability_model Effect enum).
inline constexpr std::uint16_t kEffectNone = 0;
inline constexpr std::uint16_t kEffectRead = 1 << 0;
inline constexpr std::uint16_t kEffectWrite = 1 << 1;
inline constexpr std::uint16_t kEffectExec = 1 << 2;
inline constexpr std::uint16_t kEffectMutate = 1 << 3;
inline constexpr std::uint16_t kEffectNetwork = 1 << 4;
inline constexpr std::uint16_t kEffectFfi = 1 << 5;
inline constexpr std::uint16_t kEffectRender = 1 << 6;
inline constexpr std::uint16_t kEffectMacroSelfEvo = 1 << 7; // Issue #2023

// Issue #2076: unified Agent-readable deny reason formatter.
// Shape: "effect-denied: <EffectName> not granted tenant=<id> op=<op>"
// Stable string literal so Agents can parse / grep / dashboard.
// Inline in the header so test TUs (and the mutate deny path in
// evaluator_primitives_mutate.cpp) can call it without a separate
// linker symbol — header-only avoids the libaura_test_objects link
// dependency on the evaluator module.
inline std::string format_deny_reason(std::uint16_t effect_bits, std::uint64_t tenant_id,
                                      std::string_view op) {
    auto name = [effect_bits]() -> const char* {
        if (effect_bits & kEffectMutate)
            return "mutate";
        if (effect_bits & kEffectFfi)
            return "ffi";
        if (effect_bits & kEffectNetwork)
            return "network";
        if (effect_bits & kEffectExec)
            return "exec";
        if (effect_bits & kEffectRender)
            return "render";
        if (effect_bits & kEffectWrite)
            return "write";
        if (effect_bits & kEffectRead)
            return "read";
        if (effect_bits & kEffectMacroSelfEvo)
            return "macro-self-evo";
        return "unknown";
    };
    return std::format("effect-denied: {} not granted tenant={} op={}", name(), tenant_id, op);
}

// Issue #2076: production default Restricted sandbox + AURA_SANDBOX env
// override. Declaration only — definition lives in evaluator_security.cpp
// (uses core/sandbox.hh + core/capability_model.hh which would make
// this header too heavy if inlined). Called from main() at startup
// (main binary links against the evaluator module so the definition
// is available) and from Evaluator::apply_env_sandbox() (evaluator
// method, no link issue).
void apply_aura_sandbox_env() noexcept;

} // namespace aura::compiler::security

#endif // AURA_COMPILER_SECURITY_CAPABILITIES_H