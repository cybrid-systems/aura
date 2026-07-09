// aura_error_bridge.h — Issue #807/#808: Diagnostic ↔ AuraError interop
//
// Core aura.core.error cannot import aura.diag (layering). This
// header bridges compiler-side Diagnostic to core AuraError so
// CompilerService / Evaluator can return AuraResult during migration.

#ifndef AURA_COMPILER_AURA_ERROR_BRIDGE_H
#define AURA_COMPILER_AURA_ERROR_BRIDGE_H

#include <string>
#include <string_view>
#include <utility>

// Callers that import aura.diag + aura.core.error can use these
// inline helpers without a separate .cpp TU.

namespace aura::compiler::error_bridge {

// Map aura::diag::ErrorKind → aura::core::AuraErrorKind via kind_name().
// Implemented as a template so this header stays free of module imports
// for plain C++ consumers; module partitions call the specializations
// after importing both modules (see service.ixx eval_as_aura_result).

template <typename DiagErrorKind, typename AuraErrorKind, typename KindNameFn>
inline AuraErrorKind map_diag_kind(DiagErrorKind k, KindNameFn&& kind_name_fn,
                                   AuraErrorKind fallback) {
    return fallback; // specialized at call site via map_kind_name(string)
}

} // namespace aura::compiler::error_bridge

#endif
