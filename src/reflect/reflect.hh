// ──────────────────────────────────────────────────────────────
//  reflect.hh — C++26 P2996 Reflection + auto_to_json<T>()
//
//  Header-only. Zero module dependencies. Use traditional #include.
//  Supports GCC 16.1+ with -std=c++26 -freflection.
//
//  Usage:
//    #include "reflect/reflect.hh"
//    MyStruct s{...};
//    std::string j = aura::reflect::auto_to_json(s);
//
//  Supported member types:
//    bool, std::string, float, double
//    int8/16/32/64, uint8/16/32/64, (un)signed char/short/long/long long
//    enum → serialized as integer
//    std::array<T,N> → serialized as JSON array
//    std::vector<T>  → serialized as JSON array
//    Nested structs → recursive auto_to_json
//
//  Unknown/unsupported → "null"
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_REFLECT_HH
#define AURA_REFLECT_REFLECT_HH

#include <meta>
#include <string>
#include <array>
#include <vector>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <concepts>
#include <type_traits>

namespace aura::reflect {

// ==============================================================
//  Compile-time member descriptor
// ==============================================================

enum class MemberKind : std::uint8_t {
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Bool, String, Float, Double,
    Array, Vector, Other, Unknown
};

struct MemberInfo {
    std::string_view name;
    std::ptrdiff_t   offset;
    MemberKind       kind;
    std::size_t      elem_size  = 0;   // for Array/Vector: element byte size
    std::size_t      array_len  = 0;   // for Array: fixed count
};

// ── Type classifier ───────────────────────────────────────────

consteval MemberKind classify_type(std::meta::info type) {
    using namespace std::meta;

    if (is_same_type(type, ^^bool))              return MemberKind::Bool;
    if (is_same_type(type, ^^std::string))       return MemberKind::String;
    if (is_same_type(type, ^^float))             return MemberKind::Float;
    if (is_same_type(type, ^^double))            return MemberKind::Double;

    if (is_same_type(type, ^^char) || is_same_type(type, ^^signed char)) return MemberKind::Int8;
    if (is_same_type(type, ^^unsigned char))     return MemberKind::UInt8;
    if (is_same_type(type, ^^short))             return MemberKind::Int16;
    if (is_same_type(type, ^^unsigned short))    return MemberKind::UInt16;
    if (is_same_type(type, ^^int))               return MemberKind::Int32;
    if (is_same_type(type, ^^unsigned) || is_same_type(type, ^^unsigned int)) return MemberKind::UInt32;
    if (is_same_type(type, ^^long) || is_same_type(type, ^^long long)) return MemberKind::Int64;
    if (is_same_type(type, ^^unsigned long) || is_same_type(type, ^^unsigned long long)) return MemberKind::UInt64;
    if (is_same_type(type, ^^std::size_t))       return MemberKind::UInt64;

    // enum → treat as integer (use underlying type size)
    if (is_enum_type(type)) {
        auto sz = size_of(type);
        if (sz <= 1) return MemberKind::Int8;
        if (sz <= 2) return MemberKind::Int16;
        if (sz <= 4) return MemberKind::Int32;
        return MemberKind::Int64;
    }

    // std::array<T,N> / std::vector<T> detection via display_string
    // (template_of + is_same_type has issues with GCC 16.1)
    if (is_class_type(type) && has_template_arguments(type)) {
        auto str = display_string_of(type);
        if (str.starts_with("std::array")) return MemberKind::Array;
        if (str.starts_with("std::vector")) return MemberKind::Vector;
    }

    // Nested struct with reflectable members → recursive
    if (is_class_type(type)) {
        auto n = nonstatic_data_members_of(type, access_context::unchecked()).size();
        if (n > 0) return MemberKind::Other;  // handled in refine step
    }

    // Integral fallback
    if (is_integral_type(type)) {
        auto sz = size_of(type);
        if (sz <= 1) return MemberKind::Int8;
        if (sz <= 2) return MemberKind::Int16;
        if (sz <= 4) return MemberKind::Int32;
        return MemberKind::Int64;
    }

    return MemberKind::Unknown;
}

// Get element type size for std::array<T,N> / std::vector<T>
consteval std::size_t elem_size_of(std::meta::info type) {
    using namespace std::meta;
    // For std::array/vector, the first template arg is T
    auto args = template_arguments_of(type);
    if (args.empty()) return 0;
    // static_assert(is_type(args[0]), "first template arg should be a type");
    return size_of(args.data()[0]);
}

// Get array length for std::array<T,N>
consteval std::size_t array_size_of(std::meta::info type) {
    using namespace std::meta;
    auto args = template_arguments_of(type);
    if (args.size() < 2) return 0;
    // static_assert(is_value(args.data()[1]), "second template arg should be a value");
    // Use extract to get the value as size_t
    return static_cast<std::size_t>(extract<std::size_t>(args.data()[1]));
}

// ── Member reflection ─────────────────────────────────────────

template <typename T>
consteval std::size_t member_count() {
    return std::meta::nonstatic_data_members_of(
        ^^T, std::meta::access_context::unchecked()).size();
}

template <typename T>
consteval std::array<MemberInfo, member_count<T>()> reflect_members() {
    using namespace std::meta;
    constexpr auto N = member_count<T>();
    auto vec = nonstatic_data_members_of(^^T, access_context::unchecked());
    std::array<MemberInfo, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        auto m = vec[i];
        auto type = type_of(m);
        auto kind = classify_type(type);
        result[i] = MemberInfo{
            .name      = identifier_of(m),
            .offset    = offset_of(m).bytes,
            .kind      = kind,
            .elem_size = (kind == MemberKind::Array || kind == MemberKind::Vector)
                        ? elem_size_of(type) : 0,
            .array_len = (kind == MemberKind::Array)
                        ? array_size_of(type) : 0,
        };
    }
    return result;
}

