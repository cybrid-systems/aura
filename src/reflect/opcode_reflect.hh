// ──────────────────────────────────────────────────────────────
//  opcode_reflect.hh — P2996 reflection for IROpcode
//
//  Uses GCC 16.1+ compile-time reflection to auto-generate
//  opcode name lookup, eliminating hand-written switch tables.
//
//  Cleanup (Issue #2289): re-evaluated against real GCC 16.1.0.
//  The following early-snapshot workarounds are NO LONGER needed:
//    - .data()[i] instead of operator[] on meta ranges
//    - fixed upper-bound 256 tables (use enum_count / exact size)
//    - avoiding extract<E>(enumerator) (works for sequential enums)
//
//  Residual limitations still true on GCC 16.1.0 / 16.2:
//    - std::vector<meta::info> cannot be stored as a constexpr
//      static (operator new); use local consteval ranges +
//      std::array copies instead
//    - constexpr std::string return still prefers string_view
//      (SSO / allocation in consteval remains fragile)
//  See docs/development/reflect-gcc16-cleanup.md
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_OPCODE_REFLECT_HH
#define AURA_REFLECT_OPCODE_REFLECT_HH

#include <meta>
#include <string_view>
#include <array>
#include <cstddef>
#include <cstdint>

namespace aura::reflect {

// ==============================================================
//  Count enumerators — consteval
// ==============================================================

template <typename E> consteval std::size_t enum_count() {
    return std::meta::enumerators_of(^^E).size();
}

// ==============================================================
//  Build a dense name table: table[enum_value] = name
// ==============================================================
//
// For sequential 0..N-1 enums (e.g. IROpcode), extract maps each
// enumerator to its ordinal. Gaps leave empty string_view slots.

template <typename E> consteval auto build_name_table() {
    constexpr auto N = enum_count<E>();
    auto enums = std::meta::enumerators_of(^^E);
    std::array<std::string_view, N> table{};
    for (std::size_t i = 0; i < enums.size(); ++i) {
        auto v = static_cast<std::size_t>(std::meta::extract<E>(enums[i]));
        if (v < N)
            table[v] = std::meta::identifier_of(enums[i]);
    }
    return table;
}

template <typename E> constexpr std::string_view opcode_name(int value) {
    constexpr auto table = build_name_table<E>();
    if (value >= 0 && static_cast<std::size_t>(value) < table.size())
        return table[static_cast<std::size_t>(value)];
    return "<unknown>";
}

// ==============================================================
//  Validate: no duplicate names
// ==============================================================

template <typename E> consteval bool validate_enum() {
    auto enums = std::meta::enumerators_of(^^E);
    for (std::size_t i = 0; i < enums.size(); ++i) {
        for (std::size_t j = i + 1; j < enums.size(); ++j) {
            if (std::meta::identifier_of(enums[i]) == std::meta::identifier_of(enums[j]))
                return false;
        }
    }
    return true;
}

// ==============================================================
//  Opcode list (for CLI/--opcodes)
// ==============================================================
// Returns exactly enum_count names in declaration order.

template <typename E> consteval auto list_opcodes() {
    constexpr auto N = enum_count<E>();
    auto enums = std::meta::enumerators_of(^^E);
    std::array<std::string_view, N> names{};
    for (std::size_t i = 0; i < N; ++i)
        names[i] = std::meta::identifier_of(enums[i]);
    return names;
}

} // namespace aura::reflect

#endif // AURA_REFLECT_OPCODE_REFLECT_HH
