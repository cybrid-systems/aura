// C++26 Reflection (P2996) + JSON Serialization Demo
// Requires: GCC 16.1+ with -std=c++26 -freflection
//
// Build:
//   g++ -std=c++26 -freflection tests/reflect_json_demo.cpp -o build/reflect_demo
//   ./build/reflect_demo
//
// GCC 16.1 P2996 API:
//   ^^T                                  — reflect operator (double caret)
//   nonstatic_data_members_of(info, ctx) — list data members
//   identifier_of(info)                  — member name as string_view
//   offset_of(info).bytes                — byte offset in struct
//   type_of(info)                        — member type reflection
//   size_of(info)                        — type size in bytes
//   is_integral_type(info)               — type category predicate
//   is_same_type(a, b)                   — type equality
//   access_context::unchecked()          — unrestricted access

#include <meta>
#include <cstdio>
#include <string>
#include <array>
#include <string_view>
#include <cstdint>

// ── Feature detection ──────────────────────────────────────────
#ifndef __cpp_impl_reflection
#error "C++26 Reflection (P2996) not supported. Requires GCC 16.1+ with -std=c++26 -freflection"
#endif

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
    if (is_same_type(type, ^^unsigned int))      return MemberKind::UInt32;

    if (is_same_type(type, ^^long))              return MemberKind::Int64;
    if (is_same_type(type, ^^long long))         return MemberKind::Int64;
    if (is_same_type(type, ^^unsigned long))     return MemberKind::UInt64;
    if (is_same_type(type, ^^unsigned long long))return MemberKind::UInt64;
    if (is_same_type(type, ^^std::size_t))       return MemberKind::UInt64;

    // Fallback: any other integral → pick size-based
    if (is_integral_type(type)) {
        auto sz = size_of(type);
        if (sz <= 1) return MemberKind::Int8;
        if (sz <= 2) return MemberKind::Int16;
        if (sz <= 4) return MemberKind::Int32;
        return MemberKind::Int64;
    }
    return MemberKind::Unknown;
}

// Get member count as consteval — needed for array size deduction
template <typename T>
consteval std::size_t member_count() {
    return std::meta::nonstatic_data_members_of(
        ^^T, std::meta::access_context::unchecked()).size();
}

// Reflect all members into a constexpr array of MemberInfo
template <typename T>
consteval std::array<MemberInfo, member_count<T>()> reflect_members() {
    using namespace std::meta;
    constexpr auto N = member_count<T>();
    auto vec = nonstatic_data_members_of(^^T, access_context::unchecked());
    std::array<MemberInfo, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        auto m = vec[i];
        result[i] = MemberInfo{
            .name   = identifier_of(m),
            .offset = offset_of(m).bytes,
            .kind   = classify_member_type(type_of(m))
        };
    }
    return result;
}

// ── JSON escape helper ─────────────────────────────────────────
static std::string json_escape(const std::string& s) {
    std::string out;
    for (auto c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') { out += "\\n"; }
        else { out += c; }
    }
    return out;
}

// ── P2996-powered auto_to_json<T>() ────────────────────────────
//
// At compile time, reflect_members<T>() generates a constexpr array
// of {name, offset, kind} for each non-static data member.
// At runtime, we walk that array and read values via typed pointer
// arithmetic using the known offsets and kind tags.
//
// Supported types: all integral types, bool, float, double, string.
// Enums with integral underlying type are serialized as integers.
// Nested structs, vectors, and complex types serialize as "null".
//
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

// ── Demo structs ───────────────────────────────────────────────
struct Point {
    int x;
    int y;
};

struct DiagnosticMsg {
    int              kind;
    std::string      msg;
    unsigned long    node_id;
    bool             recoverable;
};

// ── Demo ───────────────────────────────────────────────────────
int main() {
    std::printf("=== C++26 Reflection (P2996) + JSON Demo ===\n");
    std::printf("  __cpp_impl_reflection = %ld\n\n", (long)__cpp_impl_reflection);

    // 1. Point
    std::printf("[1] auto_to_json<Point>:\n");
    std::printf("  %s\n\n", auto_to_json(Point{10, 20}).c_str());

    // 2. DiagnosticMsg
    std::printf("[2] auto_to_json<DiagnosticMsg>:\n");
    DiagnosticMsg d{3, "unbound variable: x", 42, true};
    std::printf("  %s\n\n", auto_to_json(d).c_str());

    // 3. With special characters
    std::printf("[3] With escaping:\n");
    DiagnosticMsg d2{5, "line1\nline2 \"quote\"", 7, false};
    std::printf("  %s\n\n", auto_to_json(d2).c_str());

    // 4. Reflection info
    std::printf("[4] Member counts (compile-time):\n");
    std::printf("  Point (%zu members)\n", member_count<Point>());
    std::printf("  DiagnosticMsg (%zu members)\n", member_count<DiagnosticMsg>());

    std::printf("\n=== Demo complete ===\n");
    return 0;
}
