// ──────────────────────────────────────────────────────────────
//  abf_serialize.hh — P2996-driven ABF deserialization helpers
//
//  Uses GCC 16.1 compile-time reflection to auto-generate
//  ABF read functions for simple AST node types, reducing
//  the hand-written read_xxx boilerplate.
//
//  For each node struct, reflect_members<T>() generates a
//  constexpr array of member metadata (name, offset, type).
//  read_struct<T>(reader) uses compile-time type dispatch
//  to read each member and construct the node.
//
//  Supported members: int64_t, std::string, Expr* (recursive)
//  Complex members (vector, params): handled manually.
//
//  Inclusion: designed for module global module fragment.
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_ABF_SERIALIZE_HH
#define AURA_REFLECT_ABF_SERIALIZE_HH

#include <meta>
#include <string>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace aura::reflect::abf {

// ── Member kind classification ───────────────────────────────
enum class MemberKind : std::uint8_t {
    Tag,        // NodeTag — skip during read
    Int64,      // std::int64_t
    String,     // std::string
    ExprPtr,    // Expr* — recursive read_node call
    ExprVector, // std::vector<Expr*>
    StringVec,  // std::vector<std::string>
    Unknown,
};

consteval MemberKind classify(std::meta::info type) {
    using namespace std::meta;
    if (is_same_type(type, ^^std::int64_t))  return MemberKind::Int64;
    if (is_same_type(type, ^^std::string))    return MemberKind::String;
    if (is_same_type(type, ^^std::size_t))    return MemberKind::Int64;
    // Can't reflect Expr* from a module-imported type,
    // so we detect via display_string_of
    auto disp = display_string_of(type);
    if (disp.ends_with("Expr*"))              return MemberKind::ExprPtr;
    if (disp.find("vector<Expr*") != std::string_view::npos) return MemberKind::ExprVector;
    if (disp.find("vector<string") != std::string_view::npos) return MemberKind::StringVec;
    // NodeTag is an enum with uint32_t underlying
    if (is_enum_type(type))                   return MemberKind::Tag;
    return MemberKind::Unknown;
}

// ── Member reflection ────────────────────────────────────────

struct MemberInfo {
    std::string_view name;
    std::ptrdiff_t   offset;
    MemberKind       kind;
};

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
        auto m = vec.data()[i];
        result[i] = MemberInfo{
            .name   = identifier_of(m),
            .offset = offset_of(m).bytes,
            .kind   = classify(type_of(m))
        };
    }
    return result;
}

// ── Forward: read_node function pointer ──────────────────────
// Set by the user at module init time. This bridges the
// reflection code (non-module) with the ABF deserializer (module).
using ReadNodeFn = void* (*)(void* reader, void* arena);

inline ReadNodeFn g_read_node = nullptr;

inline void set_read_node_fn(ReadNodeFn fn) { g_read_node = fn; }

// ── Generic struct reader ────────────────────────────────────
// Reads members from ABF stream using type-specific dispatch.
// The 'tag' member is skipped (ABF header handles it).
// Expr* members call g_read_node recursively.

template <typename T>
void* read_struct(void* reader_ptr, void* arena_ptr) {
    using namespace std::meta;

    // We need to construct T and set its members.
    // Since T might be defined in a module, we use mem-level ops.
    // T must be trivially movable.
    alignas(T) unsigned char buf[sizeof(T)];
    T* node = new (buf) T{};

    constexpr auto members = reflect_members<T>();
    auto* reader = reader_ptr;

    // ReadMember is a function pointer: reads from stream, writes to offset
    // We define it as a typed lambda dispatch

    for (auto& m : members) {
        if (m.kind == MemberKind::Tag) continue;  // skip tag

        auto addr = reinterpret_cast<char*>(node) + m.offset;

        switch (m.kind) {
        case MemberKind::Int64: {
            // read_int64_be from reader
            // Reader is opaque, call via function pointer
            // For now, assume we can read int64 via memcpy
            int64_t val = 0;
            // TODO: call reader's read_int64_be
            // This needs a function pointer bridge too
            break;
        }
        case MemberKind::String: {
            // read_string from reader
            break;
        }
        case MemberKind::ExprPtr: {
            // recursive: g_read_node
            if (g_read_node) {
                auto* expr = g_read_node(reader, arena_ptr);
                *reinterpret_cast<void**>(addr) = expr;
            }
            break;
        }
        default: break;
        }
    }

    return node;
}

} // namespace aura::reflect::abf

#endif
