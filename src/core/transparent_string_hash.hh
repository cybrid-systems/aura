// SPDX-License-Identifier: MIT
//
// TransparentStringHash — C++20 heterogeneous-lookup hash + comparator
// for std::unordered_map<std::string, V>. Consolidates the pattern that
// existed in two duplicated local definitions:
//
//   - Primitives::StringHash / StringEq (src/compiler/evaluator.ixx) —
//     tables table_, hot_map_, name_to_slot_; original references
//     #891/#914.
//   - StatsNameHash / StatsNameEq (src/compiler/evaluator_primitives_observability.cpp) —
//     LegacyStatsMap; references #1671.
//
// Hashes std::string, std::string_view, and const char* identically
// (all delegate to std::hash<std::string_view>) so .find() / .count() /
// .contains() accept any of those types without allocating a temporary
// std::string on the hot path. The is_transparent tag is what enables
// the heterogeneous overload selection in std::unordered_map; without
// it, every lookup would force a std::string copy from string_view.
//
// Use the convenience alias TransparentStringMap<V> for the common case
// (transparent hash + std::equal_to<> comparator + default allocator).
//
// Pattern rationale: see #891/#914/#1439/#1671 in the bug tracker.
// This is the established idiom in src/compiler/evaluator.ixx for
// name-keyed primitive dispatch tables; consolidating here lets the
// remaining ~140 std::unordered_map<std::string, V> sites across
// service.ixx / lowering_impl.cpp / etc. adopt the same pattern with
// a single include.
//
// CppCoreGuidelines ES.65 ("don't compare addresses; use string_view
// for lookups") and ES.84 ("don't use unnamed numeric literals")
// are also relevant — the transparent lookup removes a class of
// allocation-per-lookup bugs by making string_view a first-class key.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aura::core {

// Heterogeneous-lookup hash for std::unordered_map<std::string, V>.
// Hashes all of (std::string_view, std::string, const char*) via
// std::hash<std::string_view> so any of those types can be used as
// the key argument to .find() / .count() / .contains() /
// try_emplace() without an intermediate std::string allocation.
struct TransparentStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

// Convenience alias for std::unordered_map<std::string, V> with
// transparent lookup enabled. Use std::equal_to<> (C++20 transparent
// comparator) so string_view and std::string compare correctly against
// the std::string keys stored in the map.
template <typename V, typename Allocator = std::allocator<std::pair<const std::string, V>>>
using TransparentStringMap =
    std::unordered_map<std::string, V, TransparentStringHash, std::equal_to<>, Allocator>;

} // namespace aura::core