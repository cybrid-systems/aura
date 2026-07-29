// ──────────────────────────────────────────────────────────────
//  enum_name_table.hh — Wave C1 generic P2996 enum name tables
//
//  Header-only, -freflection required. Prefer string_view tables
//  (no consteval std::string). Sequential 0..N-1 enums use the
//  dense helpers; sparse / gapped enums use extent = max+1.
//
//  opcode_reflect.hh re-exports the dense API under historical
//  names (opcode_name, list_opcodes, …).
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_ENUM_NAME_TABLE_HH
#define AURA_REFLECT_ENUM_NAME_TABLE_HH

#include <meta>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace aura::reflect {

// ── Count / validate ──────────────────────────────────────────

template <typename E>
    requires std::is_enum_v<E>
consteval std::size_t enum_count() {
    return std::meta::enumerators_of(^^E).size();
}

template <typename E>
    requires std::is_enum_v<E>
consteval bool validate_enum() {
    constexpr auto enums = std::define_static_array(std::meta::enumerators_of(^^E));
    for (std::size_t i = 0; i < enums.size(); ++i) {
        for (std::size_t j = i + 1; j < enums.size(); ++j) {
            if (std::meta::identifier_of(enums[i]) == std::meta::identifier_of(enums[j]))
                return false;
        }
    }
    return true;
}

// Max underlying value among enumerators (for sparse tables).
template <typename E>
    requires std::is_enum_v<E>
consteval std::size_t enum_max_value() {
    constexpr auto enums = std::define_static_array(std::meta::enumerators_of(^^E));
    std::size_t m = 0;
    for (std::size_t i = 0; i < enums.size(); ++i) {
        auto v = static_cast<std::size_t>(std::meta::extract<E>(enums[i]));
        if (v > m)
            m = v;
    }
    return m;
}

// ── Declaration-order list ────────────────────────────────────

template <typename E>
    requires std::is_enum_v<E>
consteval auto list_enumerators() {
    constexpr auto N = enum_count<E>();
    constexpr auto enums = std::define_static_array(std::meta::enumerators_of(^^E));
    std::array<std::string_view, N> names{};
    for (std::size_t i = 0; i < N; ++i)
        names[i] = std::meta::identifier_of(enums[i]);
    return names;
}

// ── Dense table: table[enum_value] for sequential 0..N-1 ──────
// Gaps (missing values) leave empty string_view slots.
// If any enumerator has value >= N, that name is not stored
// (use build_name_table_extent instead).

template <typename E>
    requires std::is_enum_v<E>
consteval auto build_name_table() {
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

// ── Extent table: table[enum_value] for values in [0, Max] ────
// Default Max = enum_max_value(). Size = Max+1.

template <typename E, std::size_t Extent = enum_max_value<E>() + 1>
    requires std::is_enum_v<E>
consteval auto build_name_table_extent() {
    constexpr auto enums = std::define_static_array(std::meta::enumerators_of(^^E));
    std::array<std::string_view, Extent> table{};
    for (std::size_t i = 0; i < enums.size(); ++i) {
        auto v = static_cast<std::size_t>(std::meta::extract<E>(enums[i]));
        if (v < Extent)
            table[v] = std::meta::identifier_of(enums[i]);
    }
    return table;
}

// ── Lookup by underlying integer / enum value ─────────────────

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view enum_name(int value) {
    // Prefer extent table so sparse sequential-from-zero enums work.
    constexpr auto table = build_name_table_extent<E>();
    if (value >= 0 && static_cast<std::size_t>(value) < table.size()) {
        auto sv = table[static_cast<std::size_t>(value)];
        if (!sv.empty())
            return sv;
    }
    return "<unknown>";
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view enum_name(E value) {
    return enum_name<E>(static_cast<int>(value));
}

// Declaration-order name (index in enumerators_of order, not value).
template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view enum_name_at(std::size_t declaration_index) {
    constexpr auto names = list_enumerators<E>();
    if (declaration_index < names.size())
        return names[declaration_index];
    return "<unknown>";
}

// ── Dense-only convenience (0..count-1, empty if hole) ────────

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view enum_name_dense(int value) {
    constexpr auto table = build_name_table<E>();
    if (value >= 0 && static_cast<std::size_t>(value) < table.size()) {
        auto sv = table[static_cast<std::size_t>(value)];
        if (!sv.empty())
            return sv;
    }
    return "<unknown>";
}

} // namespace aura::reflect

#endif // AURA_REFLECT_ENUM_NAME_TABLE_HH
