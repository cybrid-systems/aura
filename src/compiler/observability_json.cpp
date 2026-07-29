// observability_json.cpp — JSON for CompilerSnapshot / FnMetrics
//
// Wave A1: use P2996 to_json (aura-reflect / -freflection).
// Non-import-std TU — safe with <meta> on g++ 16.1.0 (#2290).
// Schema is the full public POD layout (all snapshot fields), not
// the historical hand-rolled subset.

#include "compiler/observability_snapshot.h"
#include "reflect/reflect.hh"

#include <string>

namespace aura::compiler {

std::string fn_metrics_to_json(const FnMetrics& f) {
    return aura::reflect::to_json(f);
}

std::string snapshot_to_json(const CompilerSnapshot& s) {
    return aura::reflect::to_json(s);
}

} // namespace aura::compiler
