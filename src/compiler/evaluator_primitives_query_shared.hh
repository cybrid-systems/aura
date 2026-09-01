// Issue #2914: POD shared across query primitive peels (global module fragment).
// Do NOT forward-declare module types (Evaluator) here — that creates
// ambiguous lookup with `import aura.compiler.evaluator` under -fmodules-ts.
#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace aura::compiler::primitives_detail {

// Query hash values are Aura fixnums. A leftover production harden
// must not abort engine:metrics when a counter is INT64_MIN / untagged
// bits — clamp to the representable range (kFixnumShift==1).
[[nodiscard]] inline std::int64_t saturate_query_fixnum(std::int64_t v) noexcept {
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min() / 2;
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max() / 2;
    if (v < kMin)
        return kMin;
    if (v > kMax)
        return kMax;
    return v;
}

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
