// typed_mutation_audit_hooks.cpp — Issue #1884
// C linkage bridges so modules that cannot include typed_mutation_audit.h
// (mutex / module-import conflicts) can still stamp correlation counters.

#include "typed_mutation_audit.h"

#include <cstdint>

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
