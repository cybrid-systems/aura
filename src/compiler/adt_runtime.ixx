// adt_runtime.ixx — ADT (datatype ...) support extracted from
// the monolithic evaluator TU (refactor Step 2.x, following
// the exact pattern of Issue #131 FFI extraction).
//
// This module owns the ADT constructor table (previously g_adt_constructors
// global + struct in evaluator). It provides a registration
// function that wires the ADT primitives/ctors into a Primitives table.
//
// The global state is now per-AdtRuntime instance (per Evaluator).
// Callers (Evaluator ctor) call register_primitives once during init.
//
// To avoid cyclic import with evaluator.ixx, we use RegisterFn callback
// (same as FFIRuntime).

module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

export module aura.compiler.adt_runtime;

import std;
import aura.compiler.value;

namespace aura::compiler {

// Mirror of PrimFn (to break cycle).
using PrimFn = std::function<types::EvalValue(std::span<const types::EvalValue>)>;

// Entry for one ADT constructor (name, arity, etc.).
// (Adapted from the previous global AdtCtorEntry.)
struct AdtCtorEntry {
    std::string name;
    int arity = 0;
    // Future: more (type params, etc.)
};

// Per-Evaluator ADT runtime (replaces the old global table + registration logic).
export class AdtRuntime {
public:
    using RegisterFn = std::function<void(std::string, PrimFn)>;

    // Register the ADT-related primitives/ctors via the callback.
    // Wiring mechanism complete (Step 2.3 FFI pattern); population still triggered
    // from parser side via the registered primitive. Full logic move follow-on.
    // 5.2 hygiene: signature now matches ffi exactly (string_heap + opaque + coverage counters)
    // for consistency; last two nullable for tests / partial wiring.
    void register_primitives(RegisterFn add_primitive,
                             std::pmr::vector<std::string>* string_heap = nullptr,
                             std::vector<void*>* opaque_heap = nullptr,
                             std::array<std::uint64_t, 16>* coverage_counters = nullptr);

    // Register a dynamically-defined multi-arg constructor (from define-type
    // eval). The PrimFn body is built by the Evaluator module (which owns
    // pairs_); this method wires it into the primitives table and records
    // the slot for Env::lookup fallback via find_ctor.
    void register_dynamic_ctor(RegisterFn add_primitive, const std::string& name, PrimFn body,
                               int arity, std::size_t slot);

    // Lookup a constructor by name (used from Env::lookup etc.).
    std::optional<std::size_t> find_ctor(const std::string& name) const;

    // Accessors for tests / debug.
    std::size_t ctor_count() const { return ctors_.size(); }

    // Issue #994: drop dynamic ctor maps (hot-update / self-evo churn).
    void clear_dynamic_ctors() {
        ctors_.clear();
        ctor_slots_.clear();
    }
    // Soft cap: if over max, clear all (dynamic ctors are rebuildable).
    void maybe_cap_dynamic_ctors(std::size_t max_entries = 4096) {
        if (ctors_.size() > max_entries)
            clear_dynamic_ctors();
    }

private:
    std::unordered_map<std::string, AdtCtorEntry> ctors_;
    std::unordered_map<std::string, std::size_t> ctor_slots_;
};

} // namespace aura::compiler
