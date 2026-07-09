// prim_names.h — Issue #904 Phase 1: canonical primitive-name string constants.
//
// Prefer these over bare "set-code" / "eval-current" literals at registration
// and call sites. Full call-site migration is incremental.
#ifndef AURA_COMPILER_PRIM_NAMES_H
#define AURA_COMPILER_PRIM_NAMES_H

namespace aura::compiler::prim {

inline constexpr const char* kSetCode = "set-code";
inline constexpr const char* kEvalCurrent = "eval-current";
inline constexpr const char* kEvalCurrentOutput = "eval-current-output";
inline constexpr const char* kCurrentSource = "current-source";
inline constexpr const char* kApiReference = "api-reference";

// Common query / mutate prefixes (for documentation / prefix checks).
inline constexpr const char* kQueryPrefix = "query:";
inline constexpr const char* kMutatePrefix = "mutate:";

} // namespace aura::compiler::prim

#endif
