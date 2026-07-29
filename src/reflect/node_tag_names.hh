// ──────────────────────────────────────────────────────────────
//  node_tag_names.hh — NodeTag display/ident names (meta-free)
//
//  Wave B1: single source for kNodeMeta[].name. Include from
//  module GMF (no <meta>). Indexed by `static_cast<size_t>(tag) - 1`
//  for tags in [0x01, 0x23]. Slot 0x0C (index 11) is the only
//  true hole ("<gap>"); MacroDef (0x0E) and DefineType (0x13) are
//  real tags (were incorrectly labeled gaps in older kNodeMeta).
//
//  P2996 alignment: tests/reflect/test_node_tag_align_b1.cpp.
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_NODE_TAG_NAMES_HH
#define AURA_REFLECT_NODE_TAG_NAMES_HH

#include <cstddef>
#include <string_view>

namespace aura::ast {

// Max NodeTag ordinal is Class = 0x23; table covers 0x01..0x23.
inline constexpr std::size_t kNodeTagNameCount = 0x23; // 35

inline constexpr std::string_view kNodeTagNames[kNodeTagNameCount] = {
    "LiteralInt",     // 0x01 index 0
    "Variable",       // 0x02
    "Call",           // 0x03
    "IfExpr",         // 0x04
    "Lambda",         // 0x05
    "Let",            // 0x06
    "LetRec",         // 0x07
    "Define",         // 0x08
    "Begin",          // 0x09
    "Set",            // 0x0A
    "Quote",          // 0x0B
    "<gap>",          // 0x0C — no enumerator
    "LiteralString",  // 0x0D
    "MacroDef",       // 0x0E
    "TypeAnnotation", // 0x0F
    "Coercion",       // 0x10
    "LiteralFloat",   // 0x11
    "Pair",           // 0x12
    "DefineType",     // 0x13
    "DefineModule",   // 0x14
    "Export",         // 0x15
    "Linear",         // 0x16
    "Move",           // 0x17
    "Borrow",         // 0x18
    "MutBorrow",      // 0x19
    "Drop",           // 0x1A
    "Interface",      // 0x1B
    "Modport",        // 0x1C
    "Property",       // 0x1D
    "Sequence",       // 0x1E
    "Assert",         // 0x1F
    "Covergroup",     // 0x20
    "Coverpoint",     // 0x21
    "Constraint",     // 0x22
    "Class",          // 0x23
};

inline constexpr std::size_t kNodeTagGapIndex = 11; // 0x0C

} // namespace aura::ast

#endif // AURA_REFLECT_NODE_TAG_NAMES_HH
