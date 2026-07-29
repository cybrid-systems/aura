// Wave B1: NodeTag mirror for P2996 (non-import-std / -freflection).
// Ordinals MUST match aura::ast::NodeTag in ast.ixx.

#ifndef AURA_REFLECT_NODE_TAG_POD_HH
#define AURA_REFLECT_NODE_TAG_POD_HH

#include <cstdint>

namespace aura::ast_pod {

enum class NodeTag : std::uint32_t {
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
    // 0x0C — no enumerator (true gap)
    LiteralString = 0x0D,
    MacroDef = 0x0E,
    TypeAnnotation = 0x0F,
    Coercion = 0x10,
    LiteralFloat = 0x11,
    Pair = 0x12,
    DefineType = 0x13,
    DefineModule = 0x14,
    Export = 0x15,
    Linear = 0x16,
    Move = 0x17,
    Borrow = 0x18,
    MutBorrow = 0x19,
    Drop = 0x1A,
    Interface = 0x1B,
    Modport = 0x1C,
    Property = 0x1D,
    Sequence = 0x1E,
    Assert = 0x1F,
    Covergroup = 0x20,
    Coverpoint = 0x21,
    Constraint = 0x22,
    Class = 0x23,
};

inline constexpr std::uint32_t kNodeTagMax = 0x23;
inline constexpr std::size_t kNodeTagEnumeratorCount = 34; // 35 slots - 1 gap

} // namespace aura::ast_pod

#endif // AURA_REFLECT_NODE_TAG_POD_HH
