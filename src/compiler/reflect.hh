// C++26 Reflection (P2996) — auto_to_json<T>() utility
//
// Header-only. Include in any .cpp file that needs reflection.
// The template functions use GCC 16.1 P2996 reflection API.
//
// Supported types: bool, std::string, float, double, all integral types.
// Enums with unsigned char underlying type are serialized as integers.
// Nested structs, vectors, and complex types serialize as "null".
//
// Usage:
//   #include "reflect.hh"
//   Diagnostic d{...};
//   std::string json = auto_to_json(d);
//
// Requires: -std=c++26 -freflection

#ifndef AURA_REFLECT_HH
#define AURA_REFLECT_HH

#include <meta>
#include <string>
#include <array>
#include <string_view>
#include <cstddef>

namespace aura::reflect {

// ── Compile-time member descriptor ────────────────────────────
enum class MemberKind : std::uint8_t {
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Bool, String, Float, Double, Unknown
};

struct MemberInfo {
    std::string_view name;
    std::ptrdiff_t   offset;
    MemberKind       kind;
};

// Classify a reflected type to a MemberKind
consteval MemberKind classify_member_type(std::meta::info type) {
    using namespace std::meta;

    if (is_same_type(type, ^^bool))              return MemberKind::Bool;
    if (is_same_type(type, ^^std::string))       return MemberKind::String;
    if (is_same_type(type, ^^float))             return MemberKind::Float;
    if (is_same_type(type, ^^double))            return MemberKind::Double;

    if (is_same_type(type, ^^char))              return MemberKind::Int8;
    if (is_same_type(type, ^^signed char))       return MemberKind::Int8;
    if (is_same_type(type, ^^unsigned char))     return MemberKind::UInt8;
    if (is_same_type(type, ^^short))             return MemberKind::Int16;
    if (is_same_type(type, ^^unsigned short))    return MemberKind::UInt16;

    if (is_same_type(type, ^^int))               return MemberKind::Int32;
    if (is_same_type(type, ^^unsigned))          return MemberKind::UInt32;

    if (is_same_type(type, ^^long))              return MemberKind::Int64;
    if (is_same_type(type, ^^long long))         return MemberKind::Int64;
    if (is_same_type(type, ^^unsigned long))     return MemberKind::UInt64;
    if (is_same_type(type, ^^unsigned long long))return MemberKind::UInt64;
    if (is_same_type(type, ^^std::size_t))       return MemberKind::UInt64;

    if (is_integral_type(type)) {
        auto sz = size_of(type);
        if (sz <= 1) return MemberKind::Int8;
        if (sz <= 2) return MemberKind::Int16;
        if (sz <= 4) return MemberKind::Int32;
        return MemberKind::Int64;
    }
    return MemberKind::Unknown;
}

template <typename T>
consteval std::size_t member_count() {
    return std::meta::nonstatic_data_members_of(
        ^^T, std::meta::access_context::unchecked()).size();
}

template <typename T>
consteval std::array<MemberInfo, member_count<T>()> reflect_members() {
    constexpr auto N = member_count<T>();
    auto vec = std::meta::nonstatic_data_members_of(
        ^^T, std::meta::access_context::unchecked());
    std::array<MemberInfo, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        auto m = vec[i];
        result[i] = MemberInfo{
            .name   = std::meta::identifier_of(m),
            .offset = std::meta::offset_of(m).bytes,
            .kind   = classify_member_type(std::meta::type_of(m))
        };
    }
    return result;
}

// ── JSON escape helper ─────────────────────────────────────────
inline std::string json_escape(std::string_view s) {
    std::string out;
    for (auto c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') { out += "\\n"; }
        else { out += c; }
    }
    return out;
}

// ── P2996-powered auto_to_json<T>() ────────────────────────────
template <typename T>
std::string auto_to_json(const T& obj) {
    constexpr auto members = reflect_members<T>();
    std::string json = "{";
    bool first = true;
    const auto* base = reinterpret_cast<const char*>(&obj);

    for (auto& m : members) {
        if (!first) json += ",";
        first = false;

        json += "\"";
        json += m.name;
        json += "\":";

        switch (m.kind) {
        case MemberKind::Int8:
            json += std::to_string(static_cast<int>(*reinterpret_cast<const unsigned char*>(base + m.offset)));
            break;
        case MemberKind::Int16:
            json += std::to_string(*reinterpret_cast<const short*>(base + m.offset));
            break;
        case MemberKind::Int32:
            json += std::to_string(*reinterpret_cast<const int*>(base + m.offset));
            break;
        case MemberKind::Int64:
            json += std::to_string(*reinterpret_cast<const long long*>(base + m.offset));
            break;
        case MemberKind::UInt8:
            json += std::to_string(*reinterpret_cast<const unsigned char*>(base + m.offset));
            break;
        case MemberKind::UInt16:
            json += std::to_string(*reinterpret_cast<const unsigned short*>(base + m.offset));
            break;
        case MemberKind::UInt32:
            json += std::to_string(*reinterpret_cast<const unsigned*>(base + m.offset));
            break;
        case MemberKind::UInt64:
            json += std::to_string(*reinterpret_cast<const unsigned long long*>(base + m.offset));
            break;
        case MemberKind::Bool:
            json += (*reinterpret_cast<const bool*>(base + m.offset)) ? "true" : "false";
            break;
        case MemberKind::Float:
            json += std::to_string(*reinterpret_cast<const float*>(base + m.offset));
            break;
        case MemberKind::Double:
            json += std::to_string(*reinterpret_cast<const double*>(base + m.offset));
            break;
        case MemberKind::String:
            json += "\"" + json_escape(*reinterpret_cast<const std::string*>(base + m.offset)) + "\"";
            break;
        default:
            json += "null";
        }
    }

    json += "}";
    return json;
}

} // namespace aura::reflect

#endif // AURA_REFLECT_HH
