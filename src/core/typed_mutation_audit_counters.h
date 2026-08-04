// typed_mutation_audit_counters.h — Issue #2493: stub header re-exporting
// TypedMutationAuditCounters + the global g_typed_mutation_audit_counters
// for tests that #include this path. The actual struct lives in
// compiler/typed_mutation_audit.h. Tests reference the core/ path because
// they pair audit counters with other core/ headers (resource_quota,
// security_event, workspace_epoch, workspace_isolation). This header is
// consumed only by tests/ — production code should include
// compiler/typed_mutation_audit.h directly.

#ifndef AURA_CORE_TYPED_MUTATION_AUDIT_COUNTERS_H
#define AURA_CORE_TYPED_MUTATION_AUDIT_COUNTERS_H

#include "compiler/typed_mutation_audit.h"

namespace aura::core {

// Re-export at the core:: namespace level for test convenience.
// Production paths should use aura::compiler::TypedMutationAuditCounters
// and aura::compiler::typed_audit::g_typed_mutation_audit_counters
// directly from compiler/typed_mutation_audit.h.
using TypedMutationAuditCounters = ::aura::compiler::typed_audit::TypedMutationAuditCounters;

} // namespace aura::core

// Re-export the global counter instance at the core:: namespace level.
// Tests use aura::core::g_typed_mutation_audit_counters via using-declaration.
namespace aura::core::typed_audit {
inline ::aura::compiler::typed_audit::TypedMutationAuditCounters&
g_typed_mutation_audit_counters() noexcept {
    return ::aura::compiler::typed_audit::g_typed_mutation_audit_counters;
}
} // namespace aura::core::typed_audit

#endif // AURA_CORE_TYPED_MUTATION_AUDIT_COUNTERS_H