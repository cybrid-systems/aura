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
#include <optional>
#include <variant>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <concepts>
#include <type_traits>
#include <stdexcept>

namespace aura::reflect {

// ==============================================================
//  Compile-time member descriptor
// ==============================================================

enum class MemberKind : std::uint8_t {
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Bool,
    String,
    StringView, // Issue #217 Cycle 2: std::string_view
    Float,
    Double,
    Array,
    Vector,
    Span, // Issue #217 Cycle 6: std::span<T, N>
    Struct,
    Other,
    Unknown
};

struct MemberInfo {
    std::string_view name;
    std::ptrdiff_t offset;
    MemberKind kind;
    std::size_t elem_size = 0; // for Array/Vector: element byte size
    std::size_t array_len = 0; // for Array: fixed count
};

// ── Type traits for container detection (Issue #215) ─────────

template <typename T> struct is_std_string : std::false_type {};
template <> struct is_std_string<std::string> : std::true_type {};
template <typename T> constexpr bool is_std_string_v = is_std_string<T>::value;

// Issue #217 Cycle 2: std::string_view detection
template <typename T> struct is_std_string_view : std::false_type {};
template <> struct is_std_string_view<std::string_view> : std::true_type {};
template <typename T> constexpr bool is_std_string_view_v = is_std_string_view<T>::value;

// Issue #217 Cycle 6: std::span detection
template <typename T> struct is_std_span : std::false_type {};
template <typename T, std::size_t Extent>
struct is_std_span<std::span<T, Extent>> : std::true_type {};
template <typename T> constexpr bool is_std_span_v = is_std_span<T>::value;

template <typename T> struct is_std_vector : std::false_type {};
template <typename T, typename A> struct is_std_vector<std::vector<T, A>> : std::true_type {};
template <typename T> constexpr bool is_std_vector_v = is_std_vector<T>::value;

template <typename T> struct is_std_array : std::false_type {};
template <typename T, std::size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
template <typename T> constexpr bool is_std_array_v = is_std_array<T>::value;

template <typename T> struct is_std_optional : std::false_type {};
template <typename T> struct is_std_optional<std::optional<T>> : std::true_type {};
template <typename T> constexpr bool is_std_optional_v = is_std_optional<T>::value;

template <typename T> struct is_std_variant : std::false_type {};
template <typename... Ts> struct is_std_variant<std::variant<Ts...>> : std::true_type {};
template <typename T> constexpr bool is_std_variant_v = is_std_variant<T>::value;

// ── Type classifier ───────────────────────────────────────────

consteval MemberKind classify_type(std::meta::info type) {
    using namespace std::meta;

    if (is_same_type(type, ^^bool))
        return MemberKind::Bool;
    if (is_same_type(type, ^^std::string))
        return MemberKind::String;
    // Issue #217 Cycle 2: std::string_view is a
    // string-like type (ptr + size) but with a different
    // layout from std::string. Use a separate MemberKind
    // so the auto_serialize switch can extract the
    // (ptr, size) correctly.
    if (is_same_type(type, ^^std::string_view))
        return MemberKind::StringView;
    if (is_same_type(type, ^^float))
        return MemberKind::Float;
    if (is_same_type(type, ^^double))
        return MemberKind::Double;

    if (is_same_type(type, ^^char) || is_same_type(type, ^^signed char))
        return MemberKind::Int8;
    if (is_same_type(type, ^^unsigned char))
        return MemberKind::UInt8;
    if (is_same_type(type, ^^short))
        return MemberKind::Int16;
    if (is_same_type(type, ^^unsigned short))
        return MemberKind::UInt16;
    if (is_same_type(type, ^^int))
        return MemberKind::Int32;
    if (is_same_type(type, ^^unsigned) || is_same_type(type, ^^unsigned int))
        return MemberKind::UInt32;
    if (is_same_type(type, ^^long) || is_same_type(type, ^^long long))
        return MemberKind::Int64;
    if (is_same_type(type, ^^unsigned long) || is_same_type(type, ^^unsigned long long))
        return MemberKind::UInt64;
    if (is_same_type(type, ^^std::size_t))
        return MemberKind::UInt64;

    // enum → treat as integer (use underlying type size)
    // Enum name available via enumerators_of at compile time
    if (is_enum_type(type)) {
        auto sz = size_of(type);
        if (sz <= 1)
            return MemberKind::Int8;
        if (sz <= 2)
            return MemberKind::Int16;
        if (sz <= 4)
            return MemberKind::Int32;
        return MemberKind::Int64;
    }

    // std::array<T,N> / std::vector<T> / std::span<T,N> detection
    // via display_string (template_of + is_same_type has
    // issues with GCC 16.1)
    if (is_class_type(type) && has_template_arguments(type)) {
        auto str = display_string_of(type);
        if (str.starts_with("std::array"))
            return MemberKind::Array;
        if (str.starts_with("std::vector"))
            return MemberKind::Vector;
        if (str.starts_with("std::span"))
            return MemberKind::Span;
    }

    // Nested struct with reflectable members → recursive
    if (is_class_type(type)) {
        auto n = nonstatic_data_members_of(type, access_context::unchecked()).size();
        if (n > 0)
            return MemberKind::Struct;
    }

    // Integral fallback
    if (is_integral_type(type)) {
        auto sz = size_of(type);
        if (sz <= 1)
            return MemberKind::Int8;
        if (sz <= 2)
            return MemberKind::Int16;
        if (sz <= 4)
            return MemberKind::Int32;
        return MemberKind::Int64;
    }

    return MemberKind::Unknown;
}

// Get element type size for std::array<T,N> / std::vector<T>
consteval std::size_t elem_size_of(std::meta::info type) {
    using namespace std::meta;
    // For std::array/vector, the first template arg is T
    auto args = template_arguments_of(type);
    if (args.empty())
        return 0;
    // static_assert(is_type(args[0]), "first template arg should be a type");
    return size_of(args.data()[0]);
}

// Get array length for std::array<T,N>
consteval std::size_t array_size_of(std::meta::info type) {
    using namespace std::meta;
    auto args = template_arguments_of(type);
    if (args.size() < 2)
        return 0;
    // static_assert(is_value(args.data()[1]), "second template arg should be a value");
    // Use extract to get the value as size_t
    return static_cast<std::size_t>(extract<std::size_t>(args.data()[1]));
}

// ── Member reflection ─────────────────────────────────────────

template <typename T> consteval std::size_t member_count() {
    return std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()).size();
}

