#pragma once

// Issue #4936: the arithmetic and numeric-comparison primitive names are a
// fixed set. The type checker used to heap-allocate them (an immortal
// unordered_set, and a leaked `new` set) so a worker thread would not
// destroy a function-local static during teardown. A constexpr array does
// not allocate.

#include <array>
#include <string_view>

namespace aura::compiler {

inline constexpr std::array<std::string_view, 4> kArithPrimNames{"+", "-", "*", "/"};
inline constexpr std::array<std::string_view, 5> kNumericCmpPrimNames{"=", "<", ">", "<=", ">="};

template <std::size_t N>
[[nodiscard]] constexpr bool prim_name_in(const std::array<std::string_view, N>& names,
                                          std::string_view name) noexcept {
    for (auto n : names) {
        if (n == name)
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool is_arith_prim_name(std::string_view name) noexcept {
    return prim_name_in(kArithPrimNames, name);
}

[[nodiscard]] constexpr bool is_numeric_cmp_prim_name(std::string_view name) noexcept {
    return prim_name_in(kNumericCmpPrimNames, name);
}

} // namespace aura::compiler
