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
    // Enum name available via enumerators_of at compile time
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


// ── auto_serialize<T> — binary serialization (for cache_module) ──

template <typename T>
void auto_serialize(std::vector<char>& buf, const T& obj) {
    constexpr auto members = reflect_members<T>();
    const auto* base = reinterpret_cast<const char*>(&obj);
    for (auto& m : members) {
        const auto* field = base + m.offset;
        switch (m.kind) {
        case MemberKind::Int8:
        case MemberKind::UInt8: {
            char v; std::memcpy(&v, field, 1);
            buf.push_back(v); break;
        }
        case MemberKind::Int16:
        case MemberKind::UInt16: {
            short v; std::memcpy(&v, field, 2);
            buf.insert(buf.end(), reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + 2);
            break;
        }
        case MemberKind::Int32:
        case MemberKind::UInt32:
        case MemberKind::Float: {
            buf.insert(buf.end(), field, field + 4); break;
        }
        case MemberKind::Int64:
        case MemberKind::UInt64:
        case MemberKind::Double: {
            buf.insert(buf.end(), field, field + 8); break;
        }
        case MemberKind::Bool: {
            buf.push_back(*reinterpret_cast<const bool*>(field) ? 1 : 0);
            break;
        }
        case MemberKind::String: {
            auto& s = *reinterpret_cast<const std::string*>(field);
            uint32_t len = s.size();
            buf.insert(buf.end(), reinterpret_cast<char*>(&len), reinterpret_cast<char*>(&len) + 4);
            buf.insert(buf.end(), s.begin(), s.end());
            break;
        }
        case MemberKind::Array: {
            // Array: write raw bytes (elem_size * array_len)
            buf.insert(buf.end(), field, field + m.elem_size * m.array_len);
            break;
        }
        case MemberKind::Vector: {
            // Write vector: size (as u32) + raw elements
            auto& vec = *reinterpret_cast<const std::vector<char>*>(field);
            uint32_t sz = static_cast<uint32_t>(vec.size());
            buf.insert(buf.end(), reinterpret_cast<char*>(&sz), reinterpret_cast<char*>(&sz) + 4);
            if (!vec.empty())
                buf.insert(buf.end(), vec.data(), vec.data() + vec.size());
            break;
        }
        case MemberKind::Other:
        case MemberKind::Unknown:
            break;
        }
    }
}

template <typename T>
std::vector<char> auto_serialize(const T& obj) {
    std::vector<char> buf;
    buf.reserve(1024);
    auto_serialize(buf, obj);
    return buf;
}


// ── auto_deserialize<T> — binary deserialization (stub) ──

template <typename T>
T auto_deserialize(const std::vector<char>& buf, std::size_t& pos) {
    T obj{};
    constexpr auto members = reflect_members<T>();
    auto* base = reinterpret_cast<char*>(&obj);
    for (auto& m : members) {
        auto* field = base + m.offset;
        switch (m.kind) {
        case MemberKind::Int8:
        case MemberKind::UInt8:
            std::memcpy(field, &buf[pos], 1); pos += 1; break;
        case MemberKind::Int16:
        case MemberKind::UInt16:
            std::memcpy(field, &buf[pos], 2); pos += 2; break;
        case MemberKind::Int32:
        case MemberKind::UInt32:
        case MemberKind::Float:
            std::memcpy(field, &buf[pos], 4); pos += 4; break;
        case MemberKind::Int64:
        case MemberKind::UInt64:
        case MemberKind::Double:
            std::memcpy(field, &buf[pos], 8); pos += 8; break;
        case MemberKind::Bool:
            *reinterpret_cast<bool*>(field) = buf[pos++] != 0;
            break;
        case MemberKind::String: {
            uint32_t len; std::memcpy(&len, &buf[pos], 4); pos += 4;
            auto& s = *reinterpret_cast<std::string*>(field);
            s.assign(&buf[pos], &buf[pos + len]); pos += len;
            break;
        }
        case MemberKind::Array: {
            std::memcpy(field, &buf[pos], m.elem_size * m.array_len);
            pos += m.elem_size * m.array_len;
            break;
        }
        case MemberKind::Vector: {
            uint32_t sz; std::memcpy(&sz, &buf[pos], 4); pos += 4;
            auto& vec = *reinterpret_cast<std::vector<char>*>(field);
            vec.assign(&buf[pos], &buf[pos + sz]); pos += sz;
            break;
        }
        case MemberKind::Other:
        case MemberKind::Unknown:
            break;
        }
    }
    return obj;
}

template <typename T>
T auto_deserialize(const std::vector<char>& buf) {
    std::size_t pos = 0;
    return auto_deserialize<T>(buf, pos);
}


// ── Binary Buffer for cache serialization ─────────────────────

class Buffer {
public:
    void write(const void* data, std::size_t size) {
        buf_.insert(buf_.end(), static_cast<const char*>(data),
                    static_cast<const char*>(data) + size);
    }
    