template <typename T> consteval std::array<MemberInfo, member_count<T>()> reflect_members() {
    using namespace std::meta;
    constexpr auto N = member_count<T>();
    auto vec = nonstatic_data_members_of(^^T, access_context::unchecked());
    std::array<MemberInfo, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        auto m = vec[i];
        auto type = type_of(m);
        auto kind = classify_type(type);
        result[i] = MemberInfo{
            .name = identifier_of(m),
            .offset = offset_of(m).bytes,
            .kind = kind,
            .elem_size = (kind == MemberKind::Array || kind == MemberKind::Vector ||
                          kind == MemberKind::Span)
                             ? elem_size_of(type)
                             : 0,
            .array_len = (kind == MemberKind::Array) ? array_size_of(type) : 0,
        };
    }
    return result;
}

// ── Module exports reflection (Issue #214 Cycle 1) ───────────
//
// Compile-time scan of a module's public function/data member
// names. Returns an array of string_views of identifier names.
//
// The `filter` template parameter lets callers restrict the
// set — e.g. only public functions, only data members with a
// certain attribute, etc. The default filter returns all
// named, non-static members.
//
// Usage:
//   constexpr auto exports = module_exports<MyType>();
//   for (auto name : exports) {
//       std::println("{}", name);
//   }
//
// Consumers (Issue #178):
//   - AI Agent: discover a module's surface area
//   - IDE: autocomplete candidate generation
//   - EDSL: validate `require` references statically
template <typename T, std::size_t MaxExports = 64> struct ModuleExports {
    std::array<std::string_view, MaxExports> data{};
    std::size_t size = 0;
    constexpr std::string_view operator[](std::size_t i) const { return data[i]; }
    constexpr auto begin() const { return data.begin(); }
    constexpr auto end() const { return data.begin() + size; }
};

template <typename T, std::size_t MaxExports = 64>
consteval ModuleExports<T, MaxExports> module_exports() {
    auto members = std::meta::members_of(^^T, std::meta::access_context::unchecked());
    ModuleExports<T, MaxExports> me{};
    for (auto m : members) {
        if (me.size >= MaxExports)
            break;
        if (!std::meta::has_identifier(m))
            continue;
        me.data[me.size++] = std::meta::identifier_of(m);
    }
    return me;
}

// ── JSON escape ───────────────────────────────────────────────

inline std::string json_escape(std::string_view s) {
    std::string out;
    for (auto c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    return out;
}

// ── Forward decl ──────────────────────────────────────────────

template <typename T> std::string auto_to_json(const T& obj);

// std::vector<T> — JSON array of scalars
template <typename T> std::string auto_to_json(const std::vector<T>& vec) {
    std::string json = "[";
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (i > 0)
            json += ",";
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
        if (i > 0)
            json += ",";
        json += "\"" + json_escape(vec[i]) + "\"";
    }
    json += "]";
    return json;
}

