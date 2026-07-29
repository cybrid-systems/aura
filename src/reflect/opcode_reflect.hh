// ──────────────────────────────────────────────────────────────
//  opcode_reflect.hh — P2996 reflection for IROpcode
//
//  GCC 16.1+ (-std=c++26 -freflection): auto-generate opcode name
//  tables from the enum (no hand-written switch).
//
//  Issue #2289 (real GCC 16.1.0): early-snapshot workarounds removed
//  (.data()[i], fixed-256 tables, extract avoidance). Natural APIs:
//  operator[], extract<E>, enum_count, std::define_static_array.
//
//  Still real on 16.1.0: consteval cannot produce a constexpr
//  std::string object (prefer string_view in tables). Local
//  enumerators_of() ranges are fine; for a durable compile-time
//  view use std::define_static_array / std::array — not a static
//  constexpr std::vector<meta::info>.
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
//  Dense name table: table[enum_value] = name
// ==============================================================
// Sequential 0..N-1 enums (e.g. IROpcode): extract → ordinal.
// Gaps leave empty string_view slots.

template <typename E> consteval auto build_name_table() {
    constexpr auto N = enum_count<E>();
    constexpr auto enums = std::define_static_array(std::meta::enumerators_of(^^E));
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
    constexpr auto enums = std::define_static_array(std::meta::enumerators_of(^^E));
    for (std::size_t i = 0; i < enums.size(); ++i) {
        for (std::size_t j = i + 1; j < enums.size(); ++j) {
            if (std::meta::identifier_of(enums[i]) == std::meta::identifier_of(enums[j]))
                return false;
        }
    }
    return true;
}

// ==============================================================
//  Opcode list (for CLI/--opcodes) — declaration order
// ==============================================================

template <typename E> consteval auto list_opcodes() {
    constexpr auto N = enum_count<E>();
    constexpr auto enums = std::define_static_array(std::meta::enumerators_of(^^E));
    std::array<std::string_view, N> names{};
    for (std::size_t i = 0; i < N; ++i)
        names[i] = std::meta::identifier_of(enums[i]);
    return names;
}

} // namespace aura::reflect

#endif // AURA_REFLECT_OPCODE_REFLECT_HH
