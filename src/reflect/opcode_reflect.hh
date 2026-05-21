// ──────────────────────────────────────────────────────────────
//  opcode_reflect.hh — P2996 reflection for IROpcode
//
//  Uses GCC 16.1 compile-time reflection to auto-generate
//  opcode name lookup, eliminating hand-written switch(24 cases).
//
//  Key constraints (GCC 16.1 P2996):
//    - vector<info> can't be constexpr-stored (operator new)
//    - Use data()[i] instead of operator[]
//    - extract<T>(enumerator) throws for enum values
//    - constexpr string return has SSO issues
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_OPCODE_REFLECT_HH
#define AURA_REFLECT_OPCODE_REFLECT_HH

#include <meta>
#include <string>
#include <string_view>
#include <array>
#include <cstdint>

namespace aura::reflect {

// ==============================================================
//  Count enumerators — consteval
// ==============================================================

template <typename E>
consteval std::size_t enum_count() {
    auto enums = std::meta::enumerators_of(^^E);
    return enums.size();
}

// ==============================================================
//  Build a dense name table: table[enum_value] = name
// ==============================================================

// For IROpcode, values are sequential 0..23.
// Extract enumerator names into a fixed array by INDEX (not by value).
// Use data()[i] to avoid vector operator[] constexpr issues.
template <typename E, std::size_t N>
consteval auto build_name_table() {
    auto enums = std::meta::enumerators_of(^^E);
    std::array<std::string_view, N> table{};
    for (std::size_t i = 0; i < N; ++i) {
        if (i < enums.size())
            table[i] = std::meta::identifier_of(enums.data()[i]);
    }
    return table;
}

template <typename E>
constexpr std::string_view opcode_name(int value) {
    constexpr auto N = enum_count<E>();
    constexpr auto table = build_name_table<E, 32>();  // generous upper bound
    if (value >= 0 && value < static_cast<int>(N))
        return table[value];
    return "<unknown>";
}

// ==============================================================
//  Validate: no duplicate names
// ==============================================================

template <typename E>
consteval bool validate_enum() {
    auto enums = std::meta::enumerators_of(^^E);
    for (std::size_t i = 0; i < enums.size(); ++i) {
        for (std::size_t j = i + 1; j < enums.size(); ++j) {
            if (std::meta::identifier_of(enums.data()[i]) ==
                std::meta::identifier_of(enums.data()[j]))
                return false;
        }
    }
    return true;
}

// ==============================================================
//  Opcode list (for CLI/--opcodes)
// ==============================================================

template <typename E>
consteval auto list_opcodes() {
    constexpr auto N = enum_count<E>();
    auto enums = std::meta::enumerators_of(^^E);
    std::array<std::string_view, 32> names{};
    for (std::size_t i = 0; i < N; ++i)
        names[i] = std::meta::identifier_of(enums.data()[i]);
    return std::pair{names, N};
}

} // namespace aura::reflect

#endif // AURA_REFLECT_OPCODE_REFLECT_HH
