// Issue #2914: POD shared across query primitive peels (global module fragment).
// Do NOT forward-declare module types (Evaluator) here — that creates
// ambiguous lookup with `import aura.compiler.evaluator` under -fmodules-ts.
#pragma once

#include <cstdint>
#include <string_view>

namespace aura::compiler::primitives_detail {

struct ReflectRuntimeValidateResult {
    bool ok = false;
    bool hygiene_held = true;
    bool stale_prevented = false;
    std::uint64_t macro_markers = 0;
};

// Issue #3175: diagnostic / low-frequency query: prims keep C++ bodies
// (and engine:metrics where they already exist) but are not registered.
// Must NOT be spelled add("…") — SlimSurface scans add() only.
template <typename... Ts> inline void sink_query_prim(std::string_view name, Ts&&...) {
    (void)name;
}

} // namespace aura::compiler::primitives_detail
