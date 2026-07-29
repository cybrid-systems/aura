// Wave C1: small sequential enum mirrors for name-table smoke tests.
// Keep ordinals in sync with production enums (module headers).

#ifndef AURA_REFLECT_ENUM_PODS_HH
#define AURA_REFLECT_ENUM_PODS_HH

#include <cstdint>

namespace aura::reflect::enum_pods {

// aura::ast::SyntaxMarker / HygieneMarker
enum class SyntaxMarker : std::uint8_t {
    User = 0,
    MacroIntroduced = 1,
    BoolLiteral = 2,
};

// aura::ir::Region
enum class Region : std::uint8_t {
    Default = 0,
    Performance = 1,
    Evolution = 2,
};

// aura::compiler::ComputeKind
enum class ComputeKind : std::uint8_t {
    Unknown = 0,
    Known = 1,
};

// aura::ir::IROpcodeClass (dense 0..)
enum class IROpcodeClass : std::uint8_t {
    Nop = 0,
    Const,
    Load,
    Arith,
    Compare,
    Logic,
    Control,
    Closure,
    Cell,
    Cast,
    String,
    Prim,
    Pair,
    Error,
    Hash,
    Linear,
    Arena,
    Guard,
};

// Subset of AuraErrorKind — first few sequential kinds for the table API.
// Full AuraErrorKind stays in error.ixx (module); name lookup for the full
// set can use build_name_table_extent once mirrored if needed.
enum class AuraErrorKindSample : std::uint8_t {
    ParseError = 0,
    UnexpectedToken,
    UnterminatedSExpr,
    UnboundVariable,
    DivisionByZero,
};

} // namespace aura::reflect::enum_pods

#endif // AURA_REFLECT_ENUM_PODS_HH