    template <typename T>
    void write(const T& val) {
        write(&val, sizeof(T));
    }
    
    template <typename T>
    void write_vector(const std::vector<T>& vec) {
        auto sz = static_cast<std::uint32_t>(vec.size());
        write(sz);
        write(vec.data(), vec.size() * sizeof(T));
    }
    
    std::uint64_t tell() const { return buf_.size(); }
    
    const std::vector<char>& data() const { return buf_; }
    std::vector<char> take() { return std::move(buf_); }

private:
    std::vector<char> buf_;
};


// ── auto_validate<T> — compile-time struct validation ─────

template <typename T>
bool auto_validate(const T& obj, std::string* error = nullptr) {
    constexpr auto members = reflect_members<T>();
    const auto* base = reinterpret_cast<const char*>(&obj);
    bool ok = true;
    
    for (auto& m : members) {
        const auto* field = base + m.offset;
        switch (m.kind) {
        case MemberKind::Vector: {
            auto& vec = *reinterpret_cast<const std::vector<char>*>(field);
            if (vec.size() > 100000000) {
                if (error) *error = std::string(m.name) + " too large: " + std::to_string(vec.size());
                ok = false;
            }
            break;
        }
        case MemberKind::String: {
            auto& s = *reinterpret_cast<const std::string*>(field);
            if (s.size() > 100000000) {
                if (error) *error = std::string(m.name) + " string too long: " + std::to_string(s.size());
                ok = false;
            }
            break;
        }
        default:
            break;
        }
    }
    return ok;
}

// ═══════════════════════════════════════════════════════════════
//  Recursive binary serialization — auto_bin_write / auto_bin_read
//
//  Uses P1306 template for + P2996 reflection to recursively
//  serialize ANY struct to/from binary buffers.
//
//  Supported: all arithmetic types, enums (as uint8),
//  std::string (length-prefixed), std::vector<T>
//  (count + recursive), std::array<T,N> (recursive),
//  nested structs (recursive), pointers (skipped).
//
//  MUST be compiled with -freflection (GCC 16+).
// ═══════════════════════════════════════════════════════════════

// ── BufferReader — binary read cursor ──────────────────────────

class BufferReader {
public:
    BufferReader() = default;
    BufferReader(const char* data, std::size_t size)
        : data_(data), size_(size), pos_(0) {}

    void read(void* out, std::size_t n) {
        std::memcpy(out, data_ + pos_, n);
        pos_ += n;
    }

    template <typename T>
    T read() {
        T val{};
        read(&val, sizeof(T));
        return val;
    }

    const char* data() const { return data_; }
    std::size_t position() const { return pos_; }
    std::size_t remaining() const { return size_ - pos_; }

    // Advance position (after string data read)
    void seek(std::size_t n) { pos_ += n; }

    bool valid() const { return data_ != nullptr && pos_ <= size_; }
    void reset(const char* d, std::size_t sz) { data_ = d; size_ = sz; pos_ = 0; }

private:
    const char* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_ = 0;
};


// ── Type traits for container detection ───────────────────────-

template <typename T> struct is_std_string      : std::false_type {};
template <> struct is_std_string<std::string>    : std::true_type {};
template <typename T> constexpr bool is_std_string_v  = is_std_string<T>::value;

template <typename T> struct is_std_vector      : std::false_type {};
template <typename T, typename A> struct is_std_vector<std::vector<T, A>> : std::true_type {};
template <typename T> constexpr bool is_std_vector_v  = is_std_vector<T>::value;

template <typename T> struct is_std_array       : std::false_type {};
template <typename T, std::size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
template <typename T> constexpr bool is_std_array_v   = is_std_array<T>::value;

template <typename T> struct always_false       : std::false_type {};
template <typename T> constexpr bool always_false_v = always_false<T>::value;


// ── consteval bridge: runtime vector → compile-time array ─────

template <typename T>
consteval std::size_t count_members_of() {
    using namespace std::meta;
    return nonstatic_data_members_of(^^T, access_context::unchecked()).size();
}

template <typename T>
consteval auto get_data_members() {
    using namespace std::meta;
    constexpr std::size_t N = count_members_of<T>();
    auto vec = nonstatic_data_members_of(^^T, access_context::unchecked());
    std::array<std::meta::info, N> arr{};
    for (std::size_t i = 0; i < N; ++i)
        arr[i] = vec[i];
    return arr;
}


// ── to_json — recursive JSON serializer (for --inspect) ────────
// Uses P1306 template for to recursively serialize ANY type to JSON.
// Unlike auto_to_json, this properly handles vectors and nested structs.

inline void json_escape_to(std::string& out, std::string_view s) {
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
}

template <typename T>
void to_json_impl(std::string& out, const T& val);

template <typename T>
std::string to_json(const T& val) {
    std::string out;
    to_json_impl(out, val);
    return out;
}

