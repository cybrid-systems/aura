// adt_runtime.ixx — ADT (datatype ...) support extracted from
// the monolithic evaluator_impl.cpp (refactor Step 2.x, following
// the exact pattern of Issue #131 FFI extraction).
//
// This module owns the ADT constructor table (previously g_adt_constructors
// global + struct in evaluator_impl). It provides a registration
// function that wires the ADT primitives/ctors into a Primitives table.
//
// The global state is now per-AdtRuntime instance (per Evaluator).
// Callers (Evaluator ctor) call register_primitives once during init.
//
// To avoid cyclic import with evaluator.ixx, we use RegisterFn callback
// (same as FFIRuntime).

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
    void register_primitives(
        RegisterFn add_primitive,
        std::pmr::vector<std::string>* string_heap = nullptr);

    // Lookup a constructor by name (used from Env::lookup etc.).
    std::optional<std::size_t> find_ctor(const std::string& name) const;

    // Accessors for tests / debug.
    std::size_t ctor_count() const { return ctors_.size(); }

private:
    std::unordered_map<std::string, AdtCtorEntry> ctors_;
};

} // namespace aura::compiler