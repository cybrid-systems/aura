// Issue #2914: POD shared across query primitive peels (global module fragment).
// Do NOT forward-declare module types (Evaluator) here — that creates
// ambiguous lookup with `import aura.compiler.evaluator` under -fmodules-ts.
#pragma once

#include <cstdint>

namespace aura::compiler::primitives_detail {

struct ReflectRuntimeValidateResult {
    bool ok = false;
    bool hygiene_held = true;
    bool stale_prevented = false;
    std::uint64_t macro_markers = 0;
};

} // namespace aura::compiler::primitives_detail
