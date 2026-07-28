// typed_mutation_audit_hooks.cpp — Issue #1884
// C linkage bridges so modules that cannot include typed_mutation_audit.h
// (mutex / module-import conflicts) can still stamp correlation counters.
//
// Also owns Issue #2262 free process atomics for hard-empty-miss / wired
// (import_total/skip live on aura.compiler.type_checker — see type_checker.ixx).

#include "typed_mutation_audit.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>

namespace aura::compiler {
// Issue #2262: free process storage (evaluator bumps hard_empty_miss;
// query reads wired). Not module-attached.
std::atomic<std::uint64_t> g_partial_cs_hard_empty_miss_total{0};
std::atomic<std::uint32_t> g_partial_cs_single_source_wired{1};
} // namespace aura::compiler

extern "C" void aura_typed_audit_note_predicate_memo_eviction(std::uint64_t n) {
    aura::compiler::typed_audit::note_predicate_memo_eviction(n);
}

extern "C" void aura_typed_audit_note_type_propagation_pass(std::uint64_t fixpoint_rounds,
                                                            std::uint64_t narrow_hits,
                                                            std::uint64_t extended_ops) {
    aura::compiler::typed_audit::note_type_propagation_pass(fixpoint_rounds, narrow_hits,
                                                            extended_ops);
}

extern "C" void aura_typed_audit_note_dce_narrow_hits(std::uint64_t narrow_hits) {
    aura::compiler::typed_audit::note_dce_narrow_hits(narrow_hits);
}
