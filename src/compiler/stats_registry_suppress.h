// compiler/stats_registry_suppress.h — Issue #4887
//
// The stats impl table (register_stats_impl) is process-global, but every entry
// captures the Evaluator that registered it. A thread-private CompilerService
// (offload worker / snapshot) is destroyed when its job ends, so letting it
// republish the table would leave the shared entries pointing at a dead
// Evaluator. Private-service construction sets this flag to keep the session's
// registrations.
#ifndef AURA_COMPILER_STATS_REGISTRY_SUPPRESS_H
#define AURA_COMPILER_STATS_REGISTRY_SUPPRESS_H

namespace aura::compiler {

// Returns the previous value so callers can restore it around a scope.
bool set_stats_registration_suppressed(bool suppress);
[[nodiscard]] bool stats_registration_suppressed();

} // namespace aura::compiler

#endif // AURA_COMPILER_STATS_REGISTRY_SUPPRESS_H
