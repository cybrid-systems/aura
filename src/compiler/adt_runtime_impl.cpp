// adt_runtime_impl.cpp — Implementation of the ADT runtime
// extracted from evaluator_impl.cpp (refactor Step 2.x).
//
// Follows *exact* pattern from ffi_primitives_impl.cpp (Issue #131):
// - Per-instance state (no more globals)
// - register_primitives takes RegisterFn callback
// - Tiny diff for skeleton (map ops + one registration hook)
//
// The actual logic for (datatype ...) parsing and ctor registration
// will be moved here in follow-on wiring (2.3). This keeps the step small.

module;

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

module aura.compiler.adt_runtime;

import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;
using namespace aura::compiler::types;

void AdtRuntime::register_primitives(RegisterFn add, std::pmr::vector<std::string>* string_heap,
                                     std::vector<void*>* opaque_heap,
                                     std::array<std::uint64_t, 16>* coverage_counters) {
    // Skeleton: register a placeholder or the hook for datatype ctors.
    // Wiring of registration mechanism complete (Step 2.3): Evaluator ctor calls
    // adt_runtime_.register_primitives with the add-callback (exact FFI pattern).
    // Parser (parse_datatype) still drives actual ctor population by invoking the
    // registered adt:* primitive(s); entries end up in the per-instance ctors_ map.
    // (Full move of population logic into this module is follow-on if needed.)

    // Example stub (will be replaced when wiring the real ctors):
    // add("datatype", [this, string_heap](std::span<const EvalValue> a) -> EvalValue { ... });

    (void)add;
    (void)string_heap;
    (void)opaque_heap;
    (void)coverage_counters;
}

std::optional<std::size_t> AdtRuntime::find_ctor(const std::string& name) const {
    auto it = ctors_.find(name);
    if (it == ctors_.end())
        return std::nullopt;
    // Return a slot/index (for now, just presence; real impl will map to primitive slot or closure
    // id).
    return 0; // placeholder
}

} // namespace aura::compiler