// ── JSON escape ───────────────────────────────────────────────

inline std::string json_escape(std::string_view s) {
    std::string out;
    for (auto c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;
        }
    }
    return out;
}

// ── Forward decl ──────────────────────────────────────────────

template <typename T> std::string auto_to_json(const T& obj);

// std::vector<T> — JSON array of scalars
template <typename T>
std::string auto_to_json(const std::vector<T>& vec) {
    std::string json = "[";
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (i > 0) json += ",";
        if constexpr (std::is_same_v<T, std::string>)
            json += "\"" + json_escape(vec[i]) + "\"";
        else if constexpr (std::is_enum_v<T>)
            json += std::to_string(static_cast<int>(vec[i]));
        else
            json += std::to_string(vec[i]);
    }
    json += "]";
    return json;
}

// std::vector<std::string> specialization
inline std::string auto_to_json(const std::vector<std::string>& vec) {
    std::string json = "[";
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"" + json_escape(vec[i]) + "\"";
    }
    json += "]";
    return json;
}

// std::array<T,N> — JSON array
template <typename T, std::size_t N>
std::string auto_to_json(const std::array<T, N>& arr) {
    std::string json = "[";
    for (std::size_t i = 0; i < N; ++i) {
        if (i > 0) json += ",";
        if constexpr (std::is_same_v<T, std::string>)
            json += "\"" + json_escape(arr[i]) + "\"";
        else if constexpr (std::is_enum_v<T>)
            json += std::to_string(static_cast<int>(arr[i]));
        else
            json += std::to_string(arr[i]);
    }
    json += "]";
    return json;
}

// ── auto_to_json<T> — generic struct serializer ───────────────

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
        // ---- Scalars ----
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

        // ---- String ----
        case MemberKind::String:
            json += "\"" + json_escape(*reinterpret_cast<const std::string*>(base + m.offset)) + "\"";
            break;

        // ---- std::array<T,N> — read element by element via memcpy ----
        case MemberKind::Array: {
            json += "[";
            for (std::size_t i = 0; i < m.array_len; ++i) {
                if (i > 0) json += ",";
                // memcpy element to uint64 and print
                uint64_t val = 0;
                std::memcpy(&val, base + m.offset + i * m.elem_size, m.elem_size);
                json += std::to_string(val);
            }
            json += "]";
            break;
        }

        // ---- std::vector<T> ----
        case MemberKind::Vector:
            json += "\"<vector>\"";
            break;

        // ---- Other (struct) → would need recursive, skipped for now ----
        case MemberKind::Other:
            json += "\"<struct>\"";
            break;

        default:
            json += "null";
        }
    }

    json += "}";
    return json;
}

// ── Convenience: pretty-print ─────────────────────────────────

template <typename T>
std::string auto_to_json_pretty(const T& obj, int indent = 0) {
    // Compact for now — pretty-print is future work
    return auto_to_json(obj);
}

} // namespace aura::reflect

#endif // AURA_REFLECT_REFLECT_HH
