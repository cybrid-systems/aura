// ──────────────────────────────────────────────────────────────
//  reflect_schema.hh — Compile-time JSON Schema generation
//
//  Uses P2996 reflection + P1306 expansion statements to generate
//  JSON Schema (draft 2020-12) at compile time.
//
//  The schema is stored as a static constexpr char array.
//  At runtime, wrap it in std::string_view for consumption.
//
//  Usage:
//    #include "reflect/reflect_schema.hh"
//    // Schema is available as:
//    aura::reflect::schema_storage<MyStruct>::value
//    // which is a const char* null-terminated string
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_SCHEMA_HH
#define AURA_REFLECT_SCHEMA_HH

#include <meta>
#include <string>
#include <array>
#include <string_view>
#include <cstddef>
#include <cstdint>

namespace aura::reflect {

// ── Type name mapper ──────────────────────────────────────────

consteval std::string_view type_name_for_schema(std::meta::info type) {
    using namespace std::meta;

    if (is_same_type(type, ^^bool))              return "boolean";
    if (is_same_type(type, ^^std::string))       return "string";
    if (is_same_type(type, ^^float))             return "number";
    if (is_same_type(type, ^^double))            return "number";

    if (is_same_type(type, ^^int8_t))            return "integer";
    if (is_same_type(type, ^^uint8_t))           return "integer";
    if (is_same_type(type, ^^char))              return "integer";
    if (is_same_type(type, ^^signed char))       return "integer";
    if (is_same_type(type, ^^unsigned char))     return "integer";
    if (is_same_type(type, ^^short))             return "integer";
    if (is_same_type(type, ^^unsigned short))    return "integer";
    if (is_same_type(type, ^^int))               return "integer";
    if (is_same_type(type, ^^unsigned))          return "integer";
    if (is_same_type(type, ^^unsigned int))      return "integer";
    if (is_same_type(type, ^^long))              return "integer";
    if (is_same_type(type, ^^unsigned long))     return "integer";
    if (is_same_type(type, ^^long long))         return "integer";
    if (is_same_type(type, ^^unsigned long long))return "integer";
    if (is_same_type(type, ^^std::size_t))       return "integer";

    // std::array<T,N> → array
    if (is_class_type(type) && has_template_arguments(type)) {
        auto str = display_string_of(type);
        if (str.starts_with("std::array"))  return "array";
        if (str.starts_with("std::vector")) return "array";
    }

    // Enum → integer
    if (is_enum_type(type))                    return "integer";

    // Class with members → object
    if (is_class_type(type)) {
        auto n = nonstatic_data_members_of(type, access_context::unchecked()).size();
        if (n > 0) return "object";
    }

    return "unknown";
}

// ── Helper: max buffer size ──────────────────────────────────
// Conservative upper bound: 256 bytes per member + overhead

template <typename T>
consteval std::size_t schema_buffer_size() {
    return 32 * 256 + 512;  // generous upper bound
}

// ── Schema generator: uses template for (P1306) ──────────────
// Builds a JSON Schema into a fixed-size char buffer.

template <typename T>
struct schema_storage {
    static constexpr std::size_t BUF = schema_buffer_size<T>();
    // We use a static member array — this is the "constexpr output" trick
    static constexpr std::array<char, BUF> build() {
        using namespace std::meta;
        
        constexpr size_t N = []() {
            return nonstatic_data_members_of(
                ^^T, access_context::unchecked()).size();
        }();
        
        static constexpr auto members = []() {
            std::array<info, N> arr{};
            auto vec = nonstatic_data_members_of(^^T, access_context::unchecked());
            for (size_t i = 0; i < N; ++i) arr[i] = vec[i];
            return arr;
        }();
        
        std::array<char, BUF> buf{};
        std::size_t pos = 0;
        
        // Helper to append a string_view to the buffer
        auto append = [&](std::string_view s) {
            for (auto c : s) {
                if (pos < BUF) buf[pos++] = c;
            }
        };
        
        // Helper to append an integer
        auto append_int = [&](size_t val) {
            if (val == 0) { append("0"); return; }
            char tmp[32];
            int len = 0;
            while (val > 0) {
                tmp[len++] = '0' + (val % 10);
                val /= 10;
            }
            for (int i = len - 1; i >= 0; --i)
                buf[pos++] = tmp[i];
        };
        
        // Build JSON Schema
        append("{\n  \"$schema\": \"https://json-schema.org/draft/2020-12/schema\",\n");
        append("  \"title\": \"");
        append(display_string_of(^^T));
        append("\",\n  \"type\": \"object\",\n  \"properties\": {\n");
        
        template for (constexpr auto m : members) {
            auto name = identifier_of(m);
            auto type = type_of(m);
            auto ts = type_name_for_schema(type);
            
            // Indent & field name
            append("    \"");
            append(name);
            append("\": {\n      \"type\": \"");
            append(ts);
            
            // Additional info for integers
            if (ts == "integer") {
                append("\",\n      \"minimum\": ");
                if (is_enum_type(type)) {
                    // Enums: 0 to however many values
                    append_int(0);
                } else {
                    // Regular int: use min/max from type size
                    append_int(0);
                }
            } else {
                append("\"");
            }
            
            // Description + separator
            append("\n    },\n");
        }
        
        // Remove trailing comma + newline, replace with newline
        if (pos >= 2 && buf[pos-2] == ',' && buf[pos-1] == '\n')
            pos -= 2;
        
        append("\n  }\n}\n");
        buf[pos] = '\0';  // null-terminate
        return buf;
    }
    
    static constexpr std::array<char, BUF> value = build();
    
    // Runtime access: get the schema as a string_view
    static std::string_view get() {
        // Find the null terminator
        const char* data = value.data();
        std::size_t len = 0;
        while (len < BUF && data[len] != '\0') ++len;
        return std::string_view(data, len);
    }
};

// ── Convenience wrapper ───────────────────────────────────────

template <typename T>
std::string_view get_json_schema() {
    return schema_storage<T>::get();
}

} // namespace aura::reflect

#endif // AURA_REFLECT_SCHEMA_HH
