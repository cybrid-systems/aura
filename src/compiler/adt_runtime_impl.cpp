// adt_runtime_impl.cpp — Implementation of the ADT runtime
// aura.compiler.evaluator module partition (refactor Step 2.x).
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
#include <optional>
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
    // Static adt:* primitives (if any) register here. Dynamic define-type
    // constructors register via register_dynamic_ctor during eval_flat.
    (void)add;
    (void)string_heap;
    (void)opaque_heap;
    (void)coverage_counters;
}

void AdtRuntime::register_dynamic_ctor(RegisterFn add, const std::string& name, PrimFn body,
                                       int arity, std::size_t slot) {
    add(name, std::move(body));
    ctors_[name] = AdtCtorEntry{.name = name, .arity = arity};
    ctor_slots_[name] = slot;
}

std::optional<std::size_t> AdtRuntime::find_ctor(const std::string& name) const {
    auto it = ctor_slots_.find(name);
    if (it == ctor_slots_.end())
        return std::nullopt;
    return it->second;
}

} // namespace aura::compiler