// std::array<T,N> — JSON array
template <typename T, std::size_t N> std::string auto_to_json(const std::array<T, N>& arr) {
    std::string json = "[";
    for (std::size_t i = 0; i < N; ++i) {
        if (i > 0)
            json += ",";
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

template <typename T> std::string auto_to_json(const T& obj) {
    return to_json(obj);
}

// ── Convenience: pretty-print ─────────────────────────────────

// ── JSON pretty-print helper ────────────────────────────────────

// Simple JSON indenter: takes compact JSON, returns indented form
inline std::string prettify_json(const std::string& compact) {
    std::string out;
    out.reserve(compact.size() * 2);
    int indent = 0;
    bool in_string = false;
    bool escaped = false;

    for (char c : compact) {
        if (escaped) {
            escaped = false;
            out += c;
            continue;
        }
        if (c == '\\') {
            out += c;
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            out += c;
            continue;
        }
        if (in_string) {
            out += c;
            continue;
        }

        if (c == '{' || c == '[') {
            out += c;
            out += '\n';
            ++indent;
            out.append(static_cast<std::size_t>(indent * 2), ' ');
        } else if (c == '}' || c == ']') {
            out += '\n';
            --indent;
            if (indent < 0)
                indent = 0;
            out.append(static_cast<std::size_t>(indent * 2), ' ');
            out += c;
        } else if (c == ',') {
            out += c;
            out += '\n';
            out.append(static_cast<std::size_t>(indent * 2), ' ');
        } else if (c == ':') {
            out += ": ";
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            out += c;
        }
    }
    return out;
}

// Convenience: pretty-print wrapper (compact by default)
template <typename T> std::string auto_to_json_pretty(const T& obj) {
    return auto_to_json(obj);
}


// ── auto_serialize<T> — binary serialization (for cache_module) ──

// Issue #215: Top-level container overloads. These are
// picked over the generic struct path for std::vector<T>,
// std::optional<T>, and std::variant<Ts...>. They
// recurse on each element so nested containers work.

// Forward declare the string + string_view overloads so
// they're visible to the vector/optional/variant overload
// bodies below.
void auto_serialize(std::vector<char>& buf, const std::string& s);
void auto_serialize(std::vector<char>& buf, const std::string_view& sv);

// Issue #217 Cycle 14 P3: forward-declare the generic
// struct template so it's visible to the
// std::vector<T>/std::optional<T>/std::variant<Ts...>/std::array<T,N>
// overloads below. Without this, the vector's `auto_serialize(buf, e)`
// call (for non-trivially-copyable element types like
// MutationRecord) can't find the struct overload because
// it hasn't been declared yet.
template <typename T> void auto_serialize(std::vector<char>& buf, const T& obj);

// std::vector<T> overload (any T)
template <typename T> void auto_serialize(std::vector<char>& buf, const std::vector<T>& vec) {
    auto sz = static_cast<std::uint32_t>(vec.size());
    buf.insert(buf.end(), reinterpret_cast<char*>(&sz), reinterpret_cast<char*>(&sz) + 4);
    if constexpr (std::is_trivially_copyable_v<T>) {
        // POD: write raw bytes (faster)
        if (!vec.empty())
            buf.insert(buf.end(), reinterpret_cast<const char*>(vec.data()),
                       reinterpret_cast<const char*>(vec.data()) + vec.size() * sizeof(T));
    } else {
        // Non-POD: recurse on each element
        for (const auto& e : vec)
            auto_serialize(buf, e);
    }
}

// std::string overload (top-level + recursion target)
inline void auto_serialize(std::vector<char>& buf, const std::string& s) {
    std::uint32_t len = static_cast<std::uint32_t>(s.size());
    buf.insert(buf.end(), reinterpret_cast<char*>(&len), reinterpret_cast<char*>(&len) + 4);
    buf.insert(buf.end(), s.begin(), s.end());
}

// Issue #217 Cycle 2: std::string_view top-level
// overload. Same length-prefixed layout as std::string;
// used for recursion targets (e.g. a vector<string_view>).
inline void auto_serialize(std::vector<char>& buf, const std::string_view& sv) {
    std::uint32_t len = static_cast<std::uint32_t>(sv.size());
    buf.insert(buf.end(), reinterpret_cast<char*>(&len), reinterpret_cast<char*>(&len) + 4);
    buf.insert(buf.end(), sv.data(), sv.data() + sv.size());
}

// std::optional<T> overload
template <typename T> void auto_serialize(std::vector<char>& buf, const std::optional<T>& opt) {
    char has_value = opt.has_value() ? 1 : 0;
    buf.push_back(has_value);
    if (opt.has_value()) {
        if constexpr (std::is_trivially_copyable_v<T> && !is_std_string_v<T>) {
            // POD: raw bytes
            buf.insert(buf.end(), reinterpret_cast<const char*>(&*opt),
                       reinterpret_cast<const char*>(&*opt) + sizeof(T));
        } else {
            // Non-POD: recurse
            auto_serialize(buf, *opt);
        }
    }
}

// std::variant<Ts...> overload
template <typename... Ts>
void auto_serialize(std::vector<char>& buf, const std::variant<Ts...>& v) {
    std::uint32_t idx = static_cast<std::uint32_t>(v.index());
    buf.insert(buf.end(), reinterpret_cast<char*>(&idx), reinterpret_cast<char*>(&idx) + 4);
    std::visit(
        [&buf](const auto& inner) {
            using InnerT = std::remove_cv_t<std::remove_reference_t<decltype(inner)>>;
            if constexpr (std::is_trivially_copyable_v<InnerT> && !is_std_string_v<InnerT>) {
                buf.insert(buf.end(), reinterpret_cast<const char*>(&inner),
                           reinterpret_cast<const char*>(&inner) + sizeof(InnerT));
            } else {
                auto_serialize(buf, inner);
            }
        },
        v);
}

// std::array<T, N> overload (for non-POD T or as top-level)
template <typename T, std::size_t N>
void auto_serialize(std::vector<char>& buf, const std::array<T, N>& arr) {
    if constexpr (std::is_trivially_copyable_v<T>) {
        // POD: raw bytes
        if (N > 0)
            buf.insert(buf.end(), reinterpret_cast<const char*>(arr.data()),
                       reinterpret_cast<const char*>(arr.data()) + N * sizeof(T));
    } else {
        // Non-POD: recurse
        for (const auto& e : arr)
            auto_serialize(buf, e);
    }
}

// ── Generic flat-struct serialization (POD/flat case) ──

template <typename T> void auto_serialize(std::vector<char>& buf, const T& obj) {
    constexpr auto members = reflect_members<T>();
    const auto* base = reinterpret_cast<const char*>(&obj);
    for (auto& m : members) {
        const auto* field = base + m.offset;
        switch (m.kind) {
            case MemberKind::Int8:
            case MemberKind::UInt8: {
                char v;
                std::memcpy(&v, field, 1);
                buf.push_back(v);
                break;
            }
            case MemberKind::Int16:
            case MemberKind::UInt16: {
                short v;
                std::memcpy(&v, field, 2);
                buf.insert(buf.end(), reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + 2);
                break;
            }
            case MemberKind::Int32:
            case MemberKind::UInt32:
            case MemberKind::Float: {
                buf.insert(buf.end(), field, field + 4);
                break;
            }
            case MemberKind::Int64:
            case MemberKind::UInt64:
            case MemberKind::Double: {
                buf.insert(buf.end(), field, field + 8);
                break;
            }
            case MemberKind::Bool: {
                buf.push_back(*reinterpret_cast<const bool*>(field) ? 1 : 0);
                break;
            }
            case MemberKind::String: {
                auto& s = *reinterpret_cast<const std::string*>(field);
                uint32_t len = s.size();
                buf.insert(buf.end(), reinterpret_cast<char*>(&len),
                           reinterpret_cast<char*>(&len) + 4);
                buf.insert(buf.end(), s.begin(), s.end());
                break;
            }
            case MemberKind::StringView: {
                // Issue #217 Cycle 2: std::string_view is
                // {const char* data, size_t size}. We
                // extract both and write the same
                // length-prefixed raw bytes as std::string.
                auto& sv = *reinterpret_cast<const std::string_view*>(field);
                uint32_t len = static_cast<uint32_t>(sv.size());
                buf.insert(buf.end(), reinterpret_cast<char*>(&len),
                           reinterpret_cast<char*>(&len) + 4);
                buf.insert(buf.end(), sv.data(), sv.data() + sv.size());
                break;
            }
            case MemberKind::Array: {
                // Array: write raw bytes (elem_size * array_len)
                buf.insert(buf.end(), field, field + m.elem_size * m.array_len);
                break;
            }
            case MemberKind::Span: {
                // Issue #217 Cycle 6/7: std::span is a
                // non-owning view (ptr + size). Write
                // byte-prefixed raw elements (NOT count-
                // prefixed — the element type might not
                // be char, so we write the byte count
                // directly). The deserialized span will
                // point into the buf (no copy) — caller
                // must keep the buf alive for the
                // lifetime of the deserialized span.
                //
                // Cycle 7 fix: use elem_size from the
                // MemberInfo (set by reflect_members for
                // Span MemberKind) to correctly compute
                // the byte count for non-char element
                // types (e.g. std::span<const NodeId> in
                // NodeView).
                auto& sp = *reinterpret_cast<const std::span<const char>*>(field);
                uint32_t byte_count = static_cast<uint32_t>(sp.size() * m.elem_size);
                buf.insert(buf.end(), reinterpret_cast<char*>(&byte_count),
                           reinterpret_cast<char*>(&byte_count) + 4);
                if (byte_count > 0)
                    buf.insert(buf.end(), sp.data(), sp.data() + byte_count);
                break;
            }
            case MemberKind::Vector: {
                // Issue #217 Cycle 11: vector<string> special path.
                // The POD-vector path below reinterpret_casts to
                // vector<char>, which gives the byte count correctly
                // for POD element types but produces garbage for
                // std::string (each string is separately heap-allocated,
                // so the vector's contiguous data is just the string
                // objects' internal memory, not the string contents).
                // Detect by elem_size == sizeof(std::string).
                // Issue #1123: cap count/len to uint32 max; refuse silent wrap.
                if (m.elem_size == sizeof(std::string)) {
                    auto& vec = *reinterpret_cast<const std::vector<std::string>*>(field);
                    if (vec.size() > 0xFFFFFFFFull)
                        break; // refuse oversized vector
                    uint32_t count = static_cast<uint32_t>(vec.size());
                    buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                               reinterpret_cast<char*>(&count) + 4);
                    for (const auto& s : vec) {
                        if (s.size() > 0xFFFFFFFFull)
                            break;
                        uint32_t len = static_cast<uint32_t>(s.size());
                        buf.insert(buf.end(), reinterpret_cast<char*>(&len),
                                   reinterpret_cast<char*>(&len) + 4);
                        if (!s.empty())
                            buf.insert(buf.end(), s.data(), s.data() + s.size());
                    }
                } else {
                    // POD-vector path: reinterpret as vector<char>
                    // so vec.size() gives the byte count.
                    auto& vec = *reinterpret_cast<const std::vector<char>*>(field);
                    if (vec.size() > 0xFFFFFFFFull)
                        break;
                    uint32_t sz = static_cast<uint32_t>(vec.size());
                    buf.insert(buf.end(), reinterpret_cast<char*>(&sz),
                               reinterpret_cast<char*>(&sz) + 4);
                    if (!vec.empty())
                        buf.insert(buf.end(), vec.data(), vec.data() + vec.size());
                }
                break;
            }
            case MemberKind::Struct:
                // Issue #1124: nested structs need MemberInfo type_info for
                // recursive serialize. Refuse silent drop — surface the gap.
                throw std::runtime_error(
                    "auto_serialize: nested MemberKind::Struct not yet supported");
            case MemberKind::Other:
            case MemberKind::Unknown:
                break;
        }
    }
}

template <typename T> std::vector<char> auto_serialize(const T& obj) {
    std::vector<char> buf;
    buf.reserve(1024);
    auto_serialize(buf, obj);
    return buf;
}


// ── auto_deserialize<T> — binary deserialization (stub) ──

// Issue #215: Single dispatching template that handles flat
// structs, vector<T>, optional<T>, variant<Ts...>, and
// array<T, N> based on T's traits. The user passes T
// explicitly and gets back a T.

// Forward declarations (Issue #215): the inner dispatcher
// and struct path are defined later; the container
// overloads below use them via these declarations.
template <typename T> T auto_deserialize_inner(const std::vector<char>& buf, std::size_t& pos);

template <typename T> T auto_deserialize_struct(const std::vector<char>& buf, std::size_t& pos);

// Issue #1101 family: bounds-checked buffer reads for deserialize.
inline bool reflect_ensure(const std::vector<char>& buf, std::size_t pos, std::size_t n) noexcept {
    return n <= buf.size() && pos <= buf.size() - n;
}

// std::vector<T> overload — reads size + elements (recursive)
template <typename T>
std::vector<T> auto_deserialize_vec(const std::vector<char>& buf, std::size_t& pos) {
    std::vector<T> result;
    if (!reflect_ensure(buf, pos, 4))
        return result;
    std::uint32_t sz = 0;
    std::memcpy(&sz, buf.data() + pos, 4);
    pos += 4;
    // Cap element count to mitigate OOM on malicious sizes.
    constexpr std::uint32_t kMaxVecElems = 50'000'000u;
    if (sz > kMaxVecElems)
        return result;
    if constexpr (std::is_trivially_copyable_v<T>) {
        const std::size_t bytes = static_cast<std::size_t>(sz) * sizeof(T);
        if (sz != 0 && bytes / sizeof(T) != sz) // overflow
            return result;
        if (!reflect_ensure(buf, pos, bytes))
            return result;
        result.resize(sz);
        if (sz > 0)
            std::memcpy(result.data(), buf.data() + pos, bytes);
        pos += bytes;
    } else {
        result.reserve(sz);
        for (std::uint32_t i = 0; i < sz; ++i) {
            if (pos >= buf.size())
                break;
            result.push_back(auto_deserialize_inner<T>(buf, pos));
        }
    }
    return result;
}

// std::array<T, N> overload
template <typename T, std::size_t N>
std::array<T, N> auto_deserialize_arr(const std::vector<char>& buf, std::size_t& pos) {
    std::array<T, N> result{};
    if constexpr (std::is_trivially_copyable_v<T>) {
        const std::size_t bytes = N * sizeof(T);
        if (N > 0 && !reflect_ensure(buf, pos, bytes))
            return result;
        if (N > 0)
            std::memcpy(result.data(), buf.data() + pos, bytes);
        pos += bytes;
    } else {
        for (std::size_t i = 0; i < N; ++i) {
            if (pos >= buf.size())
                break;
            result[i] = auto_deserialize_inner<T>(buf, pos);
        }
    }
    return result;
}

// std::optional<T> overload
template <typename T>
std::optional<T> auto_deserialize_opt(const std::vector<char>& buf, std::size_t& pos) {
    if (!reflect_ensure(buf, pos, 1))
        return std::nullopt;
    char has_value = buf[pos++];
    if (!has_value)
        return std::nullopt;
    if constexpr (std::is_trivially_copyable_v<T> && !is_std_string_v<T>) {
        if (!reflect_ensure(buf, pos, sizeof(T)))
            return std::nullopt;
        T val{};
        std::memcpy(&val, buf.data() + pos, sizeof(T));
        pos += sizeof(T);
        return val;
    } else {
        return auto_deserialize_inner<T>(buf, pos);
    }
}

// std::variant<Ts...> overload
template <std::size_t I, typename... Ts>
std::variant<Ts...> auto_deserialize_variant_at(const std::vector<char>& buf, std::size_t& pos,
                                                std::uint32_t idx) {
    if constexpr (I < sizeof...(Ts)) {
        using CurT = std::variant_alternative_t<I, std::variant<Ts...>>;
        if (I == idx) {
            if constexpr (std::is_trivially_copyable_v<CurT> && !is_std_string_v<CurT>) {
                CurT val;
                std::memcpy(&val, &buf[pos], sizeof(CurT));
                pos += sizeof(CurT);
                return val;
            } else {
                return auto_deserialize_inner<CurT>(buf, pos);
            }
        } else {
            return auto_deserialize_variant_at<I + 1, Ts...>(buf, pos, idx);
        }
    } else {
        return std::variant<Ts...>{};
    }
}
template <typename... Ts>
std::variant<Ts...> auto_deserialize_var(const std::vector<char>& buf, std::size_t& pos) {
    // Issue #1109: bounds-check index; out-of-range → default-construct alt 0.
    if (!reflect_ensure(buf, pos, 4))
        return std::variant<Ts...>{};
    std::uint32_t idx = 0;
    std::memcpy(&idx, buf.data() + pos, 4);
    pos += 4;
    if (idx >= sizeof...(Ts))
        return std::variant<Ts...>{};
    return auto_deserialize_variant_at<0, Ts...>(buf, pos, idx);
}

// Inner dispatcher — given an element type T (could be int,
// string, struct, etc.), produce a T. The top-level
// auto_deserialize<T> routes container types to the
// per-container overloads above; this dispatcher handles
// the element types.
template <typename T> T auto_deserialize_inner(const std::vector<char>& buf, std::size_t& pos) {
    if constexpr (is_std_string_v<T>) {
        // Issue #1107: bounds-check string len + payload.
        if (!reflect_ensure(buf, pos, 4))
            return T{};
        std::uint32_t len = 0;
        std::memcpy(&len, buf.data() + pos, 4);
        pos += 4;
        if (!reflect_ensure(buf, pos, len))
            return T{};
        T val;
        val.assign(buf.data() + pos, len);
        pos += len;
        return val;
    } else if constexpr (std::is_trivially_copyable_v<T>) {
        if (!reflect_ensure(buf, pos, sizeof(T)))
            return T{};
        T val{};
        std::memcpy(&val, buf.data() + pos, sizeof(T));
        pos += sizeof(T);
        return val;
    } else {
        // For complex element types, fall through to the
        // member-based path (assumes T is a flat struct)
        return auto_deserialize_struct<T>(buf, pos);
    }
}

// Member-based struct path (POD flat structs)
template <typename T> T auto_deserialize_struct(const std::vector<char>& buf, std::size_t& pos) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
    T obj{};
    constexpr auto members = reflect_members<T>();
    auto* base = reinterpret_cast<char*>(&obj);
    for (auto& m : members) {
        auto* field = base + m.offset;
        switch (m.kind) {
            case MemberKind::Int8:
            case MemberKind::UInt8:
                std::memcpy(field, &buf[pos], 1);
                pos += 1;
                break;
            case MemberKind::Int16:
            case MemberKind::UInt16:
                std::memcpy(field, &buf[pos], 2);
                pos += 2;
                break;
            case MemberKind::Int32:
            case MemberKind::UInt32:
            case MemberKind::Float:
                std::memcpy(field, &buf[pos], 4);
                pos += 4;
                break;
            case MemberKind::Int64:
            case MemberKind::UInt64:
            case MemberKind::Double:
                std::memcpy(field, &buf[pos], 8);
                pos += 8;
                break;
            case MemberKind::Bool:
                *reinterpret_cast<bool*>(field) = buf[pos++] != 0;
                break;
            case MemberKind::String: {
                uint32_t len;
                std::memcpy(&len, &buf[pos], 4);
                pos += 4;
                auto& s = *reinterpret_cast<std::string*>(field);
                s.assign(&buf[pos], &buf[pos + len]);
                pos += len;
                break;
            }
            case MemberKind::StringView: {
                // Issue #217 Cycle 2: read length-prefixed
                // raw bytes into a std::string_view. The
                // string_view points into the buffer (no
                // copy), so the caller must keep the buffer
                // alive for the lifetime of the view.
                uint32_t len;
                std::memcpy(&len, &buf[pos], 4);
                pos += 4;
                const std::string_view sv(buf.data() + pos, len);
                std::memcpy(field, &sv, sizeof(sv));
                pos += len;
                break;
            }
            case MemberKind::Array: {
                std::memcpy(field, &buf[pos], m.elem_size * m.array_len);
                pos += m.elem_size * m.array_len;
                break;
            }
            case MemberKind::Span: {
                // Issue #1126: bounds-check payload. Span remains
                // non-owning into buf (caller must keep buf alive);
                // empty span on short/corrupt input (no dangling).
                if (!reflect_ensure(buf, pos, 4)) {
                    const std::span<const char> empty{};
                    std::memcpy(field, &empty, sizeof(empty));
                    break;
                }
                uint32_t byte_count = 0;
                std::memcpy(&byte_count, buf.data() + pos, 4);
                pos += 4;
                if (!reflect_ensure(buf, pos, byte_count)) {
                    const std::span<const char> empty{};
                    std::memcpy(field, &empty, sizeof(empty));
                    break;
                }
                const std::size_t elem_count =
                    m.elem_size > 0 ? byte_count / m.elem_size : byte_count;
                const std::span<const char> sp(buf.data() + pos, elem_count);
                std::memcpy(field, &sp, sizeof(sp));
                pos += byte_count;
                break;
            }
            case MemberKind::Vector: {
                // Issue #217 Cycle 11: vector<string> special path
                // (mirror of the serialize side). For POD element
                // types, the data is contiguous raw bytes. For
                // vector<string>, each string is length-prefixed
                // and the count precedes the list.
                // Issue #1123: bounds-check count/len payloads.
                if (m.elem_size == sizeof(std::string)) {
                    if (!reflect_ensure(buf, pos, 4))
                        break;
                    uint32_t count = 0;
                    std::memcpy(&count, buf.data() + pos, 4);
                    pos += 4;
                    auto& vec = *reinterpret_cast<std::vector<std::string>*>(field);
                    vec.clear();
                    if (count > 50'000'000u)
                        break;
                    vec.reserve(count);
                    for (uint32_t i = 0; i < count; ++i) {
                        if (!reflect_ensure(buf, pos, 4))
                            break;
                        uint32_t len = 0;
                        std::memcpy(&len, buf.data() + pos, 4);
                        pos += 4;
                        if (!reflect_ensure(buf, pos, len))
                            break;
                        std::string s(buf.data() + pos, len);
                        pos += len;
                        vec.push_back(std::move(s));
                    }
                } else {
                    if (!reflect_ensure(buf, pos, 4))
                        break;
                    uint32_t sz = 0;
                    std::memcpy(&sz, buf.data() + pos, 4);
                    pos += 4;
                    if (!reflect_ensure(buf, pos, sz))
                        break;
                    auto& vec = *reinterpret_cast<std::vector<char>*>(field);
                    vec.assign(buf.data() + pos, buf.data() + pos + sz);
                    pos += sz;
                }
                break;
            }
            case MemberKind::Struct:
                // Issue #1125: symmetric to #1124 — do not silently skip.
                throw std::runtime_error(
                    "auto_deserialize_struct: nested MemberKind::Struct not yet supported");
            case MemberKind::Other:
            case MemberKind::Unknown:
                break;
        }
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    return obj;
}

// ── Top-level dispatcher for auto_deserialize (Issue #215) ──
//
// Single template that dispatches on T's traits:
//   - std::vector<T_elem>  → auto_deserialize_vec<T_elem>
//   - std::array<T, N>     → auto_deserialize_arr<T>
//   - std::optional<T>     → auto_deserialize_opt<T>
//   - std::variant<Ts...>  → auto_deserialize_var<Ts...>
//   - flat struct          → auto_deserialize_struct<T>
//   - scalar/string        → auto_deserialize_inner<T>
namespace detail {

    // Variant dispatch helper. Expands the variant's
    // alternative types into a parameter pack and calls
    // auto_deserialize_var.
    template <typename Variant, std::size_t... Is>
    Variant auto_deserialize_variant_impl(const std::vector<char>& buf, std::size_t& pos,
                                          std::index_sequence<Is...>) {
        return auto_deserialize_var<typename std::variant_alternative_t<Is, Variant>...>(buf, pos);
    }

} // namespace detail

template <typename T> T auto_deserialize(const std::vector<char>& buf, std::size_t& pos) {
    if constexpr (is_std_vector_v<T>) {
        using Inner = typename T::value_type;
        return auto_deserialize_vec<Inner>(buf, pos);
    } else if constexpr (is_std_array_v<T>) {
        using Inner = typename T::value_type;
        constexpr std::size_t N = std::tuple_size_v<T>;
        return auto_deserialize_arr<Inner, N>(buf, pos);
    } else if constexpr (is_std_optional_v<T>) {
        using Inner = typename T::value_type;
        return auto_deserialize_opt<Inner>(buf, pos);
    } else if constexpr (is_std_variant_v<T>) {
        constexpr std::size_t N = std::variant_size_v<T>;
        return detail::auto_deserialize_variant_impl<T>(buf, pos, std::make_index_sequence<N>{});
    } else if constexpr (is_std_string_v<T> || std::is_arithmetic_v<T>) {
        // Scalar/string element types: use the inner
        // dispatcher (raw memcpy for POD, length-prefix
        // for strings).
        return auto_deserialize_inner<T>(buf, pos);
    } else {
        // Flat struct (with reflectable members): use
        // the member-based path. This is critical —
        // auto_deserialize_inner would do a raw memcpy
        // of sizeof(T) bytes including padding, but
        // auto_serialize writes only the member bytes
        // (skipping padding). The member-based path
        // reads back exactly what was written.
        return auto_deserialize_struct<T>(buf, pos);
    }
}

template <typename T> T auto_deserialize(const std::vector<char>& buf) {
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

    template <typename T> void write(const T& val) { write(&val, sizeof(T)); }

    template <typename T> void write_vector(const std::vector<T>& vec) {
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

template <typename T> bool auto_validate(const T& obj, std::string* error = nullptr) {
    constexpr auto members = reflect_members<T>();
    const auto* base = reinterpret_cast<const char*>(&obj);
    bool ok = true;

    for (auto& m : members) {
        const auto* field = base + m.offset;
        switch (m.kind) {
            case MemberKind::Vector: {
                auto& vec = *reinterpret_cast<const std::vector<char>*>(field);
                if (vec.size() > 100000000) {
                    if (error)
                        *error = std::string(m.name) + " too large: " + std::to_string(vec.size());
                    ok = false;
                }
                break;
            }
            case MemberKind::String: {
                auto& s = *reinterpret_cast<const std::string*>(field);
                if (s.size() > 100000000) {
                    if (error)
                        *error =
                            std::string(m.name) + " string too long: " + std::to_string(s.size());
                    ok = false;
                }
                break;
            }
            case MemberKind::StringView: {
                // Issue #217 Cycle 2: same size check for
                // string_view (it's also a string-like type
                // with a size() accessor).
                auto& sv = *reinterpret_cast<const std::string_view*>(field);
                if (sv.size() > 100000000) {
                    if (error)
                        *error = std::string(m.name) +
                                 " string_view too long: " + std::to_string(sv.size());
                    ok = false;
                }
                break;
            }
            case MemberKind::Span: {
                // Issue #217 Cycle 6: size check for
                // std::span (it's also a container-like
                // type with a size() accessor).
                auto& sp = *reinterpret_cast<const std::span<const char>*>(field);
                if (sp.size() > 100000000) {
                    if (error)
                        *error =
                            std::string(m.name) + " span too large: " + std::to_string(sp.size());
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

// Issue #488: mutation-specific reflect health probe for Guard
// dtor snapshots (marker consistency + generation sanity).
struct MutationReflectHealth {
    bool marker_consistent = true;
    bool generation_healthy = true;
    std::uint64_t dirty_nodes = 0;
    std::uint64_t macro_markers = 0;
};

inline bool validate_mutation_reflect_health(const MutationReflectHealth& health,
                                             std::string* error = nullptr) {
    if (!health.generation_healthy) {
        if (error)
            *error = "generation health check failed";
        return false;
    }
    if (!health.marker_consistent) {
        if (error)
            *error = "macro marker consistency check failed";
        return false;
    }
    (void)health.dirty_nodes;
    (void)health.macro_markers;
    return true;
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
        : data_(data)
        , size_(size)
        , pos_(0) {}

    // Issue #1117: bounds-checked reads (no OOB memcpy).
    void read(void* out, std::size_t n) {
        if (n > remaining()) {
            // Truncate / zero-fill on short buffer.
            const auto avail = remaining();
            if (avail > 0)
                std::memcpy(out, data_ + pos_, avail);
            if (n > avail)
                std::memset(static_cast<char*>(out) + avail, 0, n - avail);
            pos_ = size_;
            return;
        }
        std::memcpy(out, data_ + pos_, n);
        pos_ += n;
    }

    template <typename T> T read() {
        T val{};
        read(&val, sizeof(T));
        return val;
    }

    const char* data() const { return data_; }
    std::size_t position() const { return pos_; }
    std::size_t remaining() const { return pos_ <= size_ ? size_ - pos_ : 0; }

    // Advance position (after string data read)
    void seek(std::size_t n) {
        if (n > remaining())
            pos_ = size_;
        else
            pos_ += n;
    }

    bool valid() const { return data_ != nullptr && pos_ <= size_; }
    void reset(const char* d, std::size_t sz) {
        data_ = d;
        size_ = sz;
        pos_ = 0;
    }

private:
    const char* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_ = 0;
};


template <typename T> struct always_false : std::false_type {};
template <typename T> constexpr bool always_false_v = always_false<T>::value;


// ── consteval bridge: runtime vector → compile-time array ─────

template <typename T> consteval std::size_t count_members_of() {
    using namespace std::meta;
    return nonstatic_data_members_of(^^T, access_context::unchecked()).size();
}

template <typename T> consteval auto get_data_members() {
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
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
}

template <typename T> void to_json_impl(std::string& out, const T& val);

template <typename T> std::string to_json(const T& val) {
    std::string out;
    to_json_impl(out, val);
    return out;
}

template <typename T> void to_json_impl(std::string& out, const T& val) {
    if constexpr (std::is_enum_v<T>) {
        out += std::to_string(static_cast<int>(val));

    } else if constexpr (is_std_string_v<T>) {
        out += '"';
        json_escape_to(out, val);
        out += '"';

    } else if constexpr (is_std_vector_v<T>) {
        out += '[';
        for (std::size_t i = 0; i < val.size(); ++i) {
            if (i > 0)
                out += ',';
            to_json_impl(out, val[i]);
        }
        out += ']';

    } else if constexpr (is_std_array_v<T>) {
        out += '[';
        for (std::size_t i = 0; i < val.size(); ++i) {
            if (i > 0)
                out += ',';
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
            if (!first)
                out += ',';
            first = false;
            out += '"';
            json_escape_to(out, identifier_of(m));
            out += '"';
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

template <typename T> void bin_write(Buffer& buf, const T& val) {
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

        buf.write(static_cast<std::uint32_t>(val.size()));
        for (auto& e : val)
            bin_write(buf, e);

    } else if constexpr (is_std_array_v<T>) {
        // std::array → elements (recursive, known count)
        for (auto& e : val)
            bin_write(buf, e);

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

template <typename T> void bin_read(BufferReader& reader, T& val) {
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

        auto sz = reader.read<std::uint32_t>();
        val.resize(sz);
        for (auto& e : val)
            bin_read(reader, e);

    } else if constexpr (is_std_array_v<T>) {
        // std::array → elements (recursive, known count)
        for (auto& e : val)
            bin_read(reader, e);

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

template <typename T> T bin_read(BufferReader& reader) {
    T val{};
    bin_read(reader, val);
    return val;
}

} // namespace aura::reflect

#endif // AURA_REFLECT_REFLECT_HH
