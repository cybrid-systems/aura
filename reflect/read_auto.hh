// ──────────────────────────────────────────────────────────────
//  read_auto.hh — Boilerplate reduction for ABF deserializer
//
//  Replaces hand-written read_xxx functions with template
//  instantiations. Defines common ABF read patterns as
//  template functions parameterized by node struct and tag.
//
//  Patterns:
//    read_value<T, Tag>(r, a)    → read one int64 value
//    read_name<T, Tag>(r, a)     → read one string
//    read_ptr<T, Tag>(r, a)      → read one Expr* node
//    read_name_value<T, Tag>(r, a) → read string + Expr*
//    read_name_value_body<T, Tag>(r, a) → read string + Expr* + Expr*
//
//  Each pattern directly constructs the node from the reader,
//  eliminating intermediate variables and move semantics.
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_READ_AUTO_HH
#define AURA_REFLECT_READ_AUTO_HH

#include <string>
#include <cstdint>

namespace aura::reflect::read {

// ── Bridge function pointers (set by module at init) ─────────
using ReadInt64Fn = std::int64_t (*)(void*);
using ReadStrFn   = std::string (*)(void*);
using ReadNodeFn  = void* (*)(void*, void*);

inline ReadInt64Fn  g_read_int64  = nullptr;
inline ReadStrFn    g_read_string = nullptr;
inline ReadNodeFn   g_read_node   = nullptr;

inline void set_bridges(ReadInt64Fn ri, ReadStrFn rs, ReadNodeFn rn) {
    g_read_int64 = ri; g_read_string = rs; g_read_node = rn;
}

// ── read_value: int64 value (LiteralIntNode) ─────────────────
// ABF format: empty body, node header already consumed.
// Value is read as 8-byte big-endian int64.
template <typename T, int Tag, typename Arena>
void* read_value(void* reader, Arena& arena) {
    auto val = g_read_int64(reader);
    return arena.template create<T>(T{{static_cast<decltype(T::tag)>(Tag)}, val});
}

// ── read_name: string name (VariableNode) ────────────────────
// ABF format: varint length + UTF-8 bytes
template <typename T, int Tag, typename Arena>
void* read_name(void* reader, Arena& arena) {
    auto name = g_read_string(reader);
    return arena.template create<T>(T{{static_cast<decltype(T::tag)>(Tag)}, std::move(name)});
}

// ── read_ptr: single Expr* (QuoteNode) ───────────────────────
// ABF format: recursively serialized child node
template <typename T, int Tag, typename Arena>
void* read_ptr(void* reader, Arena& arena) {
    auto* val = static_cast<Arena::ExprType*>(g_read_node(reader, &arena));
    T node{{static_cast<decltype(T::tag)>(Tag)}, val};
    return arena.template create<T>(std::move(node));
}
// Wait — Arena::ExprType doesn't exist. Use the struct directly.

// ── read_name_value: string + Expr* (DefineNode, SetNode) ────
template <typename T, int Tag, typename Arena>
void* read_name_value(void* reader, Arena& arena) {
    auto name = g_read_string(reader);
    auto* val = static_cast<typename std::remove_pointer<decltype(T::value)>::type*>(
        g_read_node(reader, &arena));
    using MemberType = decltype(T::value);
    T node{{static_cast<decltype(T::tag)>(Tag)}, std::move(name), static_cast<MemberType>(val)};
    return arena.template create<T>(std::move(node));
}

// ── read_body: named expr + body (LetNode pattern) ───────────
template <typename T, int Tag, typename Arena>
void* read_let(void* reader, Arena& arena) {
    auto name = g_read_string(reader);
    auto* val  = static_cast<decltype(T::value)>(g_read_node(reader, &arena));
    auto* body = static_cast<decltype(T::body)>(g_read_node(reader, &arena));
    T node{{static_cast<decltype(T::tag)>(Tag)}, std::move(name), val, body};
    return arena.template create<T>(std::move(node));
}

} // namespace aura::reflect::read

#endif
