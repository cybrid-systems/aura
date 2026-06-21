// Shared wire-layout struct for NodeView reflection tests.
// Must stay in sync with aura::ast::NodeView (src/core/ast.ixx).
#pragma once

#include <cstdint>
#include <span>

enum class WireNodeTag : std::uint32_t {
    LiteralInt = 0x01,
    Variable = 0x02,
    Call = 0x03,
    Let = 0x06,
};

enum class WireSyntaxMarker : std::uint8_t {
    User = 0,
    MacroIntroduced = 1,
};

struct NodeViewWire {
    std::uint32_t id = ~0u;
    WireNodeTag tag = WireNodeTag::LiteralInt;
    std::int64_t int_value = 0;
    double float_value = 0.0;
    std::uint32_t sym_id = ~0u;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::uint32_t type_id = 0;
    std::span<const std::uint32_t> children;
    std::span<const std::uint32_t> params;
    std::span<const std::uint32_t> param_annotations;
    WireSyntaxMarker marker = WireSyntaxMarker::User;
};