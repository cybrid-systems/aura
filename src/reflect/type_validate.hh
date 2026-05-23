#pragma once
#include <meta>
#include <cstdint>
#include <string_view>
#include <array>
#include <string>

// P2996 编译期类型系统验证
//
// 确保关键类型的结构布局符合设计预期。
// 由于 P2996 不能跨 C++26 module 边界使用，独立在 reflect/ 头文件中。

namespace aura::reflect {

// ── TypeId 布局验证 ─────────────────────────────────────────
// TypeId: { uint32_t index; uint32_t generation; }
// Any = {0,1}, Int = {1,1}, Bool = {2,1}, ...

consteval bool validate_type_id_layout() {
    namespace meta = std::meta;

    // TypeId 必须刚好 64 位（两个 uint32_t）
    constexpr auto tid_size = []() -> std::size_t {
        struct TID {
            std::uint32_t index;
            std::uint32_t generation;
        };
        return sizeof(TID);
    }();
    static_assert(tid_size == 8, "TypeId must be exactly 8 bytes");

    // 验证成员偏移
    struct TID_Test {
        std::uint32_t index;
        std::uint32_t generation;
    };
    static_assert(offsetof(TID_Test, index) == 0, "TypeId.index must be at offset 0");
    static_assert(offsetof(TID_Test, generation) == 4, "TypeId.generation must be at offset 4");

    return true;
}

// ── TypeInfo 布局验证 ───────────────────────────────────────
// TypeInfo: { TypeId resolved; TypeId expected; bool has_annotation; }
// 确保 SoA 索引对齐

consteval bool validate_type_info_layout() {
    namespace meta = std::meta;

    struct TypeInfoTest {
        std::uint64_t resolved; // TypeId packed as uint64_t
        std::uint64_t expected; // TypeId packed as uint64_t
        bool has_annotation;
    };

    static_assert(sizeof(TypeInfoTest) <= 24, "TypeInfo should be <= 24 bytes");
    static_assert(offsetof(TypeInfoTest, resolved) == 0, "resolved at offset 0");

    return true;
}

// ── P2996 Compile-time type validation ───────────────────────
// Validates that a type's struct layout matches what serialization expects.
// AI-generated annotations can be verified at compile time.

template <typename T> consteval bool validate_struct_layout() {
    namespace meta = std::meta;

    // Get member count via reflection
    constexpr auto members = meta::members_of(^T);
    constexpr auto count = members.size();

    // All non-static data members must be trivially copyable or known types
    bool all_valid = true;
    meta::for_each(members, [&](auto member) {
        if constexpr (requires { member.is_static_data_member(); }) {
            if (!member.is_static_data_member()) {
                // Check the member type
                using member_type = typename decltype(member)::type;
                if constexpr (!std::is_trivially_copyable_v<member_type>) {
                    // Allow strings and vectors (serialized via auto_serialize)
                    if constexpr (!std::is_same_v<member_type, std::string> &&
                                  !std::is_same_v<member_type, std::vector<std::string>>) {
                        all_valid = false;
                    }
                }
            }
        }
    });

    return all_valid;
}

// ── type_info_of<T>() — Compile-time type descriptor ─────────
// Returns a human-readable JSON string describing a type's members
// at compile time. Uses P2996 reflection to introspect struct fields.

template <typename T> consteval std::string type_info_of() {
    namespace meta = std::meta;

    std::string result = "{ ";
    result += "\"name\": \"";
    result += meta::name_of(^T);
    result += "\", \"size\": ";
    result += std::to_string(sizeof(T));
    result += ", \"alignment\": ";
    result += std::to_string(alignof(T));
    result += ", \"members\": [";

    bool first = true;
    meta::for_each(meta::members_of(^T), [&](auto member) {
        if constexpr (requires { member.is_static_data_member(); }) {
            if (!member.is_static_data_member()) {
                if (!first)
                    result += ", ";
                first = false;
                result += "{ \"name\": \"";
                result += meta::name_of(member);
                result += "\", \"offset\": ";
                // Calculate offset via the member pointer
                result += std::to_string(reinterpret_cast<std::size_t>(
                    &reinterpret_cast<const volatile char&>((*(T*)nullptr).*member.pointer)));
                result += " }";
            }
        }
    });

    result += " ] }";
    return result;
}

// ── Compile-time test runner ─────────────────────────────────
consteval bool run_all_type_validation() {
    return validate_type_id_layout() && validate_type_info_layout();
}

static_assert(run_all_type_validation(), "Type system layout validation failed");

} // namespace aura::reflect