template <typename T>
void to_json_impl(std::string& out, const T& val) {
    if constexpr (std::is_enum_v<T>) {
        out += std::to_string(static_cast<int>(val));

    } else if constexpr (is_std_string_v<T>) {
        out += '"';
        json_escape_to(out, val);
        out += '"';

    } else if constexpr (is_std_vector_v<T>) {
        using Elem = typename T::value_type;
        out += '[';
        for (std::size_t i = 0; i < val.size(); ++i) {
            if (i > 0) out += ',';
            to_json_impl(out, val[i]);
        }
        out += ']';

    } else if constexpr (is_std_array_v<T>) {
        out += '[';
        for (std::size_t i = 0; i < val.size(); ++i) {
            if (i > 0) out += ',';
            to_json_impl(out, val[i]);
        }
        out += ']';

    } else if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
        out += "null";

    } else if constexpr (std::is_same_v<T, bool>) {
        out += val ? "true" : "false";

    } else if constexpr (std::is_arithmetic_v<T>) {
        out += std::to_string(val);

    } else if constexpr (std::is_class_v<T>) {
        // Struct — iterate members via P1306 template for
        out += '{';
        bool first = true;
        static constexpr auto aura_members = get_data_members<T>();
        template for (constexpr auto m : aura_members) {
            using namespace std::meta;
            if (!first) out += ',';
            first = false;
            out += '"'; json_escape_to(out, identifier_of(m)); out += '"';
            out += ':';
            to_json_impl(out, val.[:m:]);
        }
        out += '}';

    } else {
        out += "null";
    }
}


// ── bin_write — recursive binary serializer ────────────────────
// Handles any single value: scalar, enum, string, vector, array,
// pointer (skip), or struct (recursive via template for).

template <typename T>
void bin_write(Buffer& buf, const T& val) {
    if constexpr (std::is_enum_v<T>) {
        // Enums → serialize as uint8
        auto v = static_cast<std::uint8_t>(val);
        buf.write(v);

    } else if constexpr (is_std_string_v<T>) {
        // String → length-prefixed
        auto len = static_cast<std::uint32_t>(val.size());
        buf.write(len);
        buf.write(val.data(), len);

    } else if constexpr (is_std_vector_v<T>) {
        // Vector → count + elements (recursive)
        using Elem = typename T::value_type;
        buf.write(static_cast<std::uint32_t>(val.size()));
        for (auto& e : val) bin_write(buf, e);

    } else if constexpr (is_std_array_v<T>) {
        // std::array → elements (recursive, known count)
        for (auto& e : val) bin_write(buf, e);

    } else if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
        // Pointers → not persisted (skip)

    } else if constexpr (std::is_arithmetic_v<T>) {
        // Arithmetic scalars → direct binary write
        buf.write(val);

    } else if constexpr (std::is_class_v<T>) {
        // Struct → iterate nonstatic data members via P1306 template for
        static constexpr auto aura_members = get_data_members<T>();
        template for (constexpr auto m : aura_members) {
            bin_write(buf, val.[:m:]);
        }

    } else {
        static_assert(always_false_v<T>, "unsupported type for bin_write");
    }
}


// ── bin_read — recursive binary deserializer ───────────────────
// Reads into a reference. Handles same type set as bin_write.

template <typename T>
void bin_read(BufferReader& reader, T& val) {
    if constexpr (std::is_enum_v<T>) {
        // Enums → read uint8, cast
        auto v = reader.read<std::uint8_t>();
        val = static_cast<T>(v);

    } else if constexpr (is_std_string_v<T>) {
        // String → length-prefixed
        auto len = reader.read<std::uint32_t>();
        val.assign(reader.data() + reader.position(), len);
        reader.seek(len);

    } else if constexpr (is_std_vector_v<T>) {
        // Vector → count + elements (recursive)
        using Elem = typename T::value_type;
        auto sz = reader.read<std::uint32_t>();
        val.resize(sz);
        for (auto& e : val) bin_read(reader, e);

    } else if constexpr (is_std_array_v<T>) {
        // std::array → elements (recursive, known count)
        for (auto& e : val) bin_read(reader, e);

    } else if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
        // Pointers → not persisted; leave as default (nullptr)

    } else if constexpr (std::is_arithmetic_v<T>) {
        // Arithmetic scalars → direct read
        reader.read(&val, sizeof(T));

    } else if constexpr (std::is_class_v<T>) {
        // Struct → iterate members via P1306 template for
        static constexpr auto aura_members = get_data_members<T>();
        template for (constexpr auto m : aura_members) {
            bin_read(reader, val.[:m:]);
        }

    } else {
        static_assert(always_false_v<T>, "unsupported type for bin_read");
    }
}


// ── Convenience: bin_read<T>(reader) — return by value ─────────

template <typename T>
T bin_read(BufferReader& reader) {
    T val{};
    bin_read(reader, val);
    return val;
}

} // namespace aura::reflect

#endif // AURA_REFLECT_REFLECT_HH
