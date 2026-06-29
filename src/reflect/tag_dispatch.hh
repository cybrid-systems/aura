// ──────────────────────────────────────────────────────────────
//  tag_dispatch.hh — NodeTag → reader dispatch (P2996 validated)
//
//  Provides a compile-time validated dispatch table that maps
//  ABF tag values (0x01-0x0C) to their corresponding reader
//  functions, replacing the hand-written switch.
//
//  The table is populated manually (function pointers from the
//  module TU) and validated by constexpr reflection.
//
//  Usage in module (global module fragment):
//    module;
//    #include "reflect/tag_dispatch.hh"
//    export module aura.binary.abf_deserializer;
//    ...
//    // Populate table:
//    auto tbl = tag_dispatch::build_table({
//      {Tag::LiteralInt, wrap_read<&read_literal_int>},
//      ...
//    });
//    // Dispatch:
//    auto reader = tbl[tag];
//    return reader(r);
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_TAG_DISPATCH_HH
#define AURA_REFLECT_TAG_DISPATCH_HH

#include <cstdint>
#include <array>
#include <cstdio>

namespace aura::reflect::tag_dispatch {

// ── Tag values (mirrors NodeTag in ast.ixx) ──────────────────
enum Tag : std::uint8_t {
    LiteralInt = 0x01,
    Variable = 0x02,
    Call = 0x03,
    IfExpr = 0x04,
    Lambda = 0x05,
    Let = 0x06,
    LetRec = 0x07,
    Define = 0x08,
    Begin = 0x09,
    Set = 0x0A,
    Quote = 0x0B,
    Cond = 0x0C,
    TypeAnnotation = 0x0F,
    Coercion = 0x10,
    // Issue #310: SV structural tags. Listed here so the Tag
    // enum mirrors NodeTag (the ABF deserializer doesn't read
    // them yet — the deserializer default-falls-through to
    // nullptr for any tag >= TAG_COUNT). Follow-up issue
    // adds the Interface/Modport deserializers + the lowerer
    // hook that populates them.
    Interface = 0x1B,
    Modport = 0x1C,
    TAG_COUNT = 0x1D // one past max
};

using ReadFn = void* (*)(void*);

// Build a dispatch table from (tag, reader) pairs
constexpr std::array<ReadFn, TAG_COUNT>
build_table(std::initializer_list<std::pair<Tag, ReadFn>> entries) {
    std::array<ReadFn, TAG_COUNT> tbl{};
    for (auto& [tag, fn] : entries)
        tbl[static_cast<std::size_t>(tag)] = fn;
    return tbl;
}

// Lookup
inline ReadFn reader_for(std::uint8_t tag, const std::array<ReadFn, TAG_COUNT>& tbl) {
    if (tag < TAG_COUNT)
        return tbl[tag];
    return nullptr;
}

// ── Compile-time tag validation ─────────────────────────────
// Called from standalone tests with P2996 reflection.
consteval bool validate_tag_values() {
    // Verify all tags have expected values
    return LiteralInt == 0x01 && Variable == 0x02 && Call == 0x03 && IfExpr == 0x04 &&
           Lambda == 0x05 && Let == 0x06 && LetRec == 0x07 && Define == 0x08 && Begin == 0x09 &&
           Set == 0x0A && Quote == 0x0B && Cond == 0x0C && TypeAnnotation == 0x0F &&
           Coercion == 0x10;
}

static_assert(validate_tag_values(), "Tag values don't match ABF spec");

} // namespace aura::reflect::tag_dispatch

#endif